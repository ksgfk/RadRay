> - 适用: 资产身份与元数据改用单份 JSON 清单的专项实施计划——`AssetDatabase` 重建、
>   `IAssetSource` 反转、强类型导入设置、example 端到端闭环
> - 权威: 本文是 grilling 对齐后的专项实施计划；决策 rationale 冻结在 ADR-0040/0041/0042，
>   实现完成后的现状描述以 `docs/architecture/asset-database.md` 为准
> - 状态: 已完成（2026-08-17）；当前契约与测试入口见 `docs/architecture/asset-database.md`
> - 锚点: `docs/adr/0040-single-text-manifest-is-asset-identity-authority.md`, `docs/adr/0041-load-bridging-belongs-to-asset-manager.md`, `docs/adr/0042-importers-are-interfaces-with-typed-settings.md`, `docs/adr/0039-abandon-bundle-organization-stabilize-asset-system.md`, `modules/runtime/include/radray/runtime/asset_database.h`, `modules/runtime/include/radray/runtime/asset_manager.h`, `modules/runtime/include/radray/runtime/application.h`, `modules/core/include/radray/json.h`, `examples/example_lambert_sphere/example_lambert_sphere.cpp`

# 资产数据库改用 JSON 清单（实施计划）

## 起点

工作树当前状态（**未提交的删除**，实施前先确认）：

- `modules/runtime/include/radray/runtime/asset_database.h` 已被清成空壳（只剩 `_assetRoot`
  成员），`src/asset_database.cpp` 是空文件。
- LMDB 全套已删：`modules/core/include/radray/lmdb.h`、`src/lmdb.cpp`、`tests/test_lmdb.cpp`，
  以及 `project_manifest.json` 的 lmdb 条目、根 `CMakeLists.txt` 的 `lmdb` 目标。
- `tests/test_asset_database.cpp` 已删。
- `docs/architecture/asset-database.md` 仍在描述那套已删除的 LMDB 实现——**是失效文档**，
  由本轮 M4 重写。

所以本轮是**重建**，不是增量修改。可复用的历史代码在 commit `6dac5623`（`NormalizeEntryPath`
与 `IsValidStoredPath` 的 path 口径实现）。

## 目标

给 asset 系统补一套**有真实消费方**的开发时身份登记设施：

- 身份权威是 `<AssetRoot>/assets.json`（ADR-0040）；资产根由装配方传入，代码不硬编码。
- 加载桥接归 `AssetManager`，`AssetDatabase` 实现 `IAssetSource`（ADR-0041）。
- 导入器是虚接口，导入设置强类型（ADR-0042）。
- **终点验收是 example 真的渲染出来**：贴图必须经 `AssetManager` 的 typed path load
  拿到并出现在画面上。每一条功能都要被这条路径压着，否则不进本轮。

前两轮（bundle 清单、LMDB）失败的共同原因是没有消费方，功能建好即废。本轮范围由消费方倒推。

## 工作术语

- **资产根（AssetRoot）**：装配方指定的资产目录。本仓库是**引擎根**而非游戏项目根，
  `assets/` 只是引擎自带的样例与测试资产；游戏项目传自己的资产根。
- **清单（manifest）**：`<AssetRoot>/assets.json`，随资产包版本化和分发，身份与元数据的唯一权威。
- **条目（entry）**：清单 `assets` 数组的一个元素，对应内存里一个 `AssetEntry`。
- **导入器（importer）**：一个 `AssetImporter` 实例，拥有一个资产类型的 type 名、扫盘扩展名、
  settings 形状与加载方式。
- **双轨 AssetId**：入库资产用清单 GUID；散文件（shaderlib、测试资源）继续
  `MakeAssetIdFromPath` 路径哈希。两轨并存、互不迁移（沿用 ADR-0008/0036）。

## 已确认需求

1. 消费方驱动：终点是 example 经数据库加载 `assets/` 下资产并渲染；范围由此倒推。
2. 身份权威是单份工程级 JSON 文本清单，随资产包版本化；不引入 LMDB 或任何本地数据库。
3. 清单位置 `<AssetRoot>/assets.json`；资产根由 `ApplicationRuntimeDescriptor` 传入，
   代码里不出现硬编码资产路径。
4. 加载桥接归 `AssetManager`；`AssetDatabase` 实现 `IAssetSource`，由 `Application` 拥有
   并在 phase 2 之后手工注入。
5. 导入设置强类型（`AssetImportSettings` 派生类）；未注册 type 的 settings 原文保真。
6. 导入器是虚接口，依赖构造时自持；不用函数指针，不用 `std::function`。
7. **不做导入产物缓存（DDC）**：loader 直接读源文件，每次运行重解 PNG/OBJ。
8. **本轮不做子资产寻址**：用一文件一资产的 `.png` / `.obj` 跑通闭环，glTF 留到 P2。
9. `Open` 失败返回 `unique_ptr` + `outError`，不抛异常（纠正 ADR-0038 落地时的
   `std::runtime_error` 构造）。
10. 文档（本文与 ADR-0040/0041/0042）先行；**用户明确指令前不开始实现**。

## 设计基线

### D1：清单 schema（version 1）

```json
{
  "version": 1,
  "assets": [
    {
      "guid": "8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b",
      "path": "textures/example.png",
      "type": "texture",
      "settings": { "srgb": true, "generateMips": true }
    },
    {
      "guid": "a12b3c4d-5e6f-4a70-8b9c-0d1e2f3a4b5c",
      "path": "meshes/example.obj",
      "type": "mesh"
    }
  ]
}
```

- 根对象只有 `version` 与 `assets`；`version != 1` 时 `Open` 硬失败。
- `guid` / `path` / `type` 必需；`settings` 可选（该 type 无设置时省略）。
- GUID 文本：写出固定 `Guid::ToString()` 的 D 格式小写；读取走 `Guid::TryParse`
  （宽容 N/D/B/P）。**禁用会抛异常的 `Guid::Parse`**。
- `type` 是随资产包发布的公开标识符，一旦发布不得重命名。

### D2：path 口径

存储形态：资产根相对、`/` 分隔、词法规范化。校验规则（违反即结构性错误）：

- 禁止 `\`、盘符（含 `:`）、绝对路径（开头 `/`）；
- 禁止 `..`、`.` 段、空段、尾斜杠；
- 唯一性按**大小写不敏感**校验（两个只差大小写的 path 在 Windows 上指向同一文件）。

`AddEntry` / `SetPath` 对输入宽容：接受 `\` 与 `/` 混用、重复分隔符、`.` 段的可解析 relPath，
内部规范化成存储形态；绝对路径、盘符、`..` 仍硬拒绝。落进清单的永远是规范形式。

实现复用 commit `6dac5623` 的 `NormalizeEntryPath` / `IsValidStoredPath`，语义描述从
"bundle 内相对"改为"资产根相对"。

### D3：错误分级

| 级别 | 情形 | 行为 |
|---|---|---|
| 结构性 | 坏 JSON；`version != 1`；缺 `guid`/`path`/`type`；guid 不可解析或为空；guid 重复；path 非法形态；path 大小写不敏感重复 | `Open` 硬失败，返回空指针 + `outError` |
| 内容性 | type 无注册 importer；path 在磁盘上不存在；settings 解析失败 | warning 放行，条目进索引 |

哲学沿用 ADR-0036 D3：身份不可信的清单任何宽容都把错误埋深；内容缺损放行给"先改清单、
后放文件"留活路。settings 解析失败归内容性——settings 属于内容而非身份，整库不因单个坏值拒载。

**注册顺序约束**：importer 必须在 `Open` **之前**注册完毕，否则清单里所有 type 都是"未注册"，
settings 全部退化为 `RawSettings`。这条要写进 `Open` 的注释。

### D4：AssetEntry 与强类型 settings

```cpp
class AssetImportSettings {
public:
    virtual ~AssetImportSettings() noexcept = default;
    virtual bool Deserialize(const JsonValue& json) = 0;
    virtual bool Serialize(JsonWriteContext& context) const noexcept = 0;
};

struct AssetEntry {
    AssetId Guid;
    string Path;                               // 资产根相对, 规范化
    string Type;
    unique_ptr<AssetImportSettings> Settings;  // 已解析; 无 settings 的类型为空
    string RawSettings;                        // 仅未注册 type / 解析失败时非空 (原文保真)
};

template <class T> Nullable<const T*> GetSettings(const AssetEntry& entry) noexcept;  // 经 dynamic_cast 判定
```

`unique_ptr` 成员让 `AssetEntry` 不可拷贝，故查询接口只能返回 `const AssetEntry*`。
**指针失效边界**：`unordered_map` 的引用在 rehash 后仍稳定，但 `RemoveEntry` 会让对应指针
失效——写进注释。

### D5：AssetImporter

```cpp
class AssetImporter {
public:
    virtual ~AssetImporter() noexcept = default;
    virtual std::string_view GetTypeName() const noexcept = 0;
    virtual std::span<const std::string_view> GetFileExtensions() const noexcept { return {}; }
    virtual unique_ptr<AssetImportSettings> CreateSettings() const { return nullptr; }

    /// 【本函数绝不能是协程】task 懒启动, 协程体要到首次 co_await 才跑, 那时 ctx 指向的
    /// 条目可能已被改动。必须同步读完 ctx 再把值传给真正的协程。见 ADR-0042。
    virtual task<AssetLoadResult> Load(const AssetLoadContext& ctx) = 0;
};

struct AssetLoadContext {
    std::filesystem::path AbsolutePath;
    const AssetImportSettings* Settings;  // 可空 (该 type 无设置)
};

template <class TSettings>
requires std::derived_from<TSettings, AssetImportSettings>
class TypedAssetImporter : public AssetImporter {
public:
    unique_ptr<AssetImportSettings> CreateSettings() const override { return make_unique<TSettings>(); }
    task<AssetLoadResult> Load(const AssetLoadContext& ctx) final;  // 取 typed settings 后按值转交
protected:
    /// 【两个形参按值】协程的引用形参只拷引用不拷对象。改成 const& 能编译、能过大多数
    /// 测试, 只在加载期间恰好发生 AddEntry 时炸。
    virtual task<AssetLoadResult> LoadTyped(std::filesystem::path path, TSettings settings) = 0;
};
```

依赖自持：`TextureImporter(FrameUploadScheduler&)`。`AssetLoadContext` 不含任何系统指针。

### D6：AssetDatabase API

```cpp
class AssetDatabase final : public IAssetSource {
public:
    /// 打开 <assetRoot>/assets.json。清单不存在时视为空库 (可 AddEntry + Save 建立)。
    /// 【importer 必须先注册】否则清单里的 type 全部视为未注册, settings 退化为 RawSettings。
    /// 失败返回 nullptr 并填 outError; 不抛异常。
    static unique_ptr<AssetDatabase> Open(
        const std::filesystem::path& assetRoot,
        vector<unique_ptr<AssetImporter>> importers,
        string& outError);

    // ─── 查询 (纯内存查表) ───
    const AssetEntry* Find(const AssetId& id) const noexcept;
    const AssetEntry* Find(std::string_view relPath) const noexcept;   // 大小写不敏感
    std::filesystem::path ResolvePath(const AssetEntry& entry) const;  // assetRoot / Path

    // ─── 登记与变更 ───
    std::optional<AssetId> AddEntry(std::string_view relPath, std::string_view type, string& outError);
    template <class T> T* MutableSettings(const AssetId& id) noexcept;
    bool SetPath(const AssetId& id, std::string_view newRelPath, string& outError);  // GUID 不变
    bool RemoveEntry(const AssetId& id) noexcept;

    /// 按 path 排序全量重写清单。不保序、不保留非条目内容。
    bool Save(string& outError) const;

    /// 扫资产根: 未登记且扩展名被 importer 认领的文件自动 AddEntry;
    /// 清单条目对应文件缺失时记 warning 但【保留条目与 GUID】。不自动 Save。
    bool Refresh(string& outError);

    // ─── IAssetSource ───
    std::optional<task<AssetLoadResult>> CreateLoadTask(const AssetId& id) override;
    std::optional<AssetId> ResolveId(std::string_view relPath) const override;
};
```

`Open` 收 importer 列表而非提供 `RegisterImporter`，是为了让 D3 的注册顺序约束在类型层面
无法违反。

两张索引：`unordered_map<AssetId, AssetEntry>` 与 `unordered_map<string, AssetId>`
（path 键存小写规范化形式）。

### D7：AssetManager 侧改动

本轮**唯一**对 `AssetManager` 的改动（显式推翻 ADR-0039 的"不改 AssetManager"，见 ADR-0041）：

```cpp
class IAssetSource {
public:
    virtual ~IAssetSource() noexcept = default;
    /// 【同步语义】所需数据必须在返回前全部取齐; 返回的 task 挂起后不得再回查本对象。
    virtual std::optional<task<AssetLoadResult>> CreateLoadTask(const AssetId& id) = 0;
    virtual std::optional<AssetId> ResolveId(std::string_view relPath) const = 0;
};

// AssetManager 新增
StreamingAssetRefAny Load(const AssetId& id);
template <class T> StreamingAssetRef<T> Load(const AssetId& id);
template <class T> StreamingAssetRef<T> Load(std::string_view relPath);
void SetAssetSource(IAssetSource* source) noexcept;   // 非拥有
```

- 既有 `Load(AssetLoadRequest)` 语义与实现不变；`test_asset_slot.cpp` 的既有用例零影响。
- source 为 nullptr 时 `Load(id)` 记 `RADRAY_ERR_LOG` 并返回无效引用，**不 abort**。
- 重载歧义已核：`AssetId` 是 `Guid`，与 `std::string_view`、`AssetLoadRequest` 之间无隐式
  转换路径，三个 `Load` 不打架。
- `SetAssetSource` **不进** `ServiceTraits<AssetManager>::Inject`——`ServiceRegistry::Wire`
  对解析不到的依赖是 `RADRAY_ABORT`（`service_registry.h:172`），而资产数据库是可选服务。

### D8：Application 装配与关停

```cpp
// ApplicationRuntimeDescriptor 新增 (与 RenderCachePath 同待遇)
/// 资产根目录。清单固定为 <AssetRoot>/assets.json。
/// 【空 = 不启用资产数据库】此时 AssetManager 只接受显式 AssetLoadRequest。
std::filesystem::path AssetRoot{};
```

phase 1（`application.cpp:872` 之后）：

```cpp
if (!desc.AssetRoot.empty()) {
    string error;
    _assetDatabase = AssetDatabase::Open(desc.AssetRoot, MakeDefaultImporters(...), error);
    if (_assetDatabase == nullptr) {
        RADRAY_ERR_LOG("open asset database failed: {}", error);  // 不 abort: 无资产也应能起
    }
}
```

phase 2（`registry.Wire()` 之后，`application.cpp:886`）：

```cpp
// AssetDatabase 是可选服务, 不进 ServiceTraits (Inject 解析不到会 ABORT)。
if (_assetDatabase != nullptr) {
    _assetManager->SetAssetSource(_assetDatabase.get());
}
```

关停（`application.cpp:816` 之后）：

```cpp
_assetManager.reset();
// AssetDatabase 持有 importer 与条目 settings, 在飞加载协程会引用它们,
// 故必须活过 AssetManager 的析构。
_assetDatabase.reset();
```

关停链变为 `World → RenderSystem → AssetManager → AssetDatabase → GpuSystem`。

### D9：本轮 importer

| type | 扩展名 | settings | 实现 |
|---|---|---|---|
| `texture` | `.png` `.jpg` `.jpeg` | `TextureImportSettings{ Srgb, GenerateMips }` | 解码 → 复用既有 GPU 上传路径产出 `TextureAsset` |
| `mesh` | `.obj` | 无（本轮无导入选项） | `WavefrontObjReader` → `MeshResource` → 复用 `LoadStaticMesh` |

`texture` importer 构造时接收 `FrameUploadScheduler&`。`mesh` importer 同样需要它
（`LoadStaticMesh` 的第一个形参）。

### D10：example 侧资产根

example 传的资产根是引擎样例资产目录，口径与测试一致（`modules/core/tests/test_img_rw.cpp:11`）：
先读 `RADRAY_ASSETS_DIR` 环境变量，编译期 `RADRAY_ASSETS_DIR_DEFAULT` 宏兜底。
`examples/example_lambert_sphere/CMakeLists.txt` 当前未注入该宏，需要加。

## 实施阶段与检查站

### M1：清单层与 AssetDatabase 骨架

1. `AssetImportSettings` 基类、`AssetEntry`、`GetSettings<T>`（D4）。
2. `AssetImporter` / `TypedAssetImporter` / `AssetLoadContext`（D5）。
3. 清单读写（D1）+ path 规范化校验（D2）+ 错误分级（D3）+ 双索引。
4. `Open` / `Find`×2 / `ResolvePath` / `AddEntry` / `SetPath` / `MutableSettings` /
   `RemoveEntry` / `Save`。
5. `AssetDatabaseTest`（见测试矩阵），不碰 GPU、不碰 `AssetManager`。

检查站：结构性错误全部返回 nullptr 且 `outError` 非空；`Save` → 重新 `Open` 索引一致；
未注册 type 的 settings 原文经一轮读写后逐字不变。

### M2：AssetManager 桥接

1. `IAssetSource` 接口 + `AssetManager::Load(AssetId)` / `Load<T>(AssetId)` /
   `Load<T>(relPath)` / `SetAssetSource`（D7）。
2. `AssetDatabase::CreateLoadTask` / `ResolveId`。
3. `test_asset_slot.cpp` 增补：假 source 的加载、source 缺失时返回无效引用、
   path 反查命中与未命中。

检查站：既有 `AssetSlotTest` 全部用例不变通过；新用例不需要 GPU。

### M3：importer 与 Refresh，端到端闭环

1. `TextureImporter` / `MeshImporter`（D9）。
2. `Refresh()`：扫盘登记 + 缺失文件 warning 保留 GUID。
3. `ApplicationRuntimeDescriptor::AssetRoot`、phase 1/2 装配、关停顺序（D8）。
4. example 接入（D10）：贴图经 `AssetManager` 加载并渲染出来；
   `<AssetRoot>/assets.json` 首次生成并随外部资产包交付。

检查站：**example 真的跑起来且画面上有那张贴图**（本轮的唯一终点验收）；
`Refresh` 两次运行 GUID 不变。

### M4：文档收尾

1. 重写 `docs/architecture/asset-database.md`（现状契约：清单 schema、path 口径、
   错误分级、importer 与 settings、装配与关停）。
2. `docs/architecture/asset-system.md`：AssetId 双轨段改指 JSON 清单；关停链加
   `AssetDatabase`。
3. `docs/architecture/overview.md` 索引行；`docs/adr/README.md` 记录表。
4. 全量 `ctest` 通过。

检查站：文档与实现同批提交（maintenance duty）；`docs/` 里不再有描述 LMDB 存储的现状文档。

## 测试矩阵

`modules/runtime/tests/`，经 `radray_add_test` 注册（套件名 = `ctest -R` 键）。

### AssetDatabaseTest（全程不碰 GPU 与 AssetManager）

- `Open`：清单不存在 → 空库；建索引后 `Find(id)` / `Find(path)` 命中与未命中；
  `ResolvePath` 拼出正确绝对路径。
- 结构性错误硬失败：坏 JSON、`version != 1`、缺 guid/path/type、guid 不可解析、
  guid 重复、path 大小写不敏感重复、path 非法形态（`\`、绝对、盘符、`..`、`.` 段、尾斜杠）。
- guid 读取宽容 N/B/P、写出固定 D 格式小写。
- 内容性缺损放行：文件缺失、未注册 type、settings 解析失败，三者都记 warning 且条目在索引中。
- **未注册 type 的 settings 原文保真**：`Open` → `Save` → 再 `Open`，settings 逐字不变。
- 强类型 settings：注册 importer 后 `GetSettings<T>` 命中；类型不符返回 nullptr；
  `MutableSettings` 改后 `Save` 生效。
- `AddEntry`：输入 `\` 分隔与 `.` 段自动规范化；新 GUID 非空且每次不同；绝对路径与 `..` 拒绝；
  撞 path 返回失败且 `outError` 含已有 GUID。
- `SetPath` 不改 GUID；`RemoveEntry` 后 `Find` 未命中。
- `Save`：按 path 排序；落盘 → 重新 `Open` → 索引与 `Find` 结果一致。
- `Refresh`：新文件自动登记；缺失文件保留条目与 GUID；连续两次 `Refresh` GUID 不变。

### AssetSlotTest 增补

- 假 `IAssetSource`：`Load(id)` 命中 → 得到 ready 引用；同 id 二次 `Load` 复用同一 slot。
- source 未装配 / id 未登记 / type 无 importer：返回无效引用并记错误日志，不 abort。
- `Load<T>(relPath)` 经 `ResolveId` 命中与未命中。

## 非目标

- 子资产寻址与 glTF importer（`assets/` 里两个 glTF 一文件含多 mesh/material/texture，
  需要 `AssetId + fragment` 一类的形状）——留 P2，靠 `version` 升级 schema。
- 导入产物缓存（DDC）、源文件 stamp、失效传播——留 P3。清单本轮连 stamp 字段都不留。
- 跨资产引用（材质持贴图 GUID）——留 P2。
- 按 type 枚举、依赖图、反向引用查询（编辑器 / 资产浏览器）——留 P3。
- 文件系统监控热重载——`Refresh()` 手动足够。
- per-bundle 组织（ADR-0039 已冻结为放弃）。
- 打包 / cook / 发布形态裁剪。
- LMDB 或任何二进制权威存储（ADR-0040 已排除）。
- 跨进程并发写；`AssetDatabase` 沿用 runtime 的单线程契约。

## 对齐记录

### 已确认
- **2026-08-17 / C1**：消费方驱动——终点是 example 经数据库加载 `assets/` 并渲染，
  范围由此倒推。前两轮（bundle 清单、LMDB）失败的共同原因是零消费方。
- **2026-08-17 / C2**：身份权威是单份工程级 JSON 文本清单，随资产包版本化；不引入 LMDB。
  用户明确接受"同文件冲突"的代价，实施以"按 path 排序 + 全量重写"把它压到平凡形态。
- **2026-08-17 / C3**：清单落 `<AssetRoot>/assets.json`。用户同时指出**本仓库是引擎根而非
  游戏项目根**，故资产根必须由装配方传入、代码不硬编码；引擎样例资产通过源码仓库之外的
  渠道分发。
- **2026-08-17 / C4**：不做导入产物缓存（DDC），loader 直接读源文件。
- **2026-08-17 / C5（用户提出）**：加载桥接不属于 `AssetDatabase`，是 `AssetManager` 的职责。
  落实为 `IAssetSource` 接口反转（ADR-0041），并因此推翻 ADR-0039 的"不改 AssetManager"。
- **2026-08-17 / C6（用户提出）**：`AssetEntry` 应当存解析后的配置数据。落实为强类型
  `AssetImportSettings` 派生类（ADR-0042），跳过"存已解析 JSON 树"的中间档——`JsonValue`
  是依附文档的 view，会把 ADR-0036 D8 的时序约束重新引进来。
- **2026-08-17 / C7（用户提出）**：`AssetTypeDescriptor` 不应用函数指针。落实为
  `AssetImporter` 虚接口，依赖构造时自持，`AssetLoadContext` 因此不含任何系统指针。
- **2026-08-17 / C8**：`AssetDatabase` 由 `Application` 拥有并走既有装配流程注入。因
  `ServiceRegistry::Wire` 对缺失依赖 `RADRAY_ABORT`，可选服务不进 `ServiceTraits::Inject`，
  改为 `Wire()` 之后手工注入一行。
- **2026-08-17 / C9**：本轮不做子资产寻址，用 `.png` / `.obj` 一文件一资产跑通闭环。
- **2026-08-17 / C10**：文档先行；**等待用户指令后再开始实现**。
