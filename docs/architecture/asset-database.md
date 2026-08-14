> - 适用: bundle.xml 清单、AssetBundleManifest、AssetDatabase 与 LoadFromDatabase 的现状契约
> - 权威: 本文是开发时资产持久化的唯一现状说明；决策 rationale 见 `adr/0036-per-bundle-manifest-is-asset-identity-authority.md` 与 `adr/0037-manifest-dom-is-backing-store.md`，生命周期与加载调度仍归 `architecture/asset-system.md`
> - 锚点: `modules/runtime/include/radray/runtime/asset_database.h`, `modules/runtime/src/asset_database.cpp`, `modules/runtime/include/radray/runtime/asset_bundle_manifest.h`, `modules/runtime/src/asset_bundle_manifest.cpp`, `CMakeLists.txt`

# 开发时资产持久化（bundle 清单与 AssetDatabase）

`AssetDatabase` 是开发时资产身份的登记门面：资产按 **bundle** 组织，每个 bundle 一份
`bundle.xml` 清单记录其内所有资产的 GUID、类型与相对路径，清单是身份的唯一权威。
`AssetManager` 不被改动，清单条目到 `AssetLoadRequest` 的桥接是自由函数 `LoadFromDatabase`。

## 磁盘布局与 schema

- bundle = 资产根下含 `bundle.xml` 的目录；**bundle 名 = 资产根下的相对目录路径**
  （`/` 分隔），目录名是唯一权威，XML 内没有 name 属性。资产根自身含 `bundle.xml` 时
  是名字为空串的 bundle。禁止嵌套：发现任一 bundle 目录是另一 bundle 目录的严格子孙
  即结构性错误。
- 根元素 `<bundle version="1">`，`version != 1` 即结构性错误。条目是 `<bundle>` 的直接
  子元素，**元素名即资产类型**（`image` / `mesh` / ...）。

```xml
<bundle version="1">
  <image guid="8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b" path="skybox.png">
    <bool name="srgb" value="true"/>
    <setting>
      <int name="lodBias" value="1"/>
    </setting>
  </image>
  <mesh guid="b23c4d5e-6f70-4a81-9cad-1e2f3a4b5c6d" path="hero.gltf"/>
</bundle>
```

- 清单层的身份契约只覆盖条目的**元素名、`guid`、`path`** 三样；其余属性与全部子节点
  原样保留，语义归各资产 loader。
- GUID 文本：**系统写出的 GUID 恒为 `Guid::ToString()` 的 D 格式**（小写、无花括号）——
  即 `AddEntry` 的条目 guid 与 `WriteGuid` helper 的值；读取经 `Guid::TryParse` 宽容接受
  N/D/B/P。手写清单里未被触碰的 guid 文本按 DOM 常驻原样保留（ADR-0037 的逐字节保证
  优先）。不经过会抛异常的 `Guid::Parse`。
- `path` 是 bundle 内相对路径；bundle 只管辖自己目录（含子目录）内的文件，条目绝对路径
  = 资产根 / bundle 目录 / path。`Mount` 会把 assetRoot 归一为绝对路径，因此
  `ResolvedAsset::AbsolutePath` 恒为绝对路径（调用方传相对根亦可）。

## path 口径

存储形态永远是 `/` 分隔、词法规范化后的 bundle 内相对路径：无 `\`、无盘符（含 `:`）、
非绝对路径（无开头 `/`）、无 `..`、无开头 `./`、无 `.` 段、无空段、无尾斜杠。违反任一条
即结构性错误。bundle 内 path 唯一性按**大小写不敏感**校验（两个只差大小写的条目在
Windows 上指向同一文件），查找同样按大小写不敏感口径。

`AddEntry` 对输入宽容：接受 `\` 与 `/` 混用、重复分隔符与 `.` 段的可解析 relPath，
经 `NormalizeEntryPath` 规范化成存储形态再写入 DOM；绝对路径、盘符与 `..` 仍硬拒绝。

## 错误分级

| 级别 | 情形 | 行为 |
|---|---|---|
| 结构性错误 | 嵌套 bundle；GUID 跨 bundle 重复；path bundle 内重复（大小写不敏感）；非法 path 形态；坏 XML；`version != 1` | `Mount` 整体硬失败，索引为空 |
| 内容性缺损 | 条目 type 无注册 loader；条目 path 在磁盘上不存在 | warning，条目照常进索引 |

哲学：身份不可信的清单任何宽容都会把错误埋深；内容缺损放行给"先改清单、后放文件"留活路。

## AssetId 双轨与身份规则

- 入库资产：GUID 由 `AddEntry` 调用 `Guid::NewGuid()` 一次分配，写入清单后永不改变。
- 散文件（shaderlib、测试资源）：继续 `MakeAssetIdFromPath` 路径哈希（ADR-0008），不迁移。
- 移动/重命名 = 人改清单 `path`，GUID 不变；不存在任何自动改 GUID 的代码路径。
- 两轨共用 `AssetManager` 的单一 slot 表，互不迁移。

## DOM 常驻与写回（ADR-0037）

`AssetBundleManifest` 把 pugixml DOM 文档本身作为后备存储常驻内存：读取 = 在 DOM 上建
guid / path → 节点的索引；写回 = 序列化同一份 DOM。解析标志必含 `pugi::parse_comments`
与 `pugi::parse_ws_pcdata`（pugixml 默认丢注释、丢纯空白文本节点），落盘用
`pugi::format_raw | pugi::format_no_declaration`（不注入任何缩进、原文件无声明时不凭空
补声明）——未触碰节点的顺序、内容与注释逐字节保持。三个序列化事实：空元素统一自闭合为
`/>`；行尾归一化为 `\n`；根元素之后的尾随空白不在 DOM 内、写回不保留。

`AddEntry` 恒定把新条目追加到 `<bundle>` 末尾，不做排序插入：对既有节点零扰动，两个分支
同时追加时的 merge 冲突是"两侧都保留"即可的平凡形态。写盘出口只有 `SaveBundle`；初版
不做目录扫描 Sync 与文件监控。

## 条目子节点的值编码（D10）

子节点的**语义**完全归 loader，但值的**编码形态**统一：

- 叶子元素：元素名是基础类型（`string` / `int` / `float` / `bool` / `guid`），
  `name` 属性是字段名，`value` 属性是值。文本口径：`int` 十进制；`float` 保 round-trip
  的最短表示；`bool` 小写 `true`/`false`；`guid` 写回 D 格式、读取 `Guid::TryParse`
  宽容 N/D/B/P。
- 非叶子元素：元素名直接就是字段名（如 `<setting>`），内部再嵌叶子或更深复合。
- 同名元素重复出现即列表，不引入 `<list>` 包装元素。
- 读写走清单层 helper（`ReadInt` / `WriteInt` / `ReadStringList` 等，见
  `asset_bundle_manifest.h`），loader 不手扒 `pugi::xml_node`。坏值由 helper 按"读不到"
  处理（nullopt / 列表跳过）。
- **Mount 不校验子节点结构**：子节点属于内容而非身份，整库不因单个坏值拒载；坏值在
  加载时由 loader 报错（内容性缺损级）。

## AssetDatabase API

```cpp
bool Mount(const std::filesystem::path& assetRoot, string& outError);
std::optional<ResolvedAsset> Resolve(const AssetId& id) const noexcept;
std::optional<AssetId> FindByPath(std::string_view bundleName, std::string_view relPath) const noexcept;
std::optional<AssetId> AddEntry(std::string_view bundleName, std::string_view relPath,
                                std::string_view type, string& outError);
bool SaveBundle(std::string_view bundleName, string& outError);
void RegisterLoader(string type, LoaderFn loader);
```

索引两张：`AssetId → (bundle, 条目节点)` 与 `(bundleName, relPath) → AssetId`（path 键
按大小写不敏感口径）。运行时查询是纯查表，不碰磁盘。loader 注册表是实例成员，进程装配
代码显式逐个注册（无静态自动注册），**先 RegisterLoader、后 Mount**——Mount 时未注册的
type 记 warning。撞 path 的 `AddEntry` 返回失败，错误信息附带已有条目的 GUID。

## 加载桥接

```cpp
StreamingAssetRefAny LoadFromDatabase(AssetManager& manager, const AssetDatabase& db, const AssetId& id);
```

唯一同时认识 `AssetDatabase` 与 `AssetManager` 的地方：按条目元素名查注册表，构造
`task<AssetLoadResult>`，`AssetLoadRequest::Id` 用清单 GUID，提交给 `manager.Load`。
`asset_database.h` 为桥接签名包含 `asset_manager.h`，依赖是单向的（`asset_manager.h`
不认识 `AssetDatabase`），两者各自单一职责（清单身份 / 生命周期）。

## 线程约束（D8）

DOM 节点访问（包括 loader 解析自定义子节点）**只允许发生在构造 `AssetLoadRequest` 的
主线程时刻**。loader 协程一旦挂起去做异步 IO，就不得再碰 DOM——`AddEntry` 可能并发修改
它；所需参数必须在构造 task 前拷出。这条与 `AssetManager` 既有的单线程契约同级。

## XML 依赖

pugixml v1.16 由 `project_manifest.json` 声明、`tools/fetch_third_party.py` 拉取，根
`CMakeLists.txt` 照 yyjson 的方式接入（`PUGIXML_NO_XPATH` / `PUGIXML_NO_EXCEPTIONS` 均开启，
对齐仓库异常规则）。pugixml 的 include 与链接被收进 `radraycore` 的 PRIVATE 依赖，下游不再
直接接触 `<pugixml.hpp>`；core 用 `radray/xml.h` 的 `XmlDocument` / `XmlElement` /
`XmlAttribute` / `XmlNode` 封装了 DOM（公开头只前置声明 pugixml 类型）。runtime 的
`AssetBundleManifest` / `ResolvedAsset` 公开签名改用它——清单常驻存储是 `XmlDocument`，
条目节点是 `XmlElement`，loader 经 `XmlElement` 与 D10 读写 helper 访问子节点。
