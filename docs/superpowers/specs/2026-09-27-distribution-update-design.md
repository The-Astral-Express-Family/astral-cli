# astral-cli 分发与自更新设计

- 日期：2026-09-27
- 状态：已获用户逐节确认（§1–§5），待实施
- 范围：release 版本烙入、GitHub Actions 发布流水线增强、`astral update` 自更新命令、跨平台安装脚本

## 0. 背景与目标

`astral-cli`（C++20 / CMake / vcpkg）已有 5 平台 release 流水线（tag `v*` 触发，产出
`astral-<ver>-<target>.tar.gz|zip` + `SHA256SUMS.txt` 并创建 GitHub Release），但存在缺口：

1. release 二进制内未烙入版本号（本地构建 git describe 为 `unknown`）；
2. 无 `astral update` 自更新能力；
3. 无安装脚本，README 仅覆盖源码构建。

目标：**GitHub Actions 自动 release + `astral update` 自更新 + 一键安装脚本**的完整分发闭环。

已确认的关键决策：

| 决策点 | 结论 |
| --- | --- |
| 仓库可见性 | public（匿名访问 GitHub API 与资产下载，无需 token） |
| 安装位置 | POSIX：`~/.local/bin`；Windows：`%LOCALAPPDATA%\Programs\astral` |
| 版本策略 | tag（`vX.Y.Z` semver）驱动，单渠道，只认 GitHub Release |
| 被动更新通知 | 不做（无后台检查，只有主动 `astral update`） |
| 版本切换 | `astral update [vX.Y.Z]` 支持指定版本，允许降级 |
| 完整性校验 | 默认启用 SHA256，不做开关 |
| 解压方案 | libarchive（vcpkg），提取 tar.gz / zip |
| SHA256 | picosha2（header-only） |
| 代理 | 依赖 libcurl 默认行为，识别 `HTTP_PROXY`/`HTTPS_PROXY`/`NO_PROXY`/`ALL_PROXY` |

## 1. 版本烙入（semver 驱动 release）

### 编译期注入

- `src/core/version.hpp.in` 新增 `@ASTRAL_EMBED_VERSION@` 占位符，生成逻辑加入 CMake：
  - release workflow 注入 `-DASTRAL_EMBED_VERSION="${GITHUB_REF_NAME}"`（如 `v0.2.0`）；
  - 本地 / 普通 CI 构建回退为 `project VERSION`（无 `v` 前缀时补 `v`）。
- 烙入值统一为 `v` 前缀 semver 字符串。

### `astral version` 输出

- human：`astral v0.2.0 (v0.2.0-4-g1a2b3c, linux/x86_64, protocol 1)`（括号内保留 git describe）；
- `--json`：新增 `"embedVersion"` 字段（加字段属兼容变更）。

### semver 比较

- 剥 `v` 前缀后按 major.minor.patch 数值比较，手写（约 20 行，nlohmann/标准库即可），不引新依赖。

### tag / project version 一致性

- `project VERSION` 随发版手动 bump；release workflow 在构建前校验 tag `vX.Y.Z` 与
  `project VERSION X.Y.Z` 一致，**不一致直接 fail**（暴露发版流程失误，不做 auto-fix）。

## 2. Release 流水线变更（`release.yml`）

1. 新增前置校验 job：tag 与 `CMakeLists.txt` 的 `project VERSION` 一致性；失败信息提示
   “先 bump CMakeLists.txt 再打 tag”。
2. 各平台 configure 追加 `-DASTRAL_EMBED_VERSION="${GITHUB_REF_NAME}"`。
3. `scripts/install.sh` 与 `scripts/install.ps1` 一并作为 release 资产上传（与二进制版本绑定的快照）。
4. 资产命名约定不变：`astral-<ver>-<target>.tar.gz|zip`；平台映射由 `uname -s`/`uname -m`
   （脚本）与 `buildPlatform()`（CLI）分别推导：`linux-x64`、`linux-arm64`、`macos-x64`、
   `macos-arm64`、`windows-x64`。
5. 主推安装入口为 raw 引用（脚本随 main 即时生效）：
   `curl -fsSL https://raw.githubusercontent.com/The-Astral-Express-Family/astral-cli/main/scripts/install.sh | sh`
6. **不**发布 `version.json` 之类的版本清单（`releases/latest` API 已含全部信息，避免双源）。

## 3. `astral update` 命令（内置自更新）

### CLI 形态

```
astral update            # 更新到最新 release
astral update v0.2.1     # 安装指定版本（允许降级）
astral update --check    # 只查询，报告最新版本，不安装
```

### 流程

1. **自省定位**：解析 `argv[0]` 为绝对路径；仅当位于约定安装目录（POSIX `~/.local/bin`、
   Windows `%LOCALAPPDATA%\Programs\astral`）或其子目录时允许自更新，否则退出码 8 并提示
   “非脚本安装，请用安装脚本或手动下载”。开发构建（`build/dev/astral`）同样不可自更新。
2. **查询版本**：`GET /repos/<owner>/<repo>/releases/latest`（指定版本走 `/releases/tags/<tag>`），
   解析 `tag_name` 与资产列表；本地 == 远端 → “已是最新”，退出码 0。
3. **下载**：资产包 + `SHA256SUMS.txt` 到 `~/.astral-cli/tmp/update-XXXX/`；复用 `HttpClient`，
   天然继承 proxy 环境变量行为。
4. **校验**：SHA256SUMS 行匹配后比对 picosha2 计算值；失配 → 删除临时目录并以新退出码 10 失败。
5. **替换（rename 舞步）**：
   - POSIX：新二进制先落同目录 `astral.new`，`rename()` 原子替换；
   - Windows：先把运行中的 exe `MoveFileEx` 挪为 `astral.old`（REPLACE_EXISTING），再移入新文件；
     残留 `astral.old` 由下次 update 惰性清理。
6. **收尾**：清理临时目录；human 打印 `astral v0.2.1 → v0.3.0 installed`；`--json` 输出
   `{command: "update", from, to}`。

### 网络端点与 HTTP 行为

- 端点仅 `api.github.com` 与 `objects.githubusercontent.com`（302 跳转后），均为 HTTPS；
- `HttpClient` 需开启 `CURLOPT_FOLLOWLOCATION`（当前未开），并带 `Accept: application/vnd.github+json`。

### 新增依赖（vcpkg.json）

- `libarchive`（解压 tar.gz / zip，仅提取目标二进制，不执行包内任何内容、不恢复可执行位之外的属性）；
- `picosha2`（SHA-256）。

## 4. 安装脚本

### `scripts/install.sh`（POSIX sh，Linux/macOS 通用）

```
curl -fsSL https://raw.githubusercontent.com/The-Astral-Express-Family/astral-cli/main/scripts/install.sh | sh
ASTRAL_VERSION=v0.2.1 sh install.sh        # 指定版本
ASTRAL_INSTALL_DIR=/custom/bin sh install.sh
```

1. `uname -s` + `uname -m` → 资产三元组；不支持的平台明确报错并列出支持清单；
2. GitHub API 查询目标版本 → 下载 tar.gz + SHA256SUMS 到 `mktemp -d`；
3. `sha256sum`（Linux）/ `shasum -a 256`（macOS）校验；
4. 解压并安装到 `~/.local/bin/astral`（`chmod +x`，`mv -f` 覆盖旧版，幂等，重装语义）；
5. PATH 检测：`~/.local/bin` 不在 PATH → 打印一行含可复制 export 语句的提示，**不自动改 rc 文件**；
6. 依赖仅 `curl` + `tar` + `sha256sum`/`shasum`；proxy 环境变量由 curl 天然继承。

### `scripts/install.ps1`（Windows x64）

1. 同流程：下载 zip + SHA256SUMS → `Get-FileHash` 校验 → `Expand-Archive` → 安装到
   `%LOCALAPPDATA%\Programs\astral\astral.exe`；
2. **自动追加用户级 PATH**（仅追加不重排），装完提示重开终端 —— 与 POSIX 的有意不对称：
   Windows 用户 PATH 追加是安装器平台惯例且非破坏性，而 `~/.local/bin` 在多数发行版默认生效；
3. 覆盖正在运行的 `astral.exe` 走与 §3 相同的 rename 舞步；
4. Windows arm64：明确报“暂不支持”。

## 5. 错误处理、退出码与测试

### 退出码（README 同步更新）

| 场景 | 退出码 | 语义 |
| --- | --- | --- |
| 网络失败 / GitHub API 5xx | 6 | 现有 NETWORK_ERROR |
| 超时 | 7 | 现有 TIMEOUT |
| 不在可自更新位置 / 临时目录不可写 | 8 | 现有本地错误语义扩展 |
| 指定版本不存在（tag 404） | 4 | 现有 NOT_FOUND |
| **checksum 失配** | **10（新增 UpdateIntegrity）** | 供应链红线，独立语义 |
| 资产中找不到本平台文件 | 4 | NOT_FOUND + 明确消息 |

`Errc` 枚举新增 `UpdateIntegrity = 10`；`--json` 错误对象结构不变。

### 安全边界

- TLS 校验恒开（无开关）；
- 临时目录用后即删；解压仅提取 `astral-*/astral(.exe)` 单文件，不执行包内任何脚本；
- human 模式下载前打印目标版本与资产名，可肉眼审计。

### 测试

1. **单元测试（Catch2）**：
   - semver 比较：`v0.2.0 < v0.2.1`、`v0.10.0 > v0.9.0`、`v` 前缀剥离、非法输入报错；
   - 平台映射全矩阵（`linux/x86_64 → linux-x64` 等 5 项）；
   - SHA256SUMS 行解析（空格 / 多空格 / `*` 二进制标记）；
   - `argv[0]` 解析与可自更新位置判定（注入假 HOME）。
2. **集成测试**（`ASTRAL_ENABLE_INTEGRATION_TESTS` 体系，标记网络依赖）：
   - 对真实 GitHub API：`update --check` 返回合法 semver tag。
3. **冒烟测试**（release.yml 扩展）：
   - `astral update --check --json` 输出含 `"latest"` 字段（CI 可联网）；
   - `astral version` 显示的烙入版本 == tag。
4. **手工验收清单**（三平台各一轮）：
   install → `astral version` → `astral update --check` → 指定版本降级 → 再升回。

### 明确不做

- 被动更新通知 / 后台检查；
- `version.json` 版本清单文件；
- Windows arm64 支持（遇 ARM64 明确报“暂不支持”）；
- 包管理器渠道（brew / winget / scoop，README 标注“计划中”）。

## 6. 涉及文件清单（实施预览）

| 文件 | 变更 |
| --- | --- |
| `src/core/version.hpp.in`、`src/core/version.cpp`、`src/commands/version_cmd.cpp` | 烙入版本 + 输出 |
| `src/core/version_semver.{hpp,cpp}`（新） | semver 解析比较 |
| `src/commands/update_cmd.{hpp,cpp}`（新） | update 命令 |
| `src/core/error.{hpp,cpp}`、`src/core/exit_codes.hpp` | `UpdateIntegrity = 10` |
| `src/client/http_client.{hpp,cpp}` | FOLLOWLOCATION、下载到文件 |
| `src/platform/` | 可自更新位置判定、rename 舞步、惰性清理 |
| `scripts/install.sh`、`scripts/install.ps1`（新） | 安装脚本 |
| `.github/workflows/release.yml` | 校验 job、版本注入、脚本上传 |
| `vcpkg.json`、`CMakeLists.txt`、`src/CMakeLists.txt` | 新依赖、版本注入逻辑 |
| `tests/` | 单测；`README.md` | 退出码表与安装说明 |
