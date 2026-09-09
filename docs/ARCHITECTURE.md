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
astral login <server_url>
astral logout <server_url>
astral whoami [<server_url>]
astral init <server_url>[/<workspace_name>] [path]

astral workspace ...
astral todo ...
astral tags ...
astral status ...
astral msg ...
astral document ...
astral event ...
astral agent ...
astral doctor
astral version
```

`astral login` 是正式入口，不再要求用户记 `astral auth login --server ...` 这类长写法。

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
  "protocol_version": 1,
  "min_cli_protocol_version": 1,
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

文件格式（按 `server_id` 为主键，一个文件管理多台服务器登录态）：

```json
{
  "version": 1,
  "servers": {
    "srv_01…": {
      "principal_id": "user_01…",
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

### 9.1 server/workspace 简写解析

由于服务端未来可能部署在 URL 子路径，不能简单把最后一个 `/` 永远当 Workspace 名。解析顺序：

1. 若提供 `--workspace`，整段第一个参数都视为 server URL；
2. 否则先把完整输入当 server URL 执行 discovery；
3. 若 discovery 失败，再拆最后一个 path segment 为 `workspace_name`，对剩余 URL 重试 discovery；
4. 两次 discovery 都失败则报 `SERVER_NOT_FOUND`。

因此：

```text
astral init https://astral.example.com/my-repo
```

通常解析为 server=`https://astral.example.com`、workspace=`my-repo`。

若服务器本身部署在 `/astral` 且 discovery 在完整 URL 成功，则不会误拆；需要同时指定 Workspace 时使用：

```text
astral init https://example.com/astral --workspace my-repo
```

### 9.2 默认 Workspace 名

未显式给 Workspace 时按下面顺序推导：

1. 当前 Git repo `remote.origin.url` 仓库 basename，去掉 `.git`；
2. Git repo 根目录 basename；
3. 待初始化目录 basename。

只用于“名称候选”，最终始终解析成服务端返回的 `workspace_id`。

### 9.3 初始化状态机

```text
resolve target path
  -> ensure path exists and is writable
  -> parse/discover server
  -> validate protocol compatibility
  -> load credential for server_id
  -> credential unavailable?
       TTY human: invoke same login flow as `astral login`
       non-TTY/--json: AUTH_REQUIRED
  -> resolve workspace by exact name/slug or explicit id
  -> workspace exists and accessible: continue
  -> workspace missing:
       TTY: ask whether to create
       non-TTY: require --create
  -> inspect existing .astral/config.json
       same binding: idempotent success
       different binding: fail WORKSPACE_ALREADY_BOUND
       explicit --rebind: replace after validation
  -> atomic write .astral/config.json
  -> GET workspace to verify final binding
```

`init` 可以顺手触发 Human 登录，但登录失败时绝不写半成品绑定。

Workspace 创建也不能因拼写错误默默发生。交互模式需要一次明确确认；机器模式必须显式 `--create`。

### 9.4 典型用法

```text
# 默认绑定当前目录，Workspace 名来自 Git repo
astral init https://astral.example.com

# 指定 Workspace
astral init https://astral.example.com/backend

# 指定本地目录
astral init https://astral.example.com/backend ../backend

# 无歧义写法
astral init https://example.com/astral ../backend --workspace backend
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

Tag 采用“两步确认”，用于减少 Agent 随手制造重复 Tag：

```text
astral tags create <tagname>
```

第一步只创建短期 proposal，并返回现有 Tag、proposal ID 与确认码，不创建正式 Tag。

Agent/Human 检查后执行：

```text
astral tags create <tagname> --confirm <code>
```

服务端校验 code 是否绑定当前 actor、workspace、动作和 tag name，并检查过期/单次使用状态。拼写统一为 `--confirm`。

Tag rename/delete 采用同类 proposal/confirm 流程。服务端只做规范化后的精确重名约束，不用模糊相似度自动拒绝；语义近似判断仍交给调用者。

## 12. Human 输出与 Agent 输出

默认输出面向人：

```text
Bound astral-modulator -> https://astral.example.com / astral-modulator
```

`--json`：

- stdout 只输出一个 JSON object，流式命令使用 JSON Lines；
- progress、诊断、日志进入 stderr；
- 不输出 ANSI 文本；
- 错误通过稳定 `error.code` 表达；
- 不输出 token secret。

`--json` 错误 envelope 的刻意子集（当前阶段）：CLI 本地错误输出
`{"error": {"code", "message"}}`；协议 envelope（error.schema.json）中的
`retryable`/`request_id` 待 HTTP client 接入命令层后补齐——`request_id`
需要透传响应头，`retryable` 需要与错误映射表对齐。补齐前不改动 error.code
语义，客户端可安全按 code 分支。

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

`client/` 只实现公开协议：

- HTTPS REST/JSON；
- Bearer Token；
- request ID；
- idempotency key；
- timeout；
- bounded retry；
- exponential backoff + jitter；
- SSE reconnect + last event cursor。

只自动重试安全读请求、幂等请求，或带服务端支持 `Idempotency-Key` 的写操作。

401 处理顺序：

1. 若使用凭证文件中的 refresh token 且 refresh session 可用，尝试一次 refresh；
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
│  │  ├─ todo_cmd.cpp / tags_cmd.cpp / msg_cmd.cpp（随实装增加）
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
- macOS arm64 + x86_64；
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
