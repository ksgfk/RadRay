# ADR-0038 asset 元数据改用 LMDB 存储

状态: 已被 ADR-0040 取代
日期: 2026-08
影响: `modules/runtime` 的 `AssetBundleManifest` / `AssetDatabase`、asset 元数据运行时存储与
`bundle.xml` 落盘、`third_party/` 依赖（LMDB）与 `project_manifest.json`

## 背景

ADR-0036/0037 选 XML 并让 pugixml DOM 常驻内存作为后备存储，唯一动机是 git merge 保序
（写回不得移动未触碰节点、不丢注释、逐字节保真）。这条动机只服务于"人类手工编辑清单"的
开发场景。运行时读取只需快速 CRUD，DOM 遍历 + 文本解析（from_chars）成了纯开销，且
`XmlElement` 出现在资产消费方的公开签名里，把 XML 实现细节一路泄漏到 loader。需要为
asset 元数据引入一个面向运行时的高性能存储，同时保留 XML 作为工程落盘的持久格式。

## 决策

**引入 LMDB 作为 asset 元数据的运行时工作态；XML 降级为落盘时的序列化格式。**

- 一个工程 = 一个 LMDB 库（environment），其中 `assets` 表按 `AssetId → asset 元数据`
  存储。key 是 16 字节 GUID，value 是 header + data 段。
- value 布局：`header = [u32 headerLen][u32 typeLen][type][u32 pathLen][path][u32 dataLen]`，
  其后是 `dataLen` 字节的 data 段。`type` / `path` 是存储层机器可读、可查询的固定字段，
  data 段对存储层完全 opaque。
- `path` 是相对工程的全局唯一路径；本轮 asset 元数据来源不绑定 bundle（per-bundle 组织
  与 bundle 身份留待后续轮次）。
- 运行时 CRUD 全在 LMDB；落盘 = 把 LMDB 数据序列化成 XML 存入工程。放弃保序——XML 不再是
  "逐字节保真的 merge 友好格式"，而是可读的持久快照。
- data 段编码完全由后续系统（资产 loader）自行决定，核心不引入统一 serializer 定制点。
- pugixml 与 `radray/xml.h` 保留（承担 XML 序列化），但 `XmlElement` 不再出现在资产消费方
  的公开签名里。

## 放弃的方案及代价

- **继续 DOM 常驻 + XML 保序（ADR-0037）**：保留 merge 动机，但运行时读取必须遍历 DOM +
  文本解析，XML 类型持续泄漏到 loader；与"高性能、不暴露 XML"的目标正面冲突。
- **自研键排序 B+tree / 紧凑 KV**：自包含、无新依赖，但要重造 LMDB 已验证的事务与持久化
  轮子；LMDB 单文件极轻（一个 .c + 一个 .h），引入成本远低于自研收益。
- **data 段统一 serializer 定制点（`ParamSerializer<T>`）**：让核心理解类型、统一编码，但
  编码语义应归后续系统决定；核心存储层只做 byte[] → byte[] 的可靠存取，不做类型理解。
- **data 段用 JSON（yyjson 已有）**：零新编码方案，但无 guid 原生类型、浮点只有 double、
  无注释，且与"紧凑二进制、低内存"目标相悖。

## 必须保持为真

- 运行时 asset 元数据的唯一权威是 LMDB 的 `assets` 表；`bundle.xml` 只是落盘持久快照，
  不承担运行时读取路径。
- 存储层只理解 value 的 header（`type` / `path`）并据此建查询索引，不解析 data 段内容。
- `XmlElement` / DOM 节点不出现在资产消费方（loader）的公开签名里。
- data 段编码由后续系统自行决定，核心不提供 serializer 定制点。
- 资产 GUID 仍是永久身份（ADR-0036），本轮不改变；per-bundle 组织与 bundle 身份留待后续
  轮次，届时再调整 ADR-0036。
