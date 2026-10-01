# astral-cli 分发与自更新 — 实施计划

- 日期：2026-09-27
- 规格来源：`docs/superpowers/specs/2026-09-27-distribution-update-design.md`（已获用户确认）
- 执行方式：TDD，每个任务独立提交

## 目标

补齐分发闭环：release 二进制烙入 tag 版本 → `astral update` 自更新（含 SHA256 校验、指定版本降级）→ `curl | sh` / PowerShell 一键安装脚本 → release 流水线校验与增强。

## 全局约定与代码锚点

- 构建/测试：`cmake --preset dev && cmake --build --preset dev && ctest --test-dir build/dev --output-on-failure`（下文简写 `build+test`）。CI 预设 `ci` 带 `-Werror`，提交前跑一遍。
- 命令接口：`src/commands/command.hpp` — `Command`（`name/description/configure(CLI::App&)/execute(const CommandContext&)`）；工厂函数模式见 `doctor_cmd.hpp`（`makeDoctorCommand()`）。注册点：`src/commands/registry.cpp` `makeBuiltinCommands()`。
- 错误：`src/core/error.hpp` — `Errc` 枚举 + `AstralError`；字符串形式是稳定 JSON 契约（只增不改）。退出码：`src/core/exit_codes.hpp` `ExitCode`（0-9 已用）。映射逻辑在 `error.cpp`（`codeString()`/`exitCode()`，按现有 switch/表风格追加）。
- HTTP：`src/client/http_client.{hpp,cpp}` — `Options{connectTimeout, requestTimeout, userAgent, verifyTls}`、`send/get/postJson`；CURLOPT 集中在 `send()`（约 109-131 行）。**禁止设置 `CURLOPT_PROXY`**（保持 libcurl 环境变量代理默认行为，这是规格要求）。
- 版本链：根 `CMakeLists.txt` L38-47 算 `ASTRAL_GIT_DESCRIBE` → `src/CMakeLists.txt` L1 `configure_file(core/version.hpp.in ... generated/core/version.hpp @ONLY)` → `version.cpp` `versionString()`（现返回 `kProjectVersion`）/ `buildPlatform()`（`linux/x86_64` 等）。
- 用户目录：复用 `src/platform/user_dirs.hpp`（`~/.astral-cli`）。
- 单测：`tests/unit/test_*.cpp` + `tests/unit/CMakeLists.txt` 追加源文件；集成：`tests/integration/`（Catch2 SKIP 模式，见 `test_http_client.cpp`）。
- 新依赖统一走 vcpkg manifest（`vcpkg.json`），`find_package` + 链接在 `src/CMakeLists.txt`。
- 资产命名（release.yml 现状）：`astral-<ver-no-v>-<target>.tar.gz|zip`（tag 去 `v` 前缀），校验清单资产名 `SHA256SUMS.txt`。target 五元：`linux-x64/linux-arm64/macos-x64/macos-arm64/windows-x64`。
- 源码一律 ASCII 输出（用户可见消息用 `->` 不用 `→`，避免编码问题）。

## 任务清单

### 任务 1：semver 解析与比较模块

**新文件** `src/core/semver.hpp` / `semver.cpp`：

```cpp
namespace astral::core {
struct SemVer {
    int major = 0, minor = 0, patch = 0;
    // 接受 "v1.2.3" / "1.2.3"；要求三段数字，忽略尾部 prerelease/build（本仓库不发）。
    // 非法输入返回 std::nullopt。
    static std::optional<SemVer> parse(std::string_view text);
    std::strong_ordering operator<=>(const SemVer&) const = default;
};
}
```

- `parse`：剥前导 `v`/`V`，按 `.` 分三段，`std::from_chars` 全数字解析；多余段/非数字/空段 → nullopt。带 `-rc.1`/`+build` 后缀时截断忽略。

**测试** `tests/unit/test_semver.cpp`：
- `v0.2.0 < v0.2.1`；`v0.10.0 > v0.9.0`（字符串比较陷阱）；`v1.0.0 == 1.0.0`
- 非法：`""`、`"v"`、`"1.2"`、`"1.2.x"`、`"unknown"` → nullopt
- `1.2.3-rc.1` 解析为 `1.2.3`

**集成**：`src/CMakeLists.txt` 源列表加 `core/semver.cpp`；`tests/unit/CMakeLists.txt` 加测试文件。

**验证**：build+test。**提交**：`feat(core): add semver parse and compare`

### 任务 2：退出码 10 UpdateIntegrity

**修改**：
- `src/core/exit_codes.hpp`：`ExitCode` 枚举加 `UpdateIntegrity = 10`（注释：供应链校验失败）。
- `src/core/error.hpp`：`Errc` 加 `UpdateIntegrity`（字符串形式 `"UPDATE_INTEGRITY"`，加入稳定契约注释块）。
- `src/core/error.cpp`：按现有风格在 `codeString()`/`exitCode()` 映射中加条目（`UPDATE_INTEGRITY` → `ExitCode::UpdateIntegrity`）。

**测试**：`tests/unit/` 扩展（就近加入 `test_cli.cpp` 或新建 `test_error.cpp`）：`AstralError(Errc::UpdateIntegrity, "...")` 的 `codeString()=="UPDATE_INTEGRITY"`、`exitCode()==10`。

**验证**：build+test。**提交**：`feat(core): add UpdateIntegrity error code (exit 10)`

### 任务 3：版本烙入

**修改**：
- 根 `CMakeLists.txt`（`project()` 之后、`ASTRAL_GIT_DESCRIBE` 块附近）：
  ```cmake
  if(NOT DEFINED ASTRAL_EMBED_VERSION)
    set(ASTRAL_EMBED_VERSION "v${PROJECT_VERSION}")
  endif()
  ```
- `src/core/version.hpp.in`：加 `inline constexpr const char* kEmbedVersion = "@ASTRAL_EMBED_VERSION@";`
- `src/core/version.cpp`：`versionString()` 改返回 `kEmbedVersion`。
- `src/commands/version_cmd.cpp`：human 行改为 `astral <versionString()> (<git>, <platform>, protocol <n>)`（现为 `astral v0.2.0 (...)`，即用烙入值）；`--json` 在现有字段（`name/version/git/platform/protocolVersion`）基础上**新增** `"embedVersion": kEmbedVersion`（`version` 字段保持 `kProjectVersion` 不动，兼容契约；`embedVersion` 为 `"v0.2.0"` 形态）。

**测试**：扩展 `tests/unit/test_json_output.cpp`（或 version 相关用例）：`version --json` 含 `"embedVersion"` 键；本地构建值为 `"v0.1.0"`。

**验证**：build+test；`./build/dev/astral version` 人肉确认输出格式。**提交**：`feat(version): embed release version into the binary`

### 任务 4：分发纯逻辑（资产三元组 + SHA256SUMS 解析）

**新文件** `src/core/distribution.hpp` / `.cpp`：

```cpp
namespace astral::core {
// "linux/x86_64" -> "linux-x64"（五个目标平台；其余 -> nullopt）
std::optional<std::string> assetTripletFor(std::string_view buildPlatform);
// 拼资产文件名：stripV(tag) + "-" + triplet + (windows ? ".zip" : ".tar.gz")
// 如 tag "v0.2.0"、buildPlatform "linux/x86_64" -> "astral-0.2.0-linux-x64.tar.gz"
std::optional<std::string> assetNameFor(std::string_view tag, std::string_view buildPlatform);
struct ShaSum { std::string hex; std::string filename; };
// 解析 "abc123 *file" / "abc123  file"（任意空白、可选二进制标记 '*'）
std::optional<ShaSum> parseShaLine(std::string_view line);
}
```

**测试** `tests/unit/test_distribution.cpp`：三元组全矩阵 5 项 + `unknown` → nullopt；`assetNameFor("v0.2.0","windows/x86_64")` 得 `.zip`；`parseShaLine` 空格/多空格/`*` 标记/非法行（hex 非 64 位、空文件名）。

**验证**：build+test。**提交**：`feat(core): asset naming and SHA256SUMS parsing helpers`

### 任务 5：HttpClient 增强（重定向 / 下载到文件 / 自定义头）

**修改** `src/client/http_client.{hpp,cpp}`：
- `Options` 加 `bool followRedirects = true;` 与 `std::map<std::string, std::string> headers;`（默认空）；`send()` 中对应设 `CURLOPT_FOLLOWLOCATION`（1L）+ `CURLOPT_MAXREDIRS`（5）与 `CURLOPT_HTTPHEADER`（追加到现有头处理，不覆盖 userAgent/Host）。
- 新方法（错误语义与 `get()` 完全一致——若 `get()` 抛 `AstralError(NetworkError)` 则同样抛）：
  ```cpp
  // 流式下载响应体到文件（不占内存），供 update 下载资产用。
  Response getToFile(const std::string& url, const std::filesystem::path& destination);
  ```
  实现：`CURLOPT_WRITEDATA` 指向 `std::ofstream`（二进制）；HTTP 状态 ≥400 时按现有错误路径处理并删除半截文件。
- **不设任何 `CURLOPT_PROXY`**（保持环境变量代理：`HTTP_PROXY/HTTPS_PROXY/NO_PROXY/ALL_PROXY` 大小写变体由 libcurl 默认识别）。

**测试**：
- 单测：`Options` 默认值断言（`followRedirects == true`）。
- 集成 `tests/integration/test_http_client.cpp` 追加：对 `https://api.github.com`（或 `ASTRAL_TEST_SERVER`）GET，验证 `getToFile` 落盘且内容与 `get().body` 一致；标记 `[integration][net]`，沿用 SKIP 模式。

**验证**：build+test。**提交**：`feat(client): follow redirects, custom headers, streaming download`

### 任务 6：vcpkg 依赖 + 压缩包提取模块

**修改**：
- `vcpkg.json`：`dependencies` 加 `"libarchive"`、`"picosha2"`。
- `src/CMakeLists.txt`：`find_package(LibArchive REQUIRED)` + `find_package(picosha2 CONFIG REQUIRED)`；`astral_core` 链接 `LibArchive::LibArchive`、`picosha2`。

**新文件** `src/platform/archive.hpp` / `.cpp`：

```cpp
namespace astral::platform {
// 从 tar.gz / zip 中提取唯一的 astral 可执行成员（成员 basename == "astral"（POSIX）
// 或 "astral.exe"（Windows），包内目录前缀任意），写到 destDir 下同名文件并置 0755。
// 成员缺失/损坏 -> nullopt 且 error 有明确消息。绝不执行包内任何内容。
std::optional<std::filesystem::path> extractBinary(
    const std::filesystem::path& archive, const std::filesystem::path& destDir, std::string& error);
}
```

- libarchive 读 API（`archive_read_new` + format/filter 自动探测），逐条目 basename 匹配；写文件后 `std::filesystem::permissions(owner_all|group_exec|others_exec)` 近似 0755（Windows 忽略）。

**测试** `tests/unit/test_archive.cpp`：测试内用 libarchive **写 API** 现造 fixture（两个成员 `dir/astral` + `dir/README.md` 的 tar.gz；一个 zip 同理），断言提取出 `astral`、无 `README.md`、无匹配成员时 nullopt。

**验证**：build+test（vcpkg 首次装依赖会久，属预期）。**提交**：`feat(platform): archive extraction via libarchive`

### 任务 7：自更新平台层（exe 定位 / 位置判定 / 原子替换）

**新文件** `src/platform/self_update.hpp` / `.cpp`：

```cpp
namespace astral::platform {
// 当前可执行文件绝对路径：Linux /proc/self/exe；macOS _NSGetExecutablePath+realpath；
// Windows GetModuleFileNameW。（规格原文写 argv[0]，实施改用 OS API：argv[0] 可被
// 调用方伪造，OS API 是规范来源；此为有意偏差，已在此记录理由。）
std::filesystem::path currentExecutable();

// exe 位于 ~/.local/bin（POSIX）或 %LOCALAPPDATA%\Programs\astral（Windows）
// 目录本身或其子目录 -> true。HOME/LOCALAPPDATA 运行时读取（可测试性）。
bool isManagedInstallLocation(const std::filesystem::path& exePath);

// rename 舞步：replacement 先位于同目录临时名。
// POSIX: rename() 原子覆盖；Windows: MoveFileExW(target->target.old, REPLACE_EXISTING)
//        再 MoveFileExW(replacement->target)。失败 -> false 且 error 说明。
bool replaceExecutable(const std::filesystem::path& target,
                       const std::filesystem::path& replacement, std::string& error);

// 惰性清理：exe 同目录残留的 astral.old / astral.exe.old（上次更新遗留）。
void cleanupStaleUpdateFiles(const std::filesystem::path& exeDir);
}
```

**测试** `tests/unit/test_self_update.cpp`（env 变异用例：改前保存、用例末恢复；注释注明不可并行）：
- `isManagedInstallLocation`：设 `HOME=<tmp>`，`<tmp>/.local/bin/astral` → true；`<tmp>/.local/bin/sub/astral` → true；`build/dev/astral` → false；Windows 侧用 `LOCALAPPDATA` 同理（用 `#ifdef _WIN32` 分组）。
- `replaceExecutable`：tmp 目录造 `astral` + `astral.new`，替换后内容为新文件、无 `.new` 残留（Windows 上允许 `.old` 残留由 cleanup 收）。
- `cleanupStaleUpdateFiles`：预置 `astral.old` 后被清。

**验证**：build+test。**提交**：`feat(platform): self-update location check and atomic replace`

### 任务 8：GitHub Release 解析层

**新文件** `src/client/github_release.hpp` / `.cpp`：

```cpp
namespace astral::client {
struct ReleaseAsset { std::string name; std::string downloadUrl; };
struct ReleaseInfo { std::string tagName; std::vector<ReleaseAsset> assets; };
// 纯解析（可离线单测）：GitHub releases API JSON -> ReleaseInfo。
// 取 tag_name 与 assets[].{name, browser_download_url}。
ReleaseInfo parseReleaseJson(const std::string& json);

class GithubReleaseClient {
public:
    explicit GithubReleaseClient(HttpClient& http);
    // GET https://api.github.com/repos/<repo>/releases/latest（或 /releases/tags/<tag>）
    // 头：Accept: application/vnd.github+json；UA 见任务 5 Options。
    // 404 -> AstralError(Errc::ServerNotFound 或按现有语义最贴近 NotFound 的用法，
    //        消息 "version <tag> not found")；403 提示 rate limit；5xx/网络 -> NetworkError。
    ReleaseInfo latest(std::string_view repo);
    ReleaseInfo byTag(std::string_view repo, std::string_view tag);
};
}
```

- 仓库常量：`src/commands/update_cmd.cpp` 内 `constexpr const char* kUpdateRepo = "The-Astral-Express-Family/astral-cli";`。

**测试** `tests/unit/test_github_release.cpp`：内嵌两段真实 API 响应样本字符串（latest 与 tags 形态各一，可截短但字段完整），断言解析出的 tagName/assets 数量与 downloadUrl；坏 JSON 抛错。

**验证**：build+test。**提交**：`feat(client): GitHub release API client and parser`

### 任务 9：`astral update` 命令组装

**新文件** `src/commands/update_cmd.hpp` / `.cpp`：
- 工厂 `makeUpdateCommand()`；`configure()`：位置可选参数 `version`（`vX.Y.Z` 或 `X.Y.Z`，非法格式 `SemVer::parse` 失败 → `ExitCode::Usage` 消息 "invalid version format"）+ `--check` flag。
- **修改** `src/commands/registry.cpp`：`makeBuiltinCommands()` 注册（doctor/version 旁）。

`execute()` 流程（严格按规格 §3，含两处实施细化）：
1. 解析目标版本：无参数 → `latest()`；有参数（规范化为带 `v`）→ `byTag()`。
2. **`--check` 模式：跳过位置判定**（只报告不安装；CI/开发构建可用，冒烟测试依赖此行为）。输出：human `current: v0.2.1, latest: v0.3.0`；json `{"command":"update","check":true,"from":"v0.2.1","latest":"v0.3.0"}`。退出 0。
3. 本地 `SemVer::parse(kEmbedVersion)` vs 远端 tag：相等 → human `astral v0.2.1 is up to date`；json `{"command":"update","from":"v0.2.1","to":"v0.2.1"}`；退出 0（不下载）。
4. 安装路径前置检查：`currentExecutable()` 不满足 `isManagedInstallLocation` → `AstralError(Errc::LocalWorkspaceError)`，消息 `not installed via install script; download manually or run the install script`（exit 8）。先 `cleanupStaleUpdateFiles(exeDir)`。
5. 下载：`assetNameFor(tag, buildPlatform())` 在 `ReleaseInfo.assets` 找名字 + `SHA256SUMS.txt`，缺失 → `Errc::ServerNotFound` + 消息列出资产名（exit 4）。`getToFile` 到 `~/.astral-cli/tmp/update-<pid>/`（复用 user_dirs；RAII 结构函数末尾整目录删除）。
6. 校验：`parseShaLine` 找到资产行，`picosha2::hash256_hex_string` 对下载文件算值，小写比较；失配 → `Errc::UpdateIntegrity`，消息含期望/实际 hash（exit 10）。
7. 提取替换：`extractBinary` 到临时目录 → `replaceExecutable(exePath, extracted)`。
8. 成功输出：human `astral v0.2.1 -> v0.3.0 installed`；json `{"command":"update","from":"v0.2.1","to":"v0.3.0"}`。退出 0。
- HttpClient `Options`：`userAgent = "astral-cli/" + 去v版本`、`requestTimeout` 放宽到 300s（二进制下载）。

**测试**：
- 单测 `tests/unit/test_update_cmd.cpp`（不起网络）：非法版本参数 → Usage；`--check` 不做位置判定（mock 不了 HTTP 的部分不测，行为靠集成）。
- 集成 `tests/integration/test_update_check.cpp`：`[integration][net]`，对真实 `api.github.com` 跑 `--check` 路径（构造 `UpdateCommand` + `CommandContext{ostringstream}`），断言退出 0 且 `latest` 值 `SemVer::parse` 成功。无网环境 SKIP（沿用现有模式）。

**验证**：build+test + 集成（有网）。**提交**：`feat(commands): astral update self-update command`

### 任务 10：`scripts/install.sh`

**新文件**，POSIX sh（`#!/bin/sh`，`set -eu`）：
1. `uname -s`+`uname -m` → 三元组（Linux x86_64→linux-x64、aarch64→linux-arm64；Darwin x86_64→macos-x64、arm64→macos-arm64）；不支持 → stderr 列支持清单，exit 1。
2. 版本：`ASTRAL_VERSION`（可无 `v`，脚本规范化）否则 `curl -fsSL $API/repos/<repo>/releases/latest` 用 `sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p'` 提取。
3. 下载 `astral-<ver>-<triplet>.tar.gz` 与 `SHA256SUMS.txt` 到 `mktemp -d`；`trap` 退出清理。
4. 校验：`sha256sum` 优先，无则 `shasum -a 256`（macOS）；失配 exit 1 并打印期望/实际。
5. `tar -xzf` → `install -m 0755 <dir>/astral "$ASTRAL_INSTALL_DIR"`，默认 `$HOME/.local/bin`（目录不存在则 `mkdir -p`）。幂等覆盖。
6. PATH 检测：`case ":$PATH:" in *":$dir:"*)` 不在 → 打印一行含 `export PATH="$dir:$PATH"` 提示，**不改 rc 文件**。
7. 成功打印 `astral <ver> installed to <dir>`。
8. 内部测试钩子：`ASTRAL_API_BASE`（默认 `https://api.github.com`）与 `ASTRAL_DOWNLOAD_BASE`（默认 `https://github.com`）覆盖，供本地 file:// 验证与未来镜像。
- `<repo>` = `The-Astral-Express-Family/astral-cli`。

**验证**：`sh -n scripts/install.sh`；`shellcheck scripts/install.sh`（如装了）；本地用 `ASTRAL_API_BASE` 指向自造目录结构冒烟（无 release 时至少验证参数/平台错误分支）。**提交**：`feat(scripts): POSIX install script`

### 任务 11：`scripts/install.ps1`

**新文件**（PowerShell 5.1 兼容，`#Requires -Version 5.1`）：
1. `[Environment]::Is64BitProcess` 否则报错；`$env:PROCESSOR_ARCHITECTURE -eq 'ARM64'` → `throw 'ARM64 Windows is not supported yet'`。
2. 版本：`$env:ASTRAL_VERSION` 否则 `Invoke-RestMethod -Uri "https://api.github.com/repos/<repo>/releases/latest" -Headers @{Accept='application/vnd.github+json'}` 取 `.tag_name`。
3. 下载 `astral-<ver>-windows-x64.zip` + `SHA256SUMS.txt` 到临时目录（`[IO.Path]::GetTempPath()` 下建子目录，`try/finally` 清理）。
4. `(Get-FileHash -Algorithm SHA256).Hash` 与清单行比对（大小写不敏感）。
5. `Expand-Archive` → 目标 `$env:LOCALAPPDATA\Programs\astral\astral.exe`（`New-Item -Force` 建目录）。
6. 覆盖运行中 exe：若目标存在，`Move-Item -Force` 挪 `.old` 再移入新文件（成功后 `Remove-Item .old -ErrorAction SilentlyContinue`）。
7. PATH：读 `[Environment]::GetEnvironmentVariable('Path','User')`，不含目标目录则**仅追加**（不重排）写回；提示“重开终端生效”。
8. 成功打印版本与路径。

**验证**：`pwsh -NoProfile -Command "$null = [System.Management.Automation.Language.Parser]::ParseFile('scripts/install.ps1', [ref]$null, [ref]$err); $err"` 零错误（Linux 上可跑）。**提交**：`feat(scripts): Windows install script`

### 任务 12：release.yml 增强

**修改** `.github/workflows/release.yml`：
1. 新首个 job `verify-version`（`needs` 加进现有 build 链头部）：
   ```yaml
   verify-version:
     runs-on: ubuntu-24.04
     steps:
       - uses: actions/checkout@v4
       - name: Tag matches project version
         if: startsWith(github.ref, 'refs/tags/')
         run: |
           TAG="${GITHUB_REF_NAME#v}"
           PROJ="$(grep -m1 -oP 'VERSION \K[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt)"
           [ "$TAG" = "$PROJ" ] || { echo "::error::tag v$TAG != project VERSION $PROJ; bump CMakeLists.txt first"; exit 1; }
   ```
2. 各平台 `cmake --preset ci` 步骤追加注入（仅 tag 触发时；workflow_dispatch 保持默认嵌入）：
   `cmake --preset ci -DASTRAL_EMBED_VERSION="${GITHUB_REF_NAME}"` + `if: startsWith(github.ref, 'refs/tags/')`（另留无注入的兜底步骤或用 shell 条件表达式二选一，实施时取最简）。
3. Smoke 追加（现有 smoke 步骤旁）：
   - `./build/ci/astral version | grep -F "${GITHUB_REF_NAME}"`（tag 时）
   - `./build/ci/astral update --check --json | grep -F '"latest"'`
4. publish/release 步骤：资产列表加入 `scripts/install.sh`、`scripts/install.ps1`（与二进制同 release 上传）。

**验证**：`actionlint .github/workflows/release.yml`（如可装）或 YAML 解析人肉过一遍；逻辑靠任务 15 的发版演习最终验证。**提交**：`ci(release): version guard, embedded version injection, script assets`

### 任务 13：README 更新

**修改** `README.md`：
- 新「安装」节：`curl -fsSL https://raw.githubusercontent.com/The-Astral-Express-Family/astral-cli/main/scripts/install.sh | sh`；Windows `powershell -c "iwr -useb https://raw.githubusercontent.com/The-Astral-Express-Family/astral-cli/main/scripts/install.ps1 | iex"`；指定版本 env 用法；源码构建保留。
- 新「更新」节：`astral update`、`astral update v0.2.1`、`astral update --check`；注明代理走标准环境变量（`HTTPS_PROXY` 等）。
- 退出码表：加 `10 UPDATE_INTEGRITY 校验失败`。
- 包管理器渠道标注“计划中”。

**提交**：`docs(readme): install, update, exit code 10`

### 任务 14：全量回归 + CI 预设

- `cmake --preset ci && cmake --build --preset ci && ctest --test-dir build/ci --output-on-failure`（`-Werror` 必须零警告）。
- `cmake --preset dev` 全量单测；集成测试带网跑一遍（`-DASTRAL_ENABLE_INTEGRATION_TESTS=ON` 单独 configure 一个 build 目录）。
- 手工：本地 `./build/dev/astral update` → 应得 exit 8 与正确消息（非托管位置路径验证）。

**提交**（若有修）：`fix: address ci-preset warnings`

### 任务 15：发版演习（用户门控）

1. `CMakeLists.txt` bump `VERSION 0.2.0`；提交 `chore(release): bump version to 0.2.0`。
2. **征得用户同意后**打 `v0.2.0` tag 推送，触发 release。
3. 流水线观察点：verify-version 通过、5 平台产物 + 两个安装脚本 + SHA256SUMS 上传、smoke 含 `update --check`。
4. 手工验收清单（规格 §5.4，三平台各一轮）：`curl|sh` 安装 → `astral version` 显示 `v0.2.0` → `astral update --check` → 发一个 `v0.2.1` 后 `astral update` 升级 + `astral update v0.2.0` 降级回滚。Windows 侧 `install.ps1` 同流程。
5. 结果回填本文件勾选。

## 完成标准

- 规格 §5 测试矩阵全部落地且绿；
- `astral update --check --json` 在 CI 与本地（任意位置）可用；
- 安装脚本三平台装出可用的 `astral` 且 `astral version` 显示烙入版本；
- 发版演习（任务 15）通过。

## 风险与备注

- libarchive/picosha2 经 vcpkg 首次安装耗时较长（Windows triplet 尤甚），属预期。
- `update --check` 跳过位置判定是对规格流程顺序的必要细化（否则 CI 冒烟无法通过），规格意图（安装动作需守护）不受影响。
- `currentExecutable` 用 OS API 而非 argv[0]（任务 7 已记录理由）。
- GitHub API 匿名限速 60 req/h：单次 update 仅 1-2 个请求，不受影响；README 不需提。
