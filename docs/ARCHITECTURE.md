# Astral CLI Architecture

> 状态：Accepted baseline for v0.1
>
> 仓库：`The-Astral-Express-Family/astral-cli`
>
> 本文是 CLI 侧架构事实来源。服务端、公网 API 与数据模型以 `astral-modulator` 仓库为准。

## 1. 仓库职责

`astral-cli` 只负责客户端、Agent 可调用命令、多平台构建与分发，不再和服务端源码放在同一个仓库。

本仓库负责：

- `astral` C++ CLI；
- Human/Agent 命令 UX；
- HTTP/SSE 客户端；
- 每台服务器独立登录状态；
- 本地 Workspace 绑定；
- 本地凭证文件（`~/.astral-cli/`）读写与多服务器管理；
- Human/JSON 输出；
- Windows、macOS、Linux 构建、签名、Release 与包管理器分发；
- 对 `astral-modulator` 公网协议做兼容测试。

本仓库不负责：

- 服务端业务状态；
- 用户、Workspace、Task、Tag 最终授权；
- Token 签发与撤销规则；
- Web GUI；
- PostgreSQL schema；
- Agent 推理；
- Git 托管。

## 2. 双仓库边界

```text
astral-modulator
  server + web + PostgreSQL migrations
  canonical OpenAPI / JSON Schema
  auth / workspace / todo / tag / memory / audit
          ^
          | HTTPS REST/JSON + SSE
          v
astral-cli
  C++ CLI + local binding + credentials file
  release / package managers / Agent JSON contract
```

协议由服务端仓库拥有。CLI 不通过 git submodule 共享源码，也不依赖服务端 Go package。

CLI 只依赖公开协议版本。`astral-modulator` 发布协议变更时，应同步 OpenAPI/Schema 快照或发布 protocol artifact；CLI CI 针对固定协议版本运行 contract tests。两个仓库可以独立发版，不能假设 SemVer 一一对应。

## 3. CLI 技术基线

- C++20；
- CMake + CMake Presets；
- vcpkg manifest mode；
- CLI11；
- libcurl；
- nlohmann/json；
- spdlog；
- Catch2；
- 凭证通过 `platform/credential_store` 统一接口封装，默认实现为 `~/.astral-cli/credentials.json` 文件（不依赖 OS keyring）。

原则：

- 不为小功能引入 Boost 全家桶；
- 不自行实现 TLS 或密码学原语；
- 不依赖 Node/Python/Java Runtime；
- 平台差异只进入 `platform/`；
- 核心命令支持 `--json`；
- 非 TTY 不进行隐式交互。

## 4. 顶层命令

v0.1 采用下面这组主入口：

```text
astral login <server_url>      # 已实装
astral logout <server_url>     # 已实装
astral whoami [<server_url>]   # 已实装
astral profile show/set        # 已实装（round 29，GET/PATCH /auth/me）
astral init <server_url>[/<workspace_name>] [path]   # 已实装

astral workspace ...           # 桩（规划中）
astral todo ...                # 已实装（协议 v2）
astral tags ...                # 已实装（round 19）
astral status ...              # 桩（规划中）
astral msg ...                 # 已实装（round 19）
astral document ...            # 已实装（manifest/get/push/delete + conflicts list/show/resolve）
astral event ...               # listen 已实装（round 22，SSE 流式 + 断线续传）
astral agent ...               # 桩（规划中）
astral doctor                  # 已实装
astral version                 # 已实装
```

`astral login` 是正式入口，不再要求用户记 `astral auth login --server ...` 这类长写法。

`astral profile`（round 29）消费快照 v2.1 的 actor 资料契约：`show` 渲染
GET /auth/me 的 Me envelope（email 仅 human），`set` 以部分更新语义调
PATCH /auth/me——只序列化给出的字段，bio/avatar_url 空串=清除，空
display_name 本地即拒。鉴权走 §6.3 Bearer 策略（ASTRAL_TOKEN 优先），
因此 agent 凭证可以用它自管 display_name/bio/avatar_url。server 解析为
positional > `--server` > repo 绑定 > `ASTRAL_SERVER`。

`astral document`（phase-5 第一层，消费快照 v2.2 的 documents 七端点）是
单文档的命令面，不承担批量同步引擎（`astral sync` 是下一层，见 §11 尾）：

```text
astral document manifest [--include-deleted] [--all]   # 受管清单（path 升序游标）
astral document get <path> [--raw]                     # 读取；--raw 只输出正文（脚本模式）
astral document push <path> (--file <f>|'-'|--body <t>) [--base-revision N] [--base-hash H]
astral document delete <path> [--base-revision N]      # tombstone 删除（禁止盲删）
astral document conflicts list [--status open|resolved|all]
astral document conflicts show <id>                    # 双方全文对比
astral document conflicts resolve <id> --resolution ours|theirs|merged|manual [--file|--body]
```

关键语义（与 modulator sync-semantics / openapi 对齐）：

- **content_hash 口径**：`sha256:<64 小写 hex>`，基于**原始 UTF-8 bytes**
  （不重写换行，CRLF 原样进 hash 与 JSON body）；本地与远端不一致时 push
  必被 400 拒——双端对接最易错点。内容须过 UTF-8 结构校验（超长编码/
  代理区/截断序列本地即拒，exit 2），上限 1MiB。
- **base 指针**：push/delete 缺省先 GET 当前行作 base（读改写，claim/done
  同款节拍）；显式 `--base-revision 0` = 创建/复活（tombstone 行 push
  base 0 即复活）；`--base-revision >0` 必须带 `--base-hash`。404 或
  tombstone 都归零为 base 0。
- **Idempotency-Key**：push 携带确定性 key，**只由命令行可观测输入派生**——
  缺省（GET-改-写）模式 = `doc-` + fnv1a(path|content_hash)；显式
  `--base-revision` 时才纳入 base（命令行完整决定基准，重跑同样稳定）。
  重跑同一命令重放首次 2xx（E2E 验证：三连推 revision 稳定）。key 若随
  GET 到的动态 base 变化，重跑会同内容连续 bump，远端被他人改动时还会
  落下**不可收回的伪冲突工件**。注意服务端只缓存 2xx：重跑**本就冲突**
  的命令会累积冲突工件，sync 引擎层需按 path+base 去重提示。
- **409 DOCUMENT_CONFLICT**：错误 envelope 的 `details.conflict_id` 被提升
  进错误消息（`astral document conflicts show <id>` 提示），exit 5；
  `--json` 仍透传 `error.code=DOCUMENT_CONFLICT` + request_id/retryable。
- **路径编码**：按 `/` 分段、段内 urlEncode、`/` 字面保留；本地做结构
  校验（空段/`..`/反斜杠/控制字符/超长），保留前缀黑名单与大小写冲突
  （R1 `path_case_collision`）等权威判定留给服务端。

所有需要网络的命令必须能够从 Workspace 绑定或显式参数中确定服务器。无法确定时立即报错，不猜服务器。

## 5. Server Discovery

登录、初始化、兼容性检查先访问：

```text
GET <server_url>/.well-known/astral
```

返回至少包含：

```json
{
  "server_id": "srv_...",
  "canonical_url": "https://astral.example.com",
  "api_base": "/api/v1",
  "protocol_version": 2,
  "min_cli_protocol_version": 2,
  "auth": {
    "device_login": true
  }
}
```

`server_id` 是服务器稳定身份；URL 只是入口地址。凭证以 `server_id` 为主键保存，避免同一服务经多个域名访问时生成重复账户状态。

CLI 必须校验 TLS。生产环境不提供持久化全局 `insecure=true`。开发环境若需要跳过校验，只允许显式单次参数，并给出醒目告警。

## 6. `astral login <server_url>`

### 6.1 目标

登录针对“服务器”，不针对某个 Workspace。

同一台机器可以同时登录：

```text
https://astral.company-a.example
https://astral.lab.example
http://localhost:8080
```

每个服务端维护独立凭证。Workspace 切换不会复制或移动登录凭证。

### 6.2 Human 登录流程

```text
astral login https://astral.example.com
  -> discover server
  -> 查询本机凭证文件（~/.astral-cli/credentials.json）
  -> 若现有 session 可刷新，校验身份后直接成功
  -> 否则 POST /api/v1/auth/device/authorizations
  -> 输出 verification URL + user code，并尝试打开系统浏览器
  -> CLI 轮询授权结果
  -> 获得 access token + rotating refresh token
  -> 写入本机凭证文件（0600，原子替换）
  -> GET /api/v1/auth/me 校验
```

密码、OIDC 登录、Passkey 等人类认证全部发生在浏览器侧。CLI 不读取账户密码。

`--json` 或非 TTY 模式不得偷偷打开浏览器或等待人工输入。机器调用若没有可用凭证，返回稳定错误：

```text
AUTH_REQUIRED
```

### 6.3 Agent/CI

Agent 不共享 Human refresh token。自动化环境优先使用服务端签发的 Agent/Service Credential：

```text
ASTRAL_TOKEN=...
```

`ASTRAL_TOKEN` 只影响当前进程，不落 Workspace，也不写入凭证文件。

后续可增加 `--token-stdin`，但禁止把 token 设计为普通命令行位置参数，以免进入 shell history 与进程列表。

## 7. 本地凭证存储

凭证保存为用户目录下的普通 JSON 文件：

```text
~/.astral-cli/credentials.json        # Windows: %USERPROFILE%\.astral-cli\credentials.json
```

**设计取舍：不使用 OS keyring**（Windows Credential Manager / DPAPI、macOS Keychain、
Linux Secret Service）。理由：

- 可用性与开发效率优先于对抗本机恶意软件的威胁模型；
- 三端路径与行为完全一致，`cat` 即可查看、`cp` 即可备份，调试直观；
- 不引入平台依赖、DBus 会话、权限弹窗，headless/CI 开箱即用。

文件格式（version 2，D12 分槽：human 会话按 canonical server URL 键、
agent credential 按 `server_id` 键，互不混淆；servers 槽的生产消费方
尚未接线，`ASTRAL_TOKEN` 环境变量优先且不落盘）：

```json
{
  "version": 2,
  "sessions": {
    "https://astral.example.com": {
      "server_id": "srv_01…",
      "api_base": "/api/v1",
      "principal_id": "usr_01…",
      "access_token": "…",
      "refresh_token": "…"
    }
  },
  "servers": {
    "srv_01…": {
      "principal_id": "agt_01…",
      "access_token": "…",
      "refresh_token": "…"
    }
  }
}
```

约束与行为：

- 写入原子（temp + rename），POSIX 上文件权限 `0600`；Windows 依赖用户
  profile 目录 ACL；
- 凭证绝不进入 Git 仓库、`.astral/`、普通日志或 crash dump（本文件在
  `~/.astral-cli/`，天然在 repo 之外）；
- 文件损坏时读取报 `CREDENTIAL_STORE_ERROR` 并提示重新登录；`save`
  容错重建，所以 `astral login` 本身就是修复手段；
- `ASTRAL_TOKEN` 依然优先于该文件，且绝不落盘。

接口保持抽象（`platform/credential_store`），默认实现为上述文件；若未来
需要 keyring，可作为同一接口的可选后端叠加，不影响命令层。

## 8. Workspace 本地绑定

### 8.1 目录内容

每个已初始化 repo/目录仅创建一个项目配置文件：

```text
<project>/
  .astral/
    config.json
```

v0.1 不在 `.astral/` 中写 `state.json`、cache、token 或临时同步文件。需要缓存时写入用户级 cache 目录，并使用 `server_id + workspace_id + project path fingerprint` 隔离。

`.astral/config.json` 不含秘密，可以提交 Git，使 clone 下来的项目继续指向同一 Astral Workspace；每个开发者仍需用自己的本机凭证登录。

格式：

```json
{
  "version": 1,
  "server": {
    "id": "srv_01...",
    "url": "https://astral.example.com"
  },
  "workspace": {
    "id": "ws_01...",
    "name": "astral-modulator",
    "slug": "astral-modulator"
  }
}
```

其中 `server.id`、`workspace.id` 是权威标识；URL、name、slug 用于发现、可读输出与恢复。

### 8.2 为什么使用 `.astral/config.json`

不采用单个根文件 `.astral`，原因是目录名更容易和现有工作区概念对应，也为未来增加非敏感 lock/metadata 留出空间；但 v0.1 明确只写 `config.json` 一项，避免旧设计中 `state/cache` 散落进项目。

选择 JSON 而不是 TOML，是因为 CLI 已使用 nlohmann/json，无需再为一个小配置文件加入解析依赖。

## 9. `astral init`

正式语法：

```text
astral init <server_url>[/<workspace_name>] [path]
```

同时保留无歧义写法：

```text
astral init <server_url> [path] --workspace <workspace_name>
```

`path` 默认当前目录。

### 9.1 server/workspace 简写解析（实况，round 11 起）

v0.1 的 init **不做两阶段 discovery 回退**，要求显式给出 workspace 名，杜绝
「URL 最后一段到底是子路径还是 workspace 名」的歧义：

1. 若提供 `--workspace`，第一个位置参数整段视为 server URL；
2. 否则拆出最后一个 path segment 作 workspace 名候选（仅一段时）；
3. 两者皆无 → LOCAL_WORKSPACE_ERROR，提示 `<server>/<workspace>` 写法。

URL 先做规范化（scheme/host 小写、去根尾斜杠），保证 URL 比较与凭证主键稳定；
服务器部署在子路径时必须用 `--workspace` 消歧：

```text
astral init https://example.com/astral/my-repo        # server=/astral, ws=my-repo
astral init https://example.com/astral --workspace my-repo
```

### 9.2 默认 Workspace 名

**未实装**（规划项）：按 Git repo remote/目录名推导 workspace 名候选。
当前没有 workspace 名时 init 直接报错，不做任何猜测。

### 9.3 初始化状态机（实况）

```text
parse <server_url>[/<workspace_name>] (+ --workspace)
  -> require workspace name (no guessing; else LOCAL_WORKSPACE_ERROR)
  -> discover server (well-known + protocol version gate)
  -> requireSession: credentials file human session (no login invocation;
       AUTH_REQUIRED if absent)
  -> resolve workspace by exact name (?name= lookup)
       found: workspace = items[0]
       missing + --create: POST /workspaces (409 -> name taken)
       missing without --create: WORKSPACE_NOT_FOUND (hints --create)
  -> GET /workspaces/{id} verify visibility (non-member -> 404)
  -> inspect existing .astral/config.json
       same binding: idempotent success ("Already bound")
       different binding: fail (hint --rebind)
       --rebind: replace
  -> atomic write .astral/config.json
```

登录失败/未登录时绝不写半成品绑定。workspace 创建永不静默发生：
非交互模式必须显式 `--create`（TTY 交互式确认未实装）。

### 9.4 典型用法

```text
astral init https://astral.example.com/backend        # 绑定当前目录到 backend
astral init https://example.com/astral/backend ../backend   # 指定本地目录
astral init https://example.com/astral --workspace backend  # 子路径消歧
astral init https://astral.example.com/todo --create  # 不存在则创建
```

## 10. Workspace 解析优先级

网络命令按下面顺序确定目标：

1. 命令显式 `--server` / `--workspace`；
2. 当前目录向上查找最近的 `.astral/config.json`；
3. 环境变量 `ASTRAL_SERVER` / `ASTRAL_WORKSPACE`；
4. 用户级默认值；
5. 无法唯一确定则失败。

安全上不允许“最近登录服务器”在无提示情况下覆盖 repo 内绑定。

`ASTRAL_TOKEN` 只替换凭证来源，不替换 server/workspace 选择。

## 11. TODO 与 Tag CLI

服务端 Task 是事实来源，CLI 提供用户友好命令：

```text
astral todo list
astral todo add "..." --parent <task-id>
astral todo show <task-id>
astral todo claim <task-id>
astral todo done <task-id>
astral todo search --regex <expr> --fuzzy <text>
```

当 `--regex` 与 `--fuzzy` 同时存在时，语义固定为：

```text
regex filter -> fuzzy ranking
```

v2 容器化语义（round 16，协议快照 v2）：`todo list` 列出容器子任务——默认
workspace 根层，`--parent <task-id>` 切到该任务的 children 集合（只回传直接
子层，行内带 tags/children_count）；`todo add --parent <task-id>` 投递进该
任务的 children 集合（服务端 TaskCreate 已无 parent_id 字段）；`todo search`
走 `/workspaces/{id}/task-search` 平面查询，regex/fuzzy 与
tag/status/assignee 平权（至少一个条件）。

`claim`/`done` 的乐观并发（round 14 实装语义）：不传 `--revision` 时 CLI 先
GET 任务当前 revision 再提交（读改写窗口由服务端 409
`REVISION_CONFLICT`/`TASK_ALREADY_CLAIMED` 兜底）；传 `--revision` 则跳过
读取、原样提交。`list`/`search` 分页：默认单页，`--all` 跟随
`next_cursor` 取尽；`--json` 输出单对象（含 `items` 与最终
`next_cursor`），非流式 JSON Lines。目标解析（server/workspace）遵循
第 10 节优先级；鉴权遵循 §6.3：`ASTRAL_TOKEN` 优先，否则 human 会话槽 +
单次惰性刷新（D13）。

Tag/msg 命令已实装（round 19）。Tag 采用“两步确认”，减少 Agent 随手制造
重复 Tag；服务端校验 confirm code 与 actor/workspace/action/name 的绑定并
检查过期/单次使用状态（拼写统一为 `--confirm`）。rename/delete 先经 tag
词典按名解析 `target_tag_id`（`tag_` 前缀参数直接作 id）。服务端只做规范化
后的精确重名约束，不用模糊相似度自动拒绝；语义近似判断仍交给调用者。

```text
astral tags create urgent
#   -> propose：返回 proposal_id + confirm_code + 全量 existing_tags，
#      人读输出附带完整可复制命令行
astral tags create urgent --proposal tgp_01… --confirm K7P4Q2
#   -> confirm：单次使用，缺一即 USAGE 错误
```

`msg send` 目标语法：`workspace`（广播）| `actor:<actor_id>` |
`task:<task_id>`（任务线程），`--thread <msg_id>` 跟进；发送携带确定性
Idempotency-Key（内容 FNV-1a），重跑同一命令服务端 24h 内重放首次 2xx
不双发。`msg list --task <task_id>` 走任务线程集合端点。
`event listen` 消费 workspace SSE 流（round 22）：stdout 每事件一行
（--json 输出原始 envelope JSON Lines；人读模式输出 `时间 类型 ID`），
断线/关流按指数退避重连并以 Last-Event-ID 续传，snapshot.required 后
自动丢弃过期游标，401 经一次 lazy refresh 重放；403/404 等终态按协议
错误码退出。`--max-events N` 消费满即干净退出（不含控制事件）。

Phase-5 分层：`astral document`（见 §4）是第一层单文档命令面；第二层
`astral sync`（FR-010 pull/push/sync 引擎：本地扫描 → manifest 分类 →
安全拉推 → 冲突汇聚，含受管路径 include/exclude 约定——round 38 R3
裁决该约定归本仓持有）尚未动工，不承诺任何本地 state 文件布局。

## 12. Human 输出与 Agent 输出

默认输出面向人：

```text
Bound backend -> https://astral.example.com (ws_01…)
```

`--json`：

- stdout 只输出一个 JSON object，流式命令使用 JSON Lines；
- progress、诊断、日志进入 stderr；
- 不输出 ANSI 文本；
- 错误通过稳定 `error.code` 表达；
- 不输出 token secret。

`--json` 错误 envelope（round 14 起补齐）：CLI 本地错误输出
`{"error": {"code", "message"}}`（code 为 CLI 本地码，如
`LOCAL_WORKSPACE_ERROR`）；来自服务端协议 envelope 的失败则透传冻结契约
`{"error": {"code", "message", "request_id", "retryable"}}`，其中 `code` 为
服务端稳定码（如 `TASK_ALREADY_CLAIMED`），退出码由 HTTP 状态映射
（401/403→3、404→4、409→5、5xx→6、其余 4xx→9）。两类失败都可直接按
`error.code` 分支。

建议顶层退出码：

```text
0 success
1 generic failure
2 invalid CLI usage
3 auth/authz failure
4 not found
5 conflict/precondition failure
6 network/server unavailable
7 timeout
8 local workspace error
9 incompatible protocol/client
```

## 13. HTTP/SSE Client

`client/` 只实现公开协议。已实装：

- HTTPS REST/JSON（libcurl）；
- Bearer Token；
- timeout；
- **CLI 身份头（R2 版本协商，phase-5 轮起）**：全部 `/api/v1` 请求经
  `auth::stampClientHeaders` 携带 `X-Astral-Client: cli` 与
  `X-Astral-Client-Version: <kProtocolVersion>`（业务命令、whoami/init、
  device flow 轮询、token refresh 全覆盖）；发现门（§5）按
  `min_cli_protocol_version` 下限自查——高于本 CLI 的 protocol_version
  只要下限覆盖就兼容，服务端对低版本头的 400
  `CLIENT_VERSION_UNSUPPORTED` 经既有错误映射落 exit 9。

**未实装**（规划项，勿当现状依赖）：请求级 request ID 头、bounded retry、
bounded retry（`event listen` 已具备退避重连 + Last-Event-ID 续传；
其余命令为单次调用）。业务命令的写请求已在应用层携带确定性
Idempotency-Key（msg send、document push）。

401 处理顺序（已实装，`withLazyRefresh`）：

1. 若凭证文件中的 refresh session 可用，尝试一次 refresh；
2. 原请求重放一次；
3. 仍失败则返回 auth error；
4. 不进入无限 refresh/retry 循环。

## 14. 内部分层

```text
/
├─ ARCHITECTURE.md
├─ README.md
├─ CMakeLists.txt
├─ CMakePresets.json
├─ vcpkg.json
├─ cmake/
├─ src/
│  ├─ app/
│  ├─ commands/        # 扁平文件，一个顶层名词一个 <noun>_cmd.cpp
│  │  ├─ login_cmd.cpp
│  │  ├─ init_cmd.cpp
│  │  ├─ todo_cmd.cpp / tags_cmd.cpp / msg_cmd.cpp / document_cmd.cpp / profile_cmd.cpp（随实装增加）
│  │  └─ registry.cpp
│  ├─ client/
│  ├─ auth/
│  ├─ workspace/
│  ├─ output/
│  ├─ platform/
│  └─ core/
├─ tests/
│  ├─ unit/
│  ├─ integration/
│  └─ contract/
├─ protocol/
└─ .github/workflows/
```

命令层不得直接调用平台 Credential API 或 libcurl；统一经过 `auth/`、`client/` 与 `platform/` 接口，便于测试。

## 15. 用户目录

项目目录只保存 `.astral/config.json`。

用户级数据三端统一放在 `~/.astral-cli/`（Windows 用 `%USERPROFILE%\.astral-cli\`），
不再按平台分散到 XDG / ~/Library / AppData：

```text
~/.astral-cli/
  credentials.json    # 登录凭证（见第 7 节，0600）
  config.json         # 用户级非敏感设置（最近访问服务器等，未来）
  cache/              # 用户级缓存
```

路径规则在 `platform/user_dirs` 集中实现；若用户设置 `ASTRAL_HOME`，以它为准。

## 16. 三端发行

GitHub Actions 负责：

- Windows x86_64；
- macOS arm64（x86_64 已移除：macos-13 runner 长期排队，Intel 包待交叉编译方案）；
- Linux x86_64 + arm64；
- Release Asset；
- SHA-256 checksum；
- stable release 签名；
- 安装 smoke test。

包管理器分阶段支持 Homebrew、Scoop/WinGet、deb/rpm。直接下载版本不默认静默自更新。

Linux 需要明确最低 glibc baseline；如确有需求再增加独立 musl 产物，不为了“一个二进制通吃”提前增加工具链复杂度。

## 17. 测试要求

至少覆盖：

- server URL canonicalization；
- server/workspace shorthand 解析；
- Git repo 默认 Workspace 名推导；
- `init` 幂等、rebind、半失败不落盘；
- Credential Store mock；
- token refresh/revoke；
- JSON stdout/stderr 契约；
- regex + fuzzy 参数序列化；
- tag proposal/confirm；
- document content_hash 口径（NIST 向量 + CRLF 不重写）、base 指针解析、
  409 冲突提示、空串选项语义（--body ""/--bio ""）；
- SSE reconnect；
- Windows/macOS/Linux build smoke；
- 与固定 `astral-modulator` protocol snapshot 的 contract test。

## 18. 已明确不做

v0.1 不做：

- CLI 内嵌密码登录；
- Workspace 内保存 token；
- repo 内 `.astral/state.json` / cache；
- Git submodule 共享服务端源码；
- 自动模糊创建 Workspace；
- Tag 语义相似度自动拦截；
- CLI 自己决定最终授权；
- 默认后台 daemon；
- 全局持久 `--insecure`。

## 19. 关键决策摘要

1. **双仓库**：CLI 生命周期、依赖和发行链与服务端分离。
2. **登录按服务器存储**：Workspace 只是授权域，不应制造一份凭证副本。
3. **`.astral/` 只放一个非敏感 `config.json`**：repo 绑定可随 Git 流转，凭证留在用户目录 `~/.astral-cli/credentials.json`。
4. **`init` 可触发登录**：减少第一次使用步骤，但只在交互式 Human 场景发生。
5. **ID 权威、name 辅助**：重命名 Workspace 不破坏本地绑定。
6. **协议而非源码耦合**：两个 repo 独立演进，只通过版本化 HTTP 契约连接。
