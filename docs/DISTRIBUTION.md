# Astral CLI 发行链路设计与实施计划

> 状态：设计定稿；**2026-09-28 状态刷新**——并行会话已落地自更新切片
> （版本烙入 / `astral update` / 安装脚本 / release 强化，commits
> ca84b15..cce9cf2，spec 见
> [superpowers/specs/2026-09-27-distribution-update-design.md](superpowers/specs/2026-09-27-distribution-update-design.md)），
> 并以 5 个 `v0.1.1-alpha.N` tag 演练了全流程。本文仍是 Distribution MVP
> 的总控（自更新切片不覆盖的部分：Windows 静态链接、glibc 基线、SBOM、
> 包管理器、签名），实施进度以文末 R0-R5 勾选为准。
>
> 需求依据（modulator 仓）：FR-015（三端发行）、NFR-001（单文件/极小
> 依赖集）、NFR-008（三端 build smoke）、ADR-0007（原生发行决策）、
> roadmap「Distribution MVP」里程碑与 MVP DoD（"downloadable assets
> pass smoke test; at least Homebrew + Scoop are functional"）。

## 1. 现状与缺口（2026-09-28 刷新）

release.yml 已经 **5 次 alpha tag 真实演练**（v0.1.1-alpha.1~5，含
prerelease 语义、版本烙入、`update --check` 冒烟、tag↔VERSION 校验）。
已落地：版本一致性门（原 G2 ✅）、版本烙入与 semver 解析（含 prerelease
排序）、`astral update` 自更新（exit 10 供应链红线）、POSIX/Windows 安装
脚本（作为 raw 引用主推入口——ADR-0007 只禁"curl|sh 唯一通道"，多渠道
并存合规）。**但 archive 产物本身的质量缺口一个没修**，且已随 5 个
alpha 连发：

| # | 缺口 | 严重度（2026-09-28） |
|---|---|---|
| G1 | ~~Windows 动态 triplet 缺 DLL~~ **2026-09-28 已修**：CI/release 矩阵切 `x64-windows-static`（静态 CRT，单 exe 自包含），dumpbin 门 + release 解包冒烟双防回归；存量 alpha.1~5 仍是坏包，下次 tag 才体现 | 已闭合（待下次 tag 验证） |
| G2 | ~~版本号双轨~~ ✅ 已修：release workflow 构建前校验 tag↔VERSION，版本烙入 `-DASTRAL_EMBED_VERSION` | 已闭合 |
| G3 | packaged smoke：**2026-09-28 最小面已修**——release 打包后同 job 从 archive 解包运行（bsdtar 解 zip）；跨平台干净 runner 矩阵（§6）仍留 R0+ | 最小面已闭合 |
| G4 | 无 SBOM；无签名/公证步骤（ADR-0007 与 roadmap 共通项） | 高 |
| G5 | 包管理器零落地（Scoop bucket / Homebrew tap / deb 均无仓库无 manifest；自更新切片明确将其留作"计划中"） | 高（DoD 硬项） |
| G6 | macOS x86_64 产物缺失（macos-13 runner 移除后未补；自更新 spec 的平台映射表列了 macos-x64，但 workflow 矩阵仍是 4 目标） | 中 |
| G7 | Linux glibc 基线仍只有 README 一句话（ubuntu-24.04 = glibc 2.39），无兼容验证、无最低基线声明落档；安装脚本会把 2.39 基线的产物发给老发行版用户 | 中（随 install.sh 上线而升高） |
| G8 | 打包仍手工 cp `build/ci/` 产物（ci preset 带 tests），`install()` 规则与发行物脱节 | 低（顺带修） |

## 2. 目标与验收映射

Exit criteria（roadmap）：**新机器无需开发工具链即可安装并登录**。

| 要求 | 设计落点 |
|---|---|
| 三端原生 Release Asset + SHA-256 | §3 产物矩阵、§5 打包 |
| NFR-001 单可执行文件 | §3 静态链接策略 |
| SBOM | §5 |
| packaged smoke / clean-machine | §6 |
| Scoop + Homebrew functional（DoD） | §7 |
| stable 签名（ADR-0007） | §8（secrets 就绪前 feature-flag 关闭） |
| deb 或 rpm 至少一个 | §7.3 |
| glibc compatibility test / musl spike | §4 |

## 3. 产物矩阵与链接策略

| target | archive | 链接策略 |
|---|---|---|
| windows-x64 | `astral-<v>-windows-x64.zip` | **`x64-windows-static` triplet（静态 CRT）**，单 exe 自包含（解 G1/NFR-001） |
| macos-arm64 | `astral-<v>-macos-arm64.tar.gz` | `arm64-osx`（系统 Clang，仅系统动态库依赖） |
| macos-x64 | `astral-<v>-macos-x64.tar.gz` | 在 arm64 runner 上以 `x64-osx` triplet 交叉构建（Xcode 自带 x86_64 SDK），**不用 osxcross**（SDK 再分发有许可灰区；vcpkg 交叉构建即可） |
| linux-x64 | `astral-<v>-linux-x64.tar.gz` | `x64-linux`（vcpkg 静态库，系统仅 libc/libstdc++） |
| linux-arm64 | `astral-<v>-linux-arm64.tar.gz` | `arm64-linux`，ubuntu-24.04-arm runner |

- 静态 CRT 的体积代价（约 +5-8MB）换取「解包即用」，符合 NFR-001。
- macOS 双架构：先分架构两个 archive（结构简单、构建并行），universal
  lipo 合并留作后续优化；Homebrew formula 同时声明两个 sha。
- archive 内容：二进制 + LICENSE + README.md +（Linux）无额外 so；
  Windows zip 含单 exe。打包统一走 `cmake --install` 装入 staging 目录
  再压缩（解 G8 的 install/发行脱节；不引入 CPack，手工 staging 足够）。

## 4. 构建基线（glibc / musl / triplet）

- **Linux 构建基线改为 ubuntu-22.04**（glibc 2.35、libstdc++ 12）：
  24.04（2.39）过新，会无声排除 Debian 12（2.36 以下无压力）与多数
  企业基线；20.04 已出标准支持期且 vcpkg 需要新工具链。README 与
  ARCHITECTURE §16 同步声明「最低 glibc 2.35」。
- 验证即 §6 的 smoke 矩阵：产物分别在 ubuntu-22.04（下限）与
  ubuntu-24.04（当前）跑通；smoke 中 `objdump -T | grep GLIBC_` 取
  最大 GLIBC 版本符号，断言 ≤ 2.35（防依赖悄悄抬高基线）。
- **musl spike：触发式不做**（roadmap 本就标 spike）。留记录位：若出现
  Alpine/静态容器需求，评估 musl 专用 triplet 产物。arm64 基线随 runner
  （24.04-arm，glibc 2.39）——arm 服务器场景后置，接受并在 README 标注。
- 新增 CMake preset `dist`：Release、`ASTRAL_BUILD_TESTS=OFF`、
  triplet 由 workflow 按矩阵注入（`VCPKG_TARGET_TRIPLET`）；
  release.yml 构建步骤从 `ci` preset 切到 `dist`（解 G8 语义错位）。

## 5. 打包、校验和、SBOM

- 每目标：`cmake --install build/dist --prefix staging/astral-<v>-<target>`
  → 压缩 → `sha256sum` 写入聚合 `SHA256SUMS.txt`（沿用现状）。
- **SBOM（解 G4）**：每目标跑 `syft dir:staging/`（CI 工具，不属运行时
  依赖约束）产出 SPDX-JSON `astral-<v>-<target>.spdx.json`；vcpkg 依赖
  版本由 vcpkg.json + builtin-baseline 锁定，SBOM 附 baseline commit。
  全部 SBOM 与 SHA256SUMS 一并挂到 Release assets。
- Release notes 模板含：版本、协议版本（kProtocolVersion）、glibc 基线、
  签名状态（见 §8）、SBOM/校验和指引。

## 6. Packaged smoke（clean-machine 验证，解 G3）

publish 前独立 job：**下载 archive 产物本身**（非构建树），在干净 runner
矩阵解包执行：

| runner | 验证目标 |
|---|---|
| windows-2022 | unzip → `astral.exe version` / `version --json` 断言 `protocolVersion` / `doctor --json` / `astral bogus` exit 2 |
| macos-latest (arm64) | tar 解包同上四项 |
| ubuntu-22.04 | 同上四项 + GLIBC 符号审计（§4） |
| ubuntu-24.04 | 同上四项（上限回归） |

- 「安装并登录」中的登录部分需要真实服务端与浏览器，自动化性价比低：
  v0.1.0 以**发布 runbook 手动项**承载（发布后在真机走一遍 login→todo
  list），后续视服务端发行物（容器镜像等）再自动化。
- Windows smoke 直接运行 exe 即是静态链接的证明（无需任何 DLL 存在）。

## 7. 包管理器（解 G5）

### 7.1 Scoop（Windows，先做——DoD 硬项）
- 新仓库 `<org>/scoop-astral`（bucket）；manifest `astral.json` 由
  release workflow 从本 Release 资产生成（url + sha256 + bin），以
  **开 PR 方式**提交到 bucket（v1 维护者手合；自动化合并待 PAT 权限）。
- 验收：全新 Windows 机器 `scoop bucket add astral <url>` → `scoop
  install astral` → `astral version` 成功。

### 7.2 Homebrew（macOS）
- 新仓库 `<org>/homebrew-tap`；formula 由 workflow 生成（arm64/x64 双
  url+sha256），同样 PR 方式提交。
- 验收：全新 macOS `brew tap <org>/astral && brew install astral` 可用
  （Gatekeeper 提示与 §8 签名状态联动标注）。

### 7.3 deb（Linux，先 deb 后 rpm）
- 手工 `dpkg-deb` 打包（不引入 fpm/ruby 依赖）：`/usr/bin/astral` +
  LICENSE；`Depends: libc6 (>= 2.35), libstdc++6 (>= 12)`。
- v0.1.0 只挂 Release asset；apt 仓库托管后置。rpm 待真实需求（触发式）。

## 8. 签名与公证（解 G4，外部资源门控）

原则：**流程先就位、secrets 未配则降级跳过并如实标注**，不因无证书
阻塞发布。

| 平台 | secrets 就绪时 | 未就绪时（v0.1.0 预期状态） |
|---|---|---|
| macOS | Developer ID codesign + `notarytool` 公证（APPLE_CERTIFICATE_*、APPLE_ID/ISSUER/KEY） | ad-hoc 签名（`codesign -s -`），release notes 标注「未公证，首次运行需右键打开」 |
| Windows | 代码签名证书 / Azure Trusted Signing（待选型） | 不签名，标注 SmartScreen 提示；Scoop 安装路径不受影响 |

签名属于 R5 阶段，前置条件是用户采购决策（见 §10）。

## 9. 发布流程（runbook）

1. 版本提交：`project(VERSION x.y.z)` bump（唯一版本事实源）。
2. 打 tag `vx.y.z`；workflow 断言 `GITHUB_REF_NAME == v<PROJECT_VERSION>`
   不符即 fail（解 G2）。
3. build×4 → 打包+SBOM+checksums → packaged smoke 矩阵全绿 →
   `gh release create`（draft）→ Scoop/Homebrew PR → 人工核对后
   publish release + 合并包管理器 PR。
4. 发布后手动项：真机 login 冒烟（§6）、release notes 核对。

## 10. 开放决策点（需要用户拍板，均已给建议）

| # | 决策 | 建议 |
|---|---|---|
| D-A | 首发版本与时机 | alpha 序列已至 `v0.1.1-alpha.5`（坏 Windows 包连发 5 次）；建议 R0 archive 三件套修完后打 `v0.1.1-rc1`，正式 `v0.1.1` 可与 agent credential CLI（docs/AGENT-CREDENTIALS.md P1/P2）落地后合并窗口 |
| D-B | scoop bucket / homebrew tap 两个新仓库的建立与 workflow 开 PR 所需 token 权限 | 建 `scoop-astral` 与 `homebrew-tap` 于同 org；v1 用 PAT 开 PR、人合 |
| D-C | macOS Apple Developer 账号（$99/年）与 Windows 签名证书（EV 约 $数百/年 / Azure Trusted Signing）是否采购 | 不采购则签名长期停留 §8 右列降级态；Scoop/手动安装不受阻 |
| D-D | deb 先行还是 rpm 先行 | deb（本机 WSL 即 Debian 部署场景）；rpm 触发式 |
| D-E | musl spike 是否排期 | 不排期（触发式：出现 Alpine/静态容器需求再做） |

## 11. 实施阶段（R0-R5，每段收尾 = CI 绿 + 对应验收达成）

> 2026-09-28 状态：tag↔版本断言与版本烙入已由自更新切片落地（并入 R0
> 记账）；`astral update` / 安装脚本 / 5 次 alpha 演练超出本文原范围，
> 属并行切片（§头部 spec 链接），不占 R 段编号。**R0 剩余=archive 质量
> 三件套（G1 静态 triplet、G3 packaged smoke、22.04 基线），修完前任何
> alpha 的 Windows zip 都不可用（§1 实锤），install.sh 的 Linux 用户也
> 拿的是 2.39 基线产物。**

- [ ] **R0 首发布打通（archive 质量清零）**：~~tag↔版本断言~~✅ +
  ~~Windows 静态 triplet~~✅（2026-09-28，x64-windows-static + dumpbin 门）
  + ~~archive 解包冒烟~~✅（release 同 job 最小面）+ 剩余：`dist` preset、
  Linux 基线切 22.04、`cmake --install` staging 打包、SBOM、跨平台干净
  runner 冒烟矩阵；打 `v0.1.1-rc1` 试跑全流程（alpha 序列已消耗至 0.1.1）。
  验收：4 archives + SHA256SUMS + SBOM 挂 draft release，smoke 全绿，
  Windows zip 在无 vcpkg 环境解包即跑。
- [ ] **R1 基线落档**：README/ARCHITECTURE §16 声明 glibc 2.35 与产物
  矩阵；GLIBC 符号审计进 smoke；install.sh 按基线提示老发行版。
- [ ] **R2 Scoop**：bucket 仓库 + manifest 生成 + PR 流程；新机安装验收。
- [ ] **R3 Homebrew + macOS x64**：tap 仓库 + formula 生成；`x64-osx`
  交叉构建补第五产物；brew 安装验收。
- [ ] **R4 deb**：dpkg-deb 产物 + Depends 基线；Ubuntu 22.04 `dpkg -i`
  安装验收。
- [ ] **R5 签名/公证**：secrets 就绪后启用 §8 左列（macOS 优先），
  WinGet 评估随 Windows 签名一并。
