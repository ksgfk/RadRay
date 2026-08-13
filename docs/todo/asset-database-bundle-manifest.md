> - 适用: 开发时资产持久化——per-bundle XML 清单（`bundle.xml`）、`AssetDatabase` 门面与 `AssetManager` 桥接的专项实施计划
> - 权威: 本文是 grilling 对齐后的专项实施计划；决策 rationale 冻结在 ADR-0036/0037，实现完成后的现状描述以 `docs/architecture/` 为准
> - 状态: 已完成（2026-08-13；M0–M3 全部落地并全量构建/测试通过）。实现后的现状契约以
>   `docs/architecture/asset-database.md` 为准；下文是冻结的实施计划与对齐记录。
> - 锚点: `docs/adr/0036-per-bundle-manifest-is-asset-identity-authority.md`, `docs/adr/0037-manifest-dom-is-backing-store.md`, `docs/architecture/asset-system.md`, `modules/runtime/include/radray/runtime/asset_manager.h`, `modules/runtime/include/radray/runtime/asset.h`, `modules/core/include/radray/guid.h`, `project_manifest.json`, `CMakeLists.txt`

# 开发时资产持久化（bundle 清单与 AssetDatabase）计划

## 目标

给 runtime 资产系统补一套开发时持久化身份设施：

- 资产按 **bundle**（资产根下含 `bundle.xml` 的目录）组织；清单记录 bundle 内所有资产的
  GUID、类型与相对路径，是资产身份的唯一权威（ADR-0036）。
- 新增 `AssetBundleManifest`（单清单的 DOM 常驻表示与读写，ADR-0037）与
  `AssetDatabase`（Mount 扫描、索引、查询、登记、落盘的开发时门面），全部落在
  `modules/runtime`，不单开模块。
- **不改动 `AssetManager`**。清单条目到 `AssetLoadRequest` 的桥接是自由函数
  `LoadFromDatabase`；`AssetLoadRequest::Id` 直接使用清单 GUID。
- 引入 pugixml（唯一新增第三方依赖）。

格式选 XML 的动机是 **git merge 冲突集中且易解**，这条动机约束了写回策略（见 D5）。

## 工作术语

- **bundle**：资产根下含 `bundle.xml` 的目录。bundle 名 = 资产根下的相对目录路径，
  目录名是唯一权威，XML 内不写 name。禁止嵌套。
- **清单（manifest）**：`bundle.xml` 的常驻 DOM 及其上的索引。
- **条目（entry）**：`<bundle>` 的直接子元素。**元素名即资产类型**（`image` / `mesh` / ...）。
- **双轨 AssetId**：入库资产的 GUID 以清单为准（`Guid::NewGuid()` 一次分配、永不改变）；
  未入库散文件（shaderlib、测试资源）继续走 `MakeAssetIdFromPath` 路径哈希。两轨并存、互不迁移。

## 已确认需求

1. per-bundle 单一 XML 清单，不做 Unity 式 per-file meta；动机是合并冲突集中可解。
2. 目录含 `bundle.xml` 即 bundle；发现嵌套 bundle 时 Mount 整体硬失败。
3. bundle 只管辖自己目录（含子目录）内的文件；`path` 是 bundle 内相对路径。
4. GUID 是资产的永久身份：移动/重命名不换 GUID，仍是同一资产；path 是可变元数据，
   移动 = 修改清单里的 `path`。
5. 初版不做目录扫描 Sync 与文件监控；入库走显式 `AddEntry`，落盘走显式 `SaveBundle`。
6. 写回不得改变未触碰节点的顺序，注释必须保留（merge 友好的机械保证）。
7. 条目子节点的**语义**由各资产 loader 自行解析，但基础值有统一编码（D10）：叶子元素名是
   基础类型、`name` 属性是字段名；非叶子元素名即字段名。清单层提供读写 helper，Mount 不校验。
8. 单资产根；多根等真实场景出现再设计。
9. 未注册 type、清单指向的文件缺失：warning 放行；结构性错误：Mount 硬失败（分级见 D3）。
10. 文档（本 TODO 与 ADR-0036/0037）先行；**用户明确指令前不开始实现**。

## 设计基线

### D1：磁盘布局与 schema

状态：**已确认（2026-08-12）**。

```text
assets/                      ← 单一资产根（路径由装配代码提供）
  env/                       ← 一个 bundle = 一个目录，bundle 名 = "env"
    bundle.xml
    skybox.png
    terrain/rock.png
  characters/props/          ← bundle 名 = "characters/props"
    bundle.xml
    hero.gltf
```

```xml
<bundle version="1">
  <image guid="8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b" path="skybox.png"/>
  <image guid="a12b3c4d-5e6f-4a70-8b9c-0d1e2f3a4b5c" path="terrain/rock.png">
    <bool name="srgb" value="true"/>   <!-- 叶子元素：元素名=基础类型，name=字段名（D10） -->
    <setting>                          <!-- 非叶子元素：元素名即字段名，内部再嵌叶子或更深复合 -->
      <int name="lodBias" value="1"/>
    </setting>
  </image>
  <mesh guid="b23c4d5e-6f70-4a81-9cad-1e2f3a4b5c6d" path="hero.gltf"/>
</bundle>
```

- 根元素 `<bundle>` 只带 `version`；`version != 1` 时 Mount 硬失败。
- 清单层的契约只覆盖条目的**元素名、`guid`、`path`** 三样；其余属性与全部子节点原样保留。
  子节点的语义归 loader，基础值的统一编码见 D10。
- GUID 文本：写回固定 `Guid::ToString()` 的 D 格式（小写、无花括号）；读取经
  `Guid::TryParse` 宽容接受 N/D/B/P。**禁用会抛异常的 `Guid::Parse`**（仓库异常规则）。

### D2：path 口径

状态：**已确认（2026-08-12）**。

存储形态永远是：`/` 分隔、词法规范化后的 bundle 内相对路径。Mount 时违反任一条即硬失败：

- 禁止 `\`、盘符与绝对路径；
- 禁止 `..` 与开头的 `./`；
- bundle 内 path 唯一性按**大小写不敏感**校验（两个只差大小写的条目在 Windows 上指向同一文件）。

`AddEntry` 对输入宽容：接受 `\` 与 `/` 混用的可解析 relPath，内部规范化成存储形态再写入 DOM；
绝对路径与 `..` 仍硬拒绝。落进 XML 的永远是规范形式。

### D3：错误分级

状态：**已确认（2026-08-12）**。

| 级别 | 情形 | 行为 |
|---|---|---|
| 结构性错误 | 嵌套 bundle；GUID 全局（跨 bundle）重复；path bundle 内重复（大小写不敏感）；非法 path 形态；坏 XML；`version != 1` | Mount 整体硬失败，索引为空 |
| 内容性缺损 | 条目 type 无注册 loader；条目 path 在磁盘上不存在 | warning，条目照常进索引 |

哲学：身份不可信的清单任何宽容都会把错误埋深；内容缺损放行给"先改清单、后放文件"留活路。

### D4：AssetId 双轨与身份规则

状态：**已确认（2026-08-12）**。

- 入库资产：GUID 由 `AddEntry` 调用 `Guid::NewGuid()` 一次分配，写入清单后永不改变。
- 散文件（shader、测试资源）：继续 `MakeAssetIdFromPath`（ADR-0008），不迁移。
- 移动/重命名 = 人改清单 `path`，GUID 不变；不存在任何自动改 GUID 的代码路径。
- 路径哈希（version/variant 位非随机）与 `NewGuid`（v4 随机）碰撞概率可忽略，两轨共用
  `AssetManager` 的单一 slot 表。

### D5：DOM 常驻与顺序保持写回（ADR-0037）

状态：**已确认（2026-08-12）**。

- `AssetBundleManifest` 把 pugixml DOM 文档本身作为后备存储常驻内存：读取 = 在 DOM 上建
  guid/path → 节点的索引；写回 = 序列化同一份 DOM。
- 解析标志必须含 `pugi::parse_comments`（pugixml 默认丢注释）。
- 未触碰节点写回后顺序与内容不变；注释保留。
- `AddEntry` 恒定追加到 `<bundle>` 末尾，不做排序插入（对既有节点零扰动；两分支同时追加时
  的冲突是"两侧都保留"即可的平凡形态）。

### D6：AssetDatabase API 形状

状态：**已确认（2026-08-12）**。实施时允许微调签名，不允许改变语义。

```cpp
class AssetDatabase {
public:
    // 扫描 assetRoot 下所有 bundle.xml，按 D3 分级校验后建索引。失败时索引为空。
    bool Mount(const std::filesystem::path& assetRoot, string& outError);

    struct ResolvedAsset {
        std::filesystem::path AbsolutePath;  // assetRoot / bundle 目录 / path
        std::string_view Type;               // 条目元素名
        pugi::xml_node Node;                 // 条目节点；子节点由 loader 自行解析（见 D8 约束）
    };
    std::optional<ResolvedAsset> Resolve(const AssetId& id) const noexcept;
    std::optional<AssetId> FindByPath(std::string_view bundleName, std::string_view relPath) const noexcept;

    // 登记新资产：规范化 relPath（D2）、分配 NewGuid、追加 DOM 末尾（D5）、更新索引。
    // 目标 path 已有条目时返回失败，错误信息附带已有条目的 GUID（调用方要幂等语义自己拼）。
    std::optional<AssetId> AddEntry(std::string_view bundleName, std::string_view relPath,
                                    std::string_view type, string& outError);

    // 把指定 bundle 的 DOM 落盘。初版唯一的写盘出口。
    bool SaveBundle(std::string_view bundleName, string& outError);

    // loader 注册表是实例成员，进程装配代码显式逐个注册；无静态自动注册。
    using LoaderFn = task<AssetLoadResult> (*)(const ResolvedAsset&);
    void RegisterLoader(string type, LoaderFn loader);
};
```

索引两张：`AssetId → (bundle, 条目节点)` 与 `(bundleName, relPath) → AssetId`（path 键按
大小写不敏感口径比较）。运行时查询是纯查表，不碰磁盘。

### D7：加载桥接

状态：**已确认（2026-08-12）**。

```cpp
// 唯一同时认识 AssetDatabase 与 AssetManager 的地方。按条目元素名查注册表，
// 构造 task<AssetLoadResult>，AssetLoadRequest::Id 用清单 GUID，提交给 manager.Load。
StreamingAssetRefAny LoadFromDatabase(AssetManager& manager, const AssetDatabase& db, const AssetId& id);
```

`AssetDatabase` 与 `AssetManager` 头文件互不包含；两者各自单一职责（清单/生命周期）。

### D8：线程与时序约束

状态：**已确认（2026-08-12）**。

DOM 节点访问（包括 loader 解析自定义子节点）**只允许发生在构造 `AssetLoadRequest` 的主线程
时刻**。loader 协程一旦挂起去做异步 IO，就不得再碰 DOM（`AddEntry` 可能并发修改它）；
所需参数必须在构造 task 前拷出。这条与 `AssetManager` 既有的单线程契约同级，写进代码注释与
架构文档。

### D9：pugixml 依赖落地

状态：**已确认（2026-08-12，库选型定稿 pugixml）**。

- `project_manifest.json` 的 `ThirdParties` 数组加条目：
  `{"Name": "pugixml", "Type": "git", "Git": "https://github.com/zeux/pugixml.git", "Tag": "v1.15"}`。
- 根 `CMakeLists.txt` 照 yyjson 的样子接入：前置 cache 变量关掉上游测试后
  `add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/pugixml)`，再
  `radray_optimize_flags_library` + `radray_set_build_path`。
- `modules/runtime` PRIVATE 链接；`pugi::xml_node` 出现在 `ResolvedAsset` 中，头文件暴露方式
  实施时确认（前置声明或受控 include）。
- 本设计不使用 XPath；实施时确认上游 CMake 是否暴露 `PUGIXML_NO_XPATH` /
  `PUGIXML_NO_EXCEPTIONS` 开关，可关则关，对齐仓库异常规则。

### D10：条目子节点的基础值编码

状态：**已确认（2026-08-12，修正 C9 的"完全无结构"表述）**。

子节点的**语义**仍完全归 loader，但值的**编码形态**统一：

- 叶子元素：元素名是**基础类型**，`name` 属性是字段名，`value` 属性是值。初版类型集合五种：
  `string` / `int` / `float` / `bool` / `guid`。
  例：`<bool name="srgb" value="true"/>`、`<int name="foo" value="1"/>`。
- 文本口径：`int` 十进制；`float` 保 round-trip 的最短表示；`bool` 小写 `true`/`false`；
  `guid` 写回 D 格式、读取 `Guid::TryParse` 宽容 N/D/B/P（与条目 `guid` 属性同口径，
  为将来跨资产引用预留统一载体）。
- 非叶子元素：元素名直接就是**字段名**（如 `<setting>`），内部再嵌叶子或更深复合。
  清单层不理解"结构类型"概念，`setting` 里有什么由 loader 决定。
- **同名元素重复出现即列表**：不引入 `<list>` 包装元素，helper 提供"读全部同名项"的形式。
- 清单层提供一组读写 helper（自由函数，如 `ReadInt(node, name)` / `WriteInt(node, name, value)`
  及列表形式），loader 经 helper 访问而不是手扒 `pugi::xml_node`。
- **Mount 不校验子节点结构**：坏值在加载时由 loader 报错（内容性缺损级），维持 D3 分级
  哲学——子节点属于内容而非身份，整库不因单个坏值拒载。

## 实施阶段与检查站

### M0：依赖引入

1. manifest 加 pugixml 条目，`python tools/fetch_third_party.py restore` 拉取。
2. 根 CMake 接入（D9），空跑一次全量构建确认无破坏。

检查站：构建通过；`third_party/pugixml` 由脚本管理，未手改。

### M1：AssetBundleManifest

1. DOM 常驻的 Load/Save（D5），解析标志含 `parse_comments`。
2. schema 校验（version、元素名、guid、path 口径 D1/D2）。
3. 基础值编码的读写 helper（D10）：五种叶子类型 + 列表形式。
4. `AssetBundleManifestTest`（见测试矩阵）。

检查站：round-trip 测试证明未触碰节点顺序与注释逐字节不变；全部坏格式路径拒载。

### M2：AssetDatabase

1. Mount 扫描 + 嵌套检测 + D3 分级校验 + 双索引。
2. `Resolve` / `FindByPath` / `AddEntry` / `SaveBundle`。
3. `AssetDatabaseTest`（见测试矩阵）。

检查站：结构性错误全部硬失败且索引为空；`SaveBundle` 落盘后重新 Mount 结果一致。

### M3：加载桥接与文档

1. loader 注册表（D6）与 `LoadFromDatabase`（D7）。
2. 新增 `docs/architecture/asset-database.md`（三行头；schema、path 口径、错误分级、
   DOM 常驻与 D8 约束）；`docs/architecture/overview.md` 索引加行；
   `docs/architecture/asset-system.md` 补 AssetId 双轨边界并指向新文档。
3. 全量 `ctest` 通过。

检查站：文档与实现同批提交（maintenance duty）；`AssetManager` 无任何改动。

## 测试矩阵

`modules/runtime/tests/`，经 `radray_add_radray_gtest_case` 注册，全程不碰 GPU 与 `AssetManager`。

### AssetBundleManifestTest

- DOM round-trip：未触碰节点顺序不变、注释保留、loader 自定义子节点与未知属性原样保留。
- 坏 XML / `version != 1` / 缺 guid / 缺 path 拒载。
- guid 读取宽容 N/B/P、写回固定 D 格式小写。
- path 校验：`\`、绝对路径、盘符、`..`、开头 `./` 均拒载。
- 基础值 helper（D10）：五种叶子类型读写 round-trip；`guid` 值宽容读取/D 格式写回；
  同名重复元素按列表读全；坏值（如 `int` 里放非数字）helper 返回失败而非 Mount 拒载；
  非叶子元素（字段名元素）内嵌叶子的读取。

### AssetDatabaseTest

- Mount 建索引；`Resolve` 拼出正确绝对路径；`FindByPath` 命中与未命中。
- 结构性错误硬失败：嵌套 bundle、GUID 跨 bundle 重复、path 大小写不敏感重复。
- 内容性缺损放行：文件缺失、未注册 type 记 warning 且条目在索引中。
- `AddEntry`：输入 `\` 分隔自动规范化；新 GUID 分配且非空；撞 path 返回失败且错误信息含
  已有 GUID；节点追加在 `<bundle>` 末尾且不扰动既有节点。
- `SaveBundle` 落盘 → 重新 Mount → 索引与 Resolve 结果一致。

## 非目标

- 目录扫描 Sync、Missing 条目自动处置、文件监控热更新。
- bundle 嵌套、多资产根、跨 bundle 依赖声明。
- 打包/压缩成运行时二进制 bundle、发布形态裁剪。
- 移动重命名的自动 GUID 保持（内容哈希匹配）——身份规则已由"人改 path"覆盖。
- 导入产物缓存（DDC 类设施）；loader 仍直接消费源文件。
- 排序、格式化等任何会移动未触碰节点的清单"规范化"。

## 对齐记录

### 已确认
- **2026-08-12 / C1**：格式选 XML，动机是 merge 冲突集中易解；允许新增第三方依赖。
- **2026-08-12 / C2**：嵌套 bundle = Mount 硬失败；目录名是 bundle 名权威，XML 无 name 属性。
- **2026-08-12 / C3**：GUID 写回固定 D 格式；读取 `TryParse` 宽容 N/D/B/P；禁用抛异常的 `Parse`。
- **2026-08-12 / C4**：AssetId 双轨并存互不迁移；bundle 只管自己目录内的文件。
- **2026-08-12 / C5**：写回不改未触碰节点顺序——DOM 常驻为后备存储，注释保留（经确认可行）。
- **2026-08-12 / C6**：初版不做 Sync，只保留 `SaveBundle` 接口。
- **2026-08-12 / C7**：移动/重命名不换 GUID；GUID 是身份，path 是可变元数据。
- **2026-08-12 / C8**：未注册 type 与文件缺失 warning 放行；GUID 重复与 path 重复硬失败。
- **2026-08-12 / C9（被 C17 部分修正）**：条目元素名即资产类型（`<image .../>`）；子节点由
  loader 自行解析。"完全不规定形状"的表述被 C17 修正为"语义归 loader，值编码统一"。
- **2026-08-12 / C10**：代码放 `modules/runtime`，不单开模块。
- **2026-08-12 / C11**：入库走 `AddEntry`（宽容输入自动规范化、`NewGuid` 分配、追加末尾）；
  撞 path 返回失败并附带已有条目 GUID。
- **2026-08-12 / C12**：schema 定稿 `<bundle version="1">`，`version != 1` 硬失败；path
  规范形态（`/` 分隔、无绝对、无 `..`、无开头 `./`）。
- **2026-08-12 / C13**：loader 注册表是 `AssetDatabase` 实例成员显式注册；桥接为自由函数
  `LoadFromDatabase`。
- **2026-08-12 / C14**：loader 直接消费 `pugi::xml_node`；DOM 访问仅限构造 task 前的主线程时刻。
- **2026-08-12 / C15**：单资产根；bundle 名 = 根下相对目录路径。
- **2026-08-12 / C16**：XML 库定稿 pugixml（Tag `v1.15`，`parse_comments` 必开）；ADR-0036/0037
  据此接受。文档先行，**等待用户指令后再开始实现**。
- **2026-08-12 / C17（修正 C9）**：子节点语义归 loader，值编码统一（D10）：叶子元素名是基础
  类型（`string`/`int`/`float`/`bool`/`guid`）、`name` 属性是字段名；非叶子元素名即字段名；
  同名元素重复即列表，不引入 `<list>` 包装；清单层提供读写 helper；Mount 不校验子节点结构，
  坏值由 loader 在加载时报错。

## 落地记录（2026-08-13）

- 代码落点：`AssetBundleManifest`（`modules/runtime/include/radray/runtime/asset_bundle_manifest.h`）
  与 `AssetDatabase` / `LoadFromDatabase`（`asset_database.h`），实现与测试在
  `src/asset_bundle_manifest.cpp`、`src/asset_database.cpp`、`tests/test_asset_bundle_manifest.cpp`、
  `tests/test_asset_database.cpp`。现状文档 `docs/architecture/asset-database.md` 已同批提交。
- 写回口径的三个实施确认（都在 `Save` 注释里）：解析标志需含 `parse_ws_pcdata`（pugixml 默认
  丢纯空白文本节点）；落盘需 `format_no_declaration`（原文件无声明时不凭空补）；根元素之后的
  尾随空白不在 DOM 内、写回不保留。空元素统一自闭合 `/>`。
- GUID「写回固定 D 格式」的口径收敛为：系统写出的 GUID（`AddEntry` 条目、`WriteGuid` helper）
  恒为 D 格式小写；手写清单里未被触碰的 guid 文本按 DOM 常驻原样保留（ADR-0037 的逐字节保证
  优先于任何规范化）。
- 头文件暴露方式（D9 待确认项）定为「受控 include」：只有 asset 的两个公开头 include
  `pugixml.hpp`；接入落点是 `radraycore` PUBLIC 链接 `pugixml-static`（include 路径与宏
  口径随 core 传给全部下游模块），依赖版本定稿 v1.16。
- 测试注册用 `radray_add_test`（套件名 `AssetBundleManifestTest` / `AssetDatabaseTest` 与
  `ctest -R` 匹配），未启用 `radray_add_radray_gtest_case`——仓库现状没有调用方，`radray_add_test`
  的 gtest_discover 已按套件名注册全部用例。
- 未新增任何 `try`/`catch`/`throw`；pugixml 经 `PUGIXML_NO_EXCEPTIONS` / `PUGIXML_NO_XPATH`
  关闭异常与 XPath。
