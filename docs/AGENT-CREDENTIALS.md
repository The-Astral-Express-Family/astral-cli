# Agent Credential CLI 签发入口设计与实施计划

> 状态：设计定稿、未实施（2026-09-26 规划轮产出）。目标：转正
> `astral agent` 桩（§4），给 agent credential 补上 CLI 侧的签发/撤销/
> 本地落盘入口，并接通 credentials.json 双槽中至今无生产者也无消费方
> 的 servers 槽（ARCHITECTURE §7 D12）。
>
> 契约依据（modulator，快照 v2.3 已含）：`POST /agents/{agent_id}/
> credentials`、`DELETE /agents/{agent_id}/credentials/{credential_id}`、
> `GET|POST /workspaces/{id}/agents`。服务端**无新增需求**（仅补 openapi
> 错误码行，见 §6）。

## 1. 问题

- 签发 agent credential 目前只有 Web GUI（MembersView）一个入口；无
  浏览器环境（纯 SSH/自动化）的 owner 无法签发。
- CLI 凭证文件双槽设计里 servers 槽（按 server_id 键控 agent
  credential）从未接线：无生产者（doctor 6846353 为此把计数改成只看
  sessions 槽）、也无消费方（`ApiSession::send` 从不读它）。
- 撤销需要 credential_id，而服务端无凭证列表端点——不本地留痕就只能
  手抄 id。

## 2. 契约事实（实装即契约，均已在服务端落地）

| 事实 | 出处 |
|---|---|
| 签发 body：`{scopes[]必填, expires_at? RFC3339, workspace_id?}`；201 → `{credential_id, secret, scopes}`，**secret 明文仅此一次**，格式 `astral_<43字符>`，服务端只存 sha256 | openapi L1182/L1972-2001 |
| 授权分流（G4）：`workspace_id` 非空 → 该 ws 的 `agent:manage`（owner/maintainer）；空/缺省 → `platform:credentials:manage`（仅平台 admin） | workspace/module.go L544-555 |
| scopes 必须在服务端白名单（workspace 轴 20 个；`platform:*` **不在白名单**——平台能力只随平台角色，不可经 credential 授予） | auth/scopes.go |
| 撤销 DELETE → 204；**幂等语义是 404**（"not found or already revoked"）；撤销联动服务端断开该 actor 的 SSE 连接 | workspace/module.go L601-647 |
| 全局 Idempotency 中间件覆盖 credential create（同 key 24h 重放首次 2xx） | architecture §21 |
| agent actor 创建：`POST /workspaces/{id}/agents {display_name, kind}` | openapi L1152-1180 |

## 3. 命令面（转正 `astral agent`）

registry 现桩词 `{list, register, revoke}` 调整为 `{list, create, issue,
revoke}`——`register` 让位给顶层 `astral register`（human 邀请注册，
3822b29 已落地），agent 侧建号用 `create` 对齐契约 `AgentCreate`。

| 命令 | 端点 | 行为要点 |
|---|---|---|
| `astral agent list` | GET `/workspaces/{ws}/agents` | 分页透传；列 id/kind/display_name（后续命令按 id 引用） |
| `astral agent create <display_name> [--kind agent\|service]` | POST `/workspaces/{ws}/agents` | 201 打印/透传 Agent（含新 id）。**不做「创建即签发」隐式链**（与 Web 的两步流不同：CLI 组合显式、可脚本化，id 可先审后用） |
| `astral agent issue <agent_id> [--scope S]... [--ttl 7d\|30d\|90d\|never] [--save] [--workspace ...] [--global] [--json]` | POST `/agents/{id}/credentials` | 见 §4 |
| `astral agent revoke <agent_id> <credential_id>`（或 `--saved`） | DELETE `/agents/{id}/credentials/{cred_id}` | 见 §5 |

workspace 解析沿用 `auth::openWorkspace`（flag > repo 绑定 > env >
用户级默认）；`--global` 跳过 ws 解析只发 server（body 整体省略
`workspace_id` 字段，走平台 admin 分支）。

## 4. issue 的关键裁决

1. **scope 缺省 = D9 agent bundle**（与 Web DEFAULT_SCOPES 同款：contributor
   一族，不含 agent:manage / workspace:write / manage_members / audit:read）；
   `--scope` 可重复、给即覆盖缺省；未知 scope 由服务端 400 兜底（exit 9）。
2. **TTL 缺省 `never`**（对齐服务端无 expires_at 语义）；`--ttl` 换算
   RFC3339（now+N）。运维收紧留给签发者自觉 + 后续 modulator §22 高风险
   审批面（触发式）。
3. **Idempotency-Key = `cred-` + fnv1a(agent_id \| sorted(scopes) \|
   expires_at)**（照 msg/document 既有派生范式）：网络重试不双发；
   同参数重跑在 24h 窗口内**重放同一签发响应（同一 secret）**——语义
   写入 README，重签请改 scope/ttl。
4. **secret 输出例外条款（修订 ARCHITECTURE §12「不输出 token secret」）**：
   签发命令的 secret 就是本次请求的有效载荷——human 模式打印到 stdout
   （credential_id + secret + 「只此一次，丢失只能撤销重签，请立即交给
   使用方（ASTRAL_TOKEN 或 Bearer）」）；`--json` 模式 secret 进 stdout
   单对象 `{"credential_id", "secret", "scopes", "saved": bool}`。例外
   仅此命令；secret 永不进 stderr/日志/其他命令输出。
5. **`--save` 可选而非默认**：多数签发场景 secret 应走安全通道交给别的
   机器/CI（不落本机）；`--save` 供本机同时充当 agent（开发/单机双角色）
   时写入 servers 槽（§5），默认打印-only 完全不落盘。

## 5. servers 槽接线（生产者 + 消费方一起，不留半成品）

- **schema version 3**（向后兼容读 v2）：`servers[server_id] = {
  principal_id(agt_xxx), credential_id, access_token(secret),
  refresh_token: ""}`——新增 `credential_id` 字段（撤销留痕），字段复用
  不改结构骨架，`writeFileAtomic + 0600` 沿用。
- **生产者**：`agent issue --save`（server_id 取自 discovery，与 human
  session 同源）。
- **消费方**：`ApiSession::send` 凭证选择链定为
  `ASTRAL_TOKEN env（最高、进程级、不落盘）→ human session（现状不变）
  → servers 槽（按目标 server 的 server_id）`。即：无 human 登录但槽内
  有该 server 的 agent credential 时，命令以 agent 身份执行；whoami 等
  身份命令输出标注身份来源（human/agent credential）。
- **doctor**：恢复双槽计数（sessions + servers 分行列出）。6846353 的
  单槽过渡版就此退役。
- **revoke --saved**：从槽读 credential_id 发 DELETE，成功（204 或 404
  幂等）后从槽中移除该条目（本地状态与服务端对齐）。

## 6. revoke 语义与 404 特判

- DELETE 204 → 「已撤销（服务端已同步断开其 SSE 连接）」exit 0；
- **404 特判为幂等达成**：人读「凭证不存在或已撤销（幂等达成）」，
  `--json` 输出 `{"revoked": true, "already": true}`，exit 0。这是对
  §12 通用 404→exit 4 映射的显式例外（服务端幂等语义即 404，见 §2），
  例外登记进 §12 清单（register 的 RegistrationRejected 同款方式）。
- 服务端无列表端点 → 撤销凭本地留痕（--saved）或手输 credential_id
  兜底；**不做服务端 list 扩展**（本地留痕已闭环；真实多端管理需求
  出现时再评估，modulator §3 触发式同款理由）。

## 7. 服务端配套（实施时一并，纯契约文档级）

- openapi 给两 credential 端点补错误码行（现缺）：POST 400
  VALIDATION_FAILED（unknown scope / bad expires_at）、403
  INSUFFICIENT_SCOPE、404 NOT_FOUND；DELETE 404（幂等语义注记）。
  G4 先例（TODO.md §9 L361）同款登记。
- 协议快照无需新版本（端点与 schema 已在 v2.3），错误码行属文档补齐，
  随下次快照刷新携带。

## 8. 测试计划（FakeApi，路由按 uses 消费）

- issue：成功（body 断言 workspace_id 分流字段 / scopes / RFC3339 /
  幂等 key 存在）、`--global` 省略字段、`--scope` 覆盖缺省 bundle、
  unknown scope 400→exit 9、403→exit 3、secret 出现在 --json stdout、
  `--save` 落槽（v3 形状断言）。
- create/list：201/分页透传/`--kind`。
- revoke：204、404 特判（exit 0 + already: true）、`--saved` 从槽取 id
  并清理槽。
- 消费链：无 human session + 槽有 credential → 请求带 Bearer secret、
  身份输出标 agent；ASTRAL_TOKEN 仍压过槽。
- doctor：双槽计数断言。

## 9. 实施切分

- [ ] **P1 签发面（无本地状态）**：`agent list/create/issue`（打印-only）
  + §12 secret 例外条款 + ARCHITECTURE §4 桩转正 + openapi 错误行 +
  README 命令清单。验收：§8 前五组测试 + CI 绿。
- [ ] **P2 槽接线**：schema v3 + `--save` + 消费链 + doctor 双槽 +
  `revoke`（含 404 特判与 `--saved`）。验收：§8 后四组测试 + CI 绿；
  E2E 手动：issue --save → 登出 human → todo list 以 agent 身份跑通 →
  revoke --saved → 再跑 todo list 得 401。
- [ ] **P3（触发式）**：服务端凭证列表端点评估（多端管理需求出现时）。
