# AGENTS.md — astral-cli 代理协作约定

## 推送前测试纪律（硬约束）

**每次推送前确保所有测试通过。**

- 当前阶段（无可靠的全量快速通道）：推送前跑**全量**单测
  `ctest --test-dir build/dev --output-on-failure`。
- 全量基线建立后：允许只跑**与 diff 相关**的测试（按改动文件映射到测试目标，
  如改 `src/core/semver.cpp` → `ctest -R semver`），用 **CI 兜底**全量——
  CI 红了视为推送事故。
- 无论哪种模式：**推送前测试必须实际运行且通过，不允许"应该能过"式跳过**。

### 操作细则

1. 改动后先构建再测试：`cmake --build --preset dev -j && ctest ...`。
2. diff 相关测试的最低集合 = 改动源文件对应的测试文件 + `test_cli`（契约回归）。
3. 涉及 CI/工作流/脚本的改动（`.github/`、`scripts/`）无本地测试可跑，
   推送前至少做语法校验（`sh -n`、YAML parse 等），CI 为最终防线。
4. 修复 CI 失败的提交同样适用本约束（先本地复现 → 修 → 过测 → 推）。

## 其他约定

- 提交信息遵循 Conventional Commits（commitlint 钩子强制，types 含 protocol）。
- 用户可见输出 ASCII（`->` 不用 `→`）。
- vcpkg 依赖必须走 manifest（`vcpkg.json`）+ baseline 锁定。
- 代理环境变量（`HTTP_PROXY` 等）依赖 libcurl 默认行为，**禁止**设置任何
  `CURLOPT_PROXY*`。
- 本地跑测试永不真开浏览器：单测 listener 常设 `ASTRAL_NO_BROWSER=1`（platform/browser.hpp 契约），新增会走到 device-flow 呈现路径的测试不得绕过该闸。
- 发版流程：bump `CMakeLists.txt` VERSION → 打 `vX.Y.Z`（可带 prerelease 后缀，
  如 `-rc.1`/`-alpha.1`）→ release.yml 校验/构建/发布；tag 与 VERSION 数字核心
  必须一致。**正式 tag 不可删除重打**（演习期除外）。

## 推送前验证（2026-09-28 教训固化）

pre-commit hook 只检查暂存文件，**无法覆盖**：子代理 worktree 的提交、
合并冲突手工解决、直接编辑后未 add 的文件。因此：

- **推 main 前**：开一个验证子代理本地跑完 CI 的全部检查（clang-format
  全仓 dry-run、dev/ci 双 preset 构建+测试、YAML/sh 语法、
  `istreambuf_iterator` 零使用、git status 干净），裁决 PUSH 才推。
- 修 CI 红的提交同样走这道闸。
- 禁用 `istreambuf_iterator`（GCC 13 -O3 误报），读文件用 seekg/tellg/read。
