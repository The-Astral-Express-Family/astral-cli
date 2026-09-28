# Protocol snapshots

这个目录保存 `astral-modulator` 公网协议的**冻结快照**，CLI 的 contract
tests（`tests/contract/`）针对这里的固定版本运行，而不是针对某个在线
服务器。详见 `docs/ARCHITECTURE.md` 第 2 节（双仓库边界）。

约定：

- 每个协议版本一个子目录：`snapshots/v1/`、`snapshots/v2/`……
- 每个快照必须带 `MANIFEST.json`：记录源仓库 commit、发布日期、
  服务端实装状态（哪些端点是真实的、哪些还是 501 桩）与关键语义备注，
  是 CLI 实现时的第一阅读材料；
- 服务端发布协议变更时，同步新的 OpenAPI/Schema 快照到对应目录，
  并在 PR 中注明协议版本与变更摘要；
- CLI 侧 bump `src/core/version.hpp.in` 里的 `kProtocolVersion` 之前，
  contract tests 必须先在 CI 中对新快照全绿；
- 快照只描述公网协议（JSON 字段与语义），不包含任何实现代码。
