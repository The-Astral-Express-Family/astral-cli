# astral-cli

[![CI](https://github.com/The-Astral-Express-Family/astral-cli/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/The-Astral-Express-Family/astral-cli/actions/workflows/ci.yml)

`astral` —— Astral 服务的 C++ 命令行客户端。跨平台（Windows / macOS / Linux，x64 + arm64），
同时服务 Human 与 Agent 两类调用者：默认输出面向人，`--json` 提供稳定的机器契约。

> 架构事实来源见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。当前状态：v0.1 ——
> auth 登录链路（device flow / whoami / logout / init 绑定）已接通协议快照 v1；
> 任务/消息等业务命令与 SSE 随后续轮次落地。

## 功能速览

```text
astral login <server_url>       # 设备码登录：拉起浏览器审批，轮询换取 token 对（已可用）
astral whoami [server_url]      # 当前登录身份（401 自动惰性刷新一次，已可用）
astral logout [server_url]      # 服务端登出 + 清除本地会话（已可用）
astral init <url>[/<ws>] [path] # 绑定工作区（--create 可创建；--rebind 换绑，已可用）
astral todo / tags / msg ...    # 规划中（--json 契约已固定）
astral doctor                   # 本地体检 + 服务端连通性探测（已可用）
astral version                  # 版本信息（已可用）
```

- Human 输出带颜色（遵循 `NO_COLOR`、TTY 检测）；`--json` 模式 stdout 恒为单个 JSON 对象，进度/诊断走 stderr。
- 退出码稳定：`0` 成功、`1` 一般失败、`2` 用法错误、`3` 鉴权失败、`4` 未找到、`5` 冲突、`6` 网络、`7` 超时、`8` 本地工作区错误、`9` 协议不兼容。
- 凭证存为普通 JSON 文件 `~/.astral-cli/credentials.json`（双槽：human 会话按 server URL、
  agent credential 按 `server_id`，0600 权限，原子写入，modulator TODO §11 D12）——不依赖
  keyring，三端一致，`cat` 可查、`cp` 可备份；`ASTRAL_TOKEN` 优先于该文件且不落盘。

## 环境要求

| 工具 | 版本 | 说明 |
| --- | --- | --- |
| CMake | ≥ 3.25 | 支持 CMake Presets v8 |
| Ninja | 任意近期版本 | 所有 preset 使用 Ninja 生成器 |
| 编译器 | GCC 11+ / Clang 14+ / MSVC 19.30+ | 需支持 C++20 |
| [vcpkg](https://vcpkg.io) | 最新 | manifest 模式管理依赖 |

安装 vcpkg（一次性）：

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics        # Windows: bootstrap-vcpkg.bat
export VCPKG_ROOT="$HOME/vcpkg"                    # 建议写入 shell 配置
```

依赖（`vcpkg.json`，baseline 已锁）：CLI11、libcurl、nlohmann/json、spdlog、fmt；测试附加 Catch2。

## 构建与测试

```bash
cmake --preset dev            # Debug + 单测，依赖经 vcpkg 拉取
cmake --build --preset dev    # 首次会编译依赖，之后增量很快
ctest --preset dev            # 运行单测

cmake --preset release && cmake --build --preset release   # 发布构建（无测试）
cmake --preset asan && cmake --build --preset asan         # AddressSanitizer + UBSan（gcc/clang）
```

| Preset | 用途 |
| --- | --- |
| `dev` | 日常开发（Debug、测试开启） |
| `release` | 发布构建（Release、无测试） |
| `ci` | CI 同款（Release、测试、`-Werror`） |
| `asan` | `dev` + ASan/UBSan |
| `system` | Linux 免 vcpkg，用系统库快速构建（需自行装齐依赖） |

可选开关：`-DASTRAL_ENABLE_INTEGRATION_TESTS=ON`（需 `ASTRAL_TEST_SERVER` 指向活服务器）、
`-DASTRAL_ENABLE_CONTRACT_TESTS=ON`（协议快照契约测试）、`-DASTRAL_ENABLE_CLANG_TIDY=ON`。

试运行：

```bash
./build/dev/astral version
./build/dev/astral doctor
./build/dev/astral doctor --json
```

## Git 钩子（commitlint）

轻量级 [Conventional Commits](https://www.conventionalcommits.org/zh-hans/) 校验，纯 POSIX sh，
Linux / macOS / Windows（Git 自带 sh）通用：

```bash
sh scripts/setup.sh          # Windows 亦可: powershell scripts/setup.ps1
```

之后每次 `git commit` 都会校验提交信息（`feat(todo): ...` ✅ / `bad message` ❌）。
紧急绕过：`git commit --no-verify`。

提交类型：`feat` `fix` `docs` `style` `refactor` `perf` `test` `build` `ci` `chore` `revert` `protocol`
（`protocol` 用于协议快照同步提交）。

## CI / CD（GitHub Actions）

- **CI**（`.github/workflows/ci.yml`）：提交信息校验 → clang-format 检查 → 4 平台矩阵构建 + 单测 + 冒烟
  （ubuntu-x64/arm64、macos-arm64、windows-x64），Linux 额外跑协议契约测试。
  （macos-13/x64 已移除：该 runner 长期排队超 24h 必被取消，Intel 包待交叉编译方案。）
- **Release**（`.github/workflows/release.yml`）：打 `v*` 标签触发，4 目标产物打包（tar.gz/zip）+
  SHA256SUMS + GitHub Release。Linux 产物以 ubuntu-24.04 为 glibc 基线。

## 目录结构

```text
src/
├─ app/        # main、CLI11 接线、全局选项、错误→退出码映射
├─ commands/   # 每个顶层名词一个模块；Command 基类 + registry
├─ client/     # HTTP（libcurl 封装）与 SSE 帧解析
├─ auth/       # token 解析策略（ASTRAL_TOKEN 优先于凭证文件）
├─ workspace/  # .astral/config.json 绑定、init 简写解析、目标解析优先级
├─ output/     # Human/JSON 双输出、颜色、TTY 检测
├─ platform/   # OS 差异集中地：~/.astral-cli 用户目录、凭证文件存储、浏览器唤起
└─ core/       # 错误码、退出码、日志、版本、环境变量
tests/         # unit / integration / contract 三层
protocol/      # astral-modulator 协议冻结快照（contract tests 输入）
scripts/       # commitlint、git 钩子、环境安装
```

分层规则（详见架构文档 §14）：命令层不得直接调用 libcurl 或平台凭证 API，
必须经过 `client/`、`auth/`、`platform/` 接口。

## License

见 [LICENSE](LICENSE)。
