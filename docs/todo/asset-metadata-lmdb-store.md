> - 适用: asset 元数据运行时存储改用 LMDB 的专项实施计划——引入 LMDB 依赖、核心 KV 封装、
>   `AssetBundleManifest` / `AssetDatabase` 迁移、XML 降级为落盘序列化
> - 权威: 本文是 grilling 对齐后的专项实施计划；决策 rationale 冻结在 ADR-0038（取代 ADR-0037
>   的 DOM 常驻），实现完成后的现状描述以 `docs/architecture/` 为准
> - 状态: 已作废（2026-08-17）。曾于 2026-08-14 落地，但 ADR-0040 以进版本控制的单份 JSON
>   清单取代 LMDB，相关实现（`radray/lmdb.h`、`AssetDatabase` 的 LMDB 存取、lmdb 依赖）已
>   移除。本文是历史快照；当前计划见 `docs/todo/asset-database-json-manifest.md`。
> - 锚点: `docs/adr/0038-asset-metadata-in-lmdb.md`, `docs/adr/0039-abandon-bundle-organization-stabilize-asset-system.md`, `docs/adr/0040-single-text-manifest-is-asset-identity-authority.md`, `docs/architecture/asset-database.md`, `modules/runtime/include/radray/runtime/asset_database.h`, `modules/runtime/src/asset_database.cpp`, `modules/core/include/radray/xml.h`, `project_manifest.json`, `CMakeLists.txt`

# asset 元数据改用 LMDB 存储（实施计划）

## 目标

把 asset 元数据的运行时表示从「pugixml DOM 常驻」（ADR-0037）换成 **LMDB 键排序存储**，XML
降级为落盘时的序列化快照：

- 一个工程 = 一个 LMDB 库（environment），`assets` 表按 `AssetId → asset 元数据` 存储。
  运行时 CRUD 全在 LMDB，读一个资产 = 一次 O(log N) 键查找 + 一次 value 反序列化。
- value = header + data 段：header 的 `type` / `path` 是存储层机器可读、可查询的固定字段，
  data 段对存储层完全 opaque（ADR-0038）。
- **放弃写回保序**：XML 不再是"逐字节保真的 merge 友好格式"，而是可读的持久快照。
- **不暴露 XML**：`XmlElement` 不再出现在资产消费方（loader）的公开签名里；pugixml 只承担
  XML 序列化，退居 `radraycore` 内部。

本轮不碰 bundle：asset 来源不绑定 bundle，`path` 是相对工程的全局唯一路径；per-bundle 组织
与 bundle 身份留待后续轮次（届时再调整 ADR-0036）。

## 工作术语

- **库（environment）**：一个 LMDB 环境，对应一个工程的元数据存储（一个目录或单文件）。
- **表（named database，DBI）**：库内独立的 key 空间。本轮只有 `assets` 一张表；未来可
  再开 `settings`、`prefab` 等表。
- **asset 元数据**：一条 `assets` 表记录。key 是 `AssetId`（16B GUID），value 是 header + data 段。
- **header**：value 前缀的机器可读固定字段（`type` / `path`），存储层据此建查询索引。
- **data 段**：header 之后的 opaque 字节，编码归后续系统（资产 loader）自行决定。

## 已确认需求

1. 引入 LMDB 作为 asset 元数据的运行时工作态；一工程一库，`assets` 表 guid→value。
2. key = `AssetId`（16B GUID），value = header + data 段。
3. value 布局：`header = [u32 headerLen][u32 typeLen][type][u32 pathLen][path][u32 dataLen]`，
   其后是 `dataLen` 字节 data 段。`headerLen` 含自身；四个长度字段均 `u32`。
4. `type` / `path` 是存储层机器可读、可查询的固定字段；data 段对存储层 opaque。
5. `path` = 相对工程的全局唯一路径（无 bundle 层级）。
6. 运行时 CRUD 在 LMDB；落盘 = 把 LMDB 数据序列化成 XML 存入工程，保序放弃。
7. 不引入统一 serializer/deserializer 定制点；data 段编码完全由后续系统自行决定。
8. `pugixml` 与 `radray/xml.h` 保留（承担 XML 序列化），但 `XmlElement` 不再出现在资产
   消费方公开签名里。
9. 旧 DOM 常驻实现、D10 读写 helper、`XmlElement` 公开签名整体替换。
10. 文档（ADR-0038 与本文）先行；**用户明确指令前不开始实现**。

## 设计基线

### D1：库表拓扑

状态：**已确认（2026-08-14）**。

```text
一个工程 = 一个 LMDB 库 (env)
  └─ assets 表 (DBI)
       key:   AssetId (16B GUID)
       value: header + data 段
```

- 运行时 asset 元数据的唯一权威是 `assets` 表；`bundle.xml` 只是落盘持久快照。
- 键排序由 LMDB 保证（B+tree，按 GUID 字节序）；本轮不依赖键排序语义（GUID 无业务序），
  仅享受其 O(log N) 查找。
- `type` / `path` 的查询索引（path→guid 反查、type 分派）在加载时于内存建，不落第二张表。

### D2：value 布局

状态：**已确认（2026-08-14）**。

```
value := header | data 段
header := [u32 headerLen][u32 typeLen][type bytes][u32 pathLen][path bytes][u32 dataLen]
data 段 := dataLen 字节 opaque bytes
```

- `headerLen` = 整个 header 的字节长（含它自己），解码时据此切出 header 与 data 段。
- `typeLen` / `pathLen` / `dataLen` 均 `u32`；`type` / `path` 是长度前缀的 UTF-8 字节。
- `dataLen` 显式存储（data 段之后未来可再跟扩展尾）。
- 存储层只解码 header；data 段原样透传，不解析。

### D3：path 口径

状态：**已确认（2026-08-14）**。

- `path` 是相对工程的全局唯一路径，无 bundle 层级。guid 查到的 path 直接拼工程根即得
  绝对路径。
- 唯一性按大小写不敏感口径校验（沿用 ADR-0036 的 Windows 事实：两个只差大小写的 path
  指向同一文件）。
- 存储形态沿用既有 `NormalizeEntryPath` / `IsValidStoredPath` 的口径（`/` 分隔、无绝对
  路径、无 `..`、无盘符、无空段）——这组函数本轮保留复用，只把"bundle 内相对"改写为
  "工程相对"的语义描述。

### D4：XML 角色

状态：**已确认（2026-08-14）**。

- XML 是落盘持久快照，不再承担运行时读取路径，也不再要求逐字节保真。
- 落盘 = 遍历 `assets` 表（LMDB cursor）→ 序列化成 XML → 写回工程；读回 = 解析 XML → 填充 LMDB。
- 保序放弃后，序列化格式可自由选择（缩进、重排、规范化均可），不再有"未触碰节点逐字节
  保持"的约束。
- pugixml 退居 `radraycore` 内部，只服务这条序列化路径。

### D5：编码归属

状态：**已确认（2026-08-14）**。

- 核心不提供 serializer/deserializer 定制点，不引入 `ParamSerializer<T>` 一类类型化编码。
- data 段 = opaque 字节串；资产 loader 如何编码/解码完全自行决定（未来系统各自实现自己的
  struct 序列化，与本轮存储层无关）。
- 存储层 API 只见 `byte[] → byte[]`，不出现类型、字段、树的任何概念。

### D6：LMDB 依赖落地

状态：**已确认（2026-08-14，库选型定稿 LMDB；tag 与接入细节待实现时确认）**。

- `project_manifest.json` 的 `ThirdParties` 数组加条目（Git 源、tag 待确认最新稳定版）：
  `{"Name": "lmdb", "Type": "git", "Git": "https://github.com/LMDB/lmdb.git", "Tag": "..."}`。
- 根 `CMakeLists.txt` 照 pugixml/yyjson 的样子接入：关闭上游测试后
  `add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/lmdb)`，再 `radray_optimize_flags_library` +
  `radray_set_build_path`。LMDB 是纯 C 单文件库，`add_subdirectory` 前确认上游 CMake 开关。
- 许可注意：LMDB 是 OLDAP-2.8，与仓库现有 MIT 依赖不同，接入前需确认合规口径（已在
  ADR-0038 背景提及，属于已接受的取舍）。

## 未决问题（实现前 grill）

以下不在本轮已确认范围内，开始实现前需逐项 grill：

1. **分层落点**：核心 LMDB KV 封装放 `radraycore`（与 `xml.h` / `json.h` 并列的通用设施）
   还是 `radrayruntime`（仅 runtime 使用）？header 的 type/path 编解码放哪层？
2. **事务边界**：LMDB 的 `mdb_txn` 何时 begin/commit——每次 CRUD 一事务，还是显式
   begin/commit 由 `AssetDatabase` 门面管理？
3. **写盘 flags**：`MDB_NOSUBDIR` / `MDB_WRITEMAP` / `MDB_NOSYNC` / `MDB_MAPASYNC` 等取舍，
   对齐仓库的同步/持久化语义。
4. **错误处理**：LMDB 返回码（`MDB_NOTFOUND` / `MDB_MAP_FULL` / ...）→ `std::error_code`
   还是沿用 `string& outError`？MAP_FULL 是否需要预扩 map size？
5. **并发模型**：LMDB 天然多读单写；`AssetDatabase` 的既有单线程契约（ADR-0037 D8）如何
   与 LMDB 事务对齐。
6. **XML 落盘 schema**：快照 XML 的根元素/字段形态（沿用 `<bundle>` 还是新 schema），以及
   落盘触发时机（显式 `Save` 是否仍保留）。

## 实施阶段与检查站

> 阶段划分依赖未决问题 1（分层落点）与 2（事务边界），此处为初稿，grill 后修订。

### M0：LMDB 依赖引入

1. `project_manifest.json` 加 lmdb 条目，`python tools/fetch_third_party.py restore` 拉取。
2. 根 CMake 接入（D6），空跑一次全量构建确认无破坏。

检查站：构建通过；`third_party/lmdb` 由脚本管理，未手改。

### M1：核心 KV 封装

1. LMDB env / DBI 的 RAII 封装，byte[] → byte[] 的 Get/Put/Delete/游标遍历（D1/D2/D5）。
2. 封装不暴露 `mdb_*` 裸类型；不包含 header 的 type/path 领域语义（或按未决问题 1 定）。

检查站：KV 读写 round-trip 测试；删除/遍历测试；错误路径（NOTFOUND 等）测试。

### M2：asset 元数据层与 AssetDatabase 迁移

1. header 编解码（D2）与 path→guid / type 分派索引（D3）。
2. `AssetBundleManifest` / `AssetDatabase` 从 DOM 常驻迁到 LMDB；公开签名去掉 `XmlElement`。
3. 落盘序列化 XML + 读回（D4）。

检查站：既有 `AssetDatabaseTest` 语义级通过（XML 保序相关用例改写/删除）；读回→落盘→再读回
一致。

### M3：文档与收尾

1. 更新 `docs/architecture/asset-database.md`（现状契约：LMDB 存储、value 布局、XML 快照）。
2. `docs/architecture/overview.md` 索引按需加行；`CONTEXT.md` 术语核对。
3. 全量 `ctest` 通过。

检查站：文档与实现同批提交（maintenance duty）。

## 测试矩阵

`modules/runtime/tests/`（及核心封装落点对应的 `modules/core/tests/`），全程不碰 GPU 与
`AssetManager`。

### 核心 KV 封装测试

- Get/Put/Delete round-trip：byte[] 原样存取，含空 value、含 NUL 字节。
- 键排序遍历：游标按 key 字节序输出全部条目。
- 未命中 Get / 重复 Put / 删除不存在 key 的行为。
- 环境关闭后重开，数据仍在（持久化）。

### AssetDatabase 迁移后测试（语义级沿用，保序相关改写）

- `Mount` 建索引；`Resolve` 拼出正确绝对路径（工程根 + path）；`FindByPath` 命中与未命中。
- path 大小写不敏感重复硬失败；GUID 重复硬失败。
- `AddEntry`：输入规范化、新 GUID 分配、撞 path 返回失败并附已有 GUID。
- 落盘 → 读回 → 索引与 Resolve 结果一致（替代旧的"DOM round-trip 逐字节"用例）。

## 非目标

- per-bundle 组织、bundle 身份 / guid、嵌套检测——本轮 asset 来源不绑 bundle，留待后续。
- data 段编码的通用 serializer/deserializer——归后续系统自行决定。
- 多资产根、目录扫描 Sync、文件监控、移动重命名自动 GUID 保持（沿用 ADR-0036 边界）。
- 打包成运行时二进制 bundle、发布形态裁剪、导入产物缓存。
- LMDB 的跨进程并发写入、多读者快照隔离等事务高级特性——本轮单写者即可。

## 对齐记录

### 已确认
- **2026-08-14 / C1**：引入 LMDB 作为 asset 元数据运行时工作态；一工程一库，`assets` 表
  guid→value；XML 降级为落盘序列化快照（ADR-0038）。
- **2026-08-14 / C2**：key = `AssetId`（16B GUID），value = header + data 段；data 段 opaque。
- **2026-08-14 / C3**：value 布局 `[headerLen][typeLen][type][pathLen][path][dataLen]` + data，
  四个长度字段均 `u32`，`headerLen` 含自身；`dataLen` 显式存（可留扩展尾）。
- **2026-08-14 / C4**：`type` / `path` 是存储层机器可读、可查询的固定字段（header），data
  段完全不碰。
- **2026-08-14 / C5**：`path` = 相对工程的全局唯一路径；asset 来源不绑定 bundle，per-bundle
  与 bundle 身份留待后续。
- **2026-08-14 / C6**：放弃写回保序；XML 不再是 merge 友好逐字节格式（推翻 ADR-0037 的动机）。
- **2026-08-14 / C7**：不引入统一 serializer 定制点；data 段编码由后续系统自行决定。
- **2026-08-14 / C8**：pugixml 与 `radray/xml.h` 保留（承担 XML 序列化），`XmlElement` 不再
  出现在资产消费方公开签名。
- **2026-08-14 / C9**：旧 DOM 常驻实现、D10 helper、`XmlElement` 公开签名整体替换。

## 落地记录（2026-08-14）

- **未决问题收敛**（grilling 时用户拍板）：① LMDB KV 封装落 `radraycore`（`radray/lmdb.h`，
  明确标注为 LMDB 封装）；② XML 落盘形态「先不管」——本轮保留 per-bundle `bundle.xml` 与
  bundle 语义不动，只做内存侧的 DOM 常驻 → LMDB 迁移。
- **M0**：`project_manifest.json` 加 lmdb（Git `LMDB_1.0.1`，OLDAP-2.8）；根 CMake 自建
  `lmdb` 目标编译 `mdb.c`+`midl.c`+`module.c`（上游无 CMake，只随附 Makefile）。
- **M1**：`modules/core` 新增 `lmdb.h`/`lmdb.cpp`——`LmdbEnvironment`/`LmdbTransaction`/
  `LmdbCursor`，byte[]→byte[] 存取，错误收敛为 `LmdbResult`（Ok/NotFound/Failure），
  PRIVATE 链接；`mdb_env_set_maxdbs(16)` 放开命名库。测试 `test_lmdb.cpp`（7 用例）。
  `core-facilities.md` 增补 `lmdb.h` 条目。
- **M2**：`AssetBundleManifest` 类改为 `BundleManifestEntry` + `LoadBundleManifest`/
  `SaveBundleManifest` 自由函数；`AssetDatabase` 迁到 LMDB——`assets` 表 guid→value
  （header `type`/`path` + data 段 = 条目 XML 快照），`Resolve` 读 LMDB（故去 const），
  `ResolvedAsset` 去 `XmlElement::Node` 改拥有式 `Type`/`Data`，`FindByPath`/`AddEntry`/
  `SaveBundle` 语义不变。临时工作库落系统 temp、`Clear`/析构删除。测试矩阵重写为
  语义级（保序相关用例删除）。
- **M3**：`architecture/asset-database.md` 重写为 LMDB 现状契约；`overview.md` 索引行更新。
- 与 ADR-0038 的两处实现取舍：data 段本轮 = 条目元素自身的 XML 序列化（含未知属性与子
  节点，round-trip 无损；「xml 先不管」），编码仍可被后续系统替换；header 的 `path` 本轮
  保持 bundle 内相对路径（bundle 组织留待后续，届时再改 ADR-0036）。
- 未新增任何 `try`/`catch`/`throw`；LMDB 错误经 `LmdbResult` + `string& outError` 走。

## 后续轮次（2026-08-14）

用户指令「把 runtime bundle 相关先删除」，一并落地：

- 删除 `asset_bundle_manifest.h/.cpp`（`BundleManifestEntry` / `LoadBundleManifest` /
  `SaveBundleManifest` / D10 读写 helper）及其测试，移除 CMake 注册。
- `AssetDatabase` 去掉 bundle 概念与 XML 落盘：删 `Bundle` / `_bundles` / 嵌套检测 /
  `FindByPath(bundleName,..)` / `SaveBundle` / `AddEntry(bundleName,..)`；`path` 改为
  工程相对全局唯一，`AddEntry(relPath, type)` / `FindByPath(relPath)`；`Mount` 只开 LMDB
  工作库，`AddEntry` 登记 data 段为空。`ResolvedAsset` 保留 `Type` / `Data`（data 本轮恒空）。
- 因此本轮无 XML 持久快照，工作库是纯临时态（重 `Mount` 从空库开始）；ADR-0038 里
  「XML 落盘快照」的描述相应暂不成立，待后续轮次决定持久化形态。
- `NormalizeEntryPath` 从被删的 manifest 头迁入 `asset_database.cpp` 匿名命名空间。
- 随后又删掉 `AssetDatabase` 的 loader 系列（`LoaderFn` / `RegisterLoader` / `FindLoader` /
  `_loaders`）与 `LoadFromDatabase` 桥接：`AssetDatabase` 不再接触 `AssetManager`，
  头不再包含 `asset_manager.h` / `coroutine.h`，加载桥接待后续轮次重建。
- 再删 `_byPath` 与 `FindByPath`：`AssetDatabase` 退化为纯 GUID 键序库（`Mount` / `Resolve` /
  `AddEntry`），path 只是 value header 元数据，本轮不校验唯一性、无 path→guid 反查。
- 再改构造语义：`Mount` 改为构造函数 `AssetDatabase(assetRoot, storePath)`，失败抛
  `std::runtime_error`（不再走 `bool + outError`）；`storePath` 由调用方传入、析构不删，
  数据持久到下次打开（`ResolvedAsset::Data` 也改为 `vector<byte>`）。
- **放弃 bundle、先稳定 asset 系统（ADR-0039，2026-08-14）**：正式冻结 per-bundle 组织
  （bundle 目录、`bundle.xml`、bundle 名权威、嵌套检测、per-bundle 清单权威）为「放弃」，
  asset 身份登记定格为纯 GUID 键序 LMDB 库。当前阶段目标是稳定 asset 系统——`AssetManager`
  的生命周期/加载去重/延迟销毁与 `AssetDatabase` 的 LMDB 存取先做到可靠可测；XML 落盘快照、
  加载桥接、bundle 组织留待有真实消费方后再设计（ADR-0036 标记「部分被 ADR-0039 取代」，
  GUID 永久身份与双轨并存等身份规则保留）。
