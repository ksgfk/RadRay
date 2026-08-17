# ADR-0042 导入器是虚接口，导入设置强类型化

状态: 生效
日期: 2026-08
影响: `modules/runtime` 的 `AssetImporter` / `AssetImportSettings` / `AssetEntry` /
`AssetDatabase`；全部资产 importer 实现；取代 ADR-0038 关于 data 段编码归属的部分

## 背景

ADR-0036 D10 让清单条目的子节点由各 loader 自行解析 `pugi::xml_node`，并因此背上 D8 那条
线程约束："DOM 节点访问只允许发生在构造 `AssetLoadRequest` 的主线程时刻，loader 协程一旦挂起
就不得再碰 DOM"。ADR-0038 换成 LMDB 后把导入设置降为 opaque data 段，编码"完全由 loader
自行决定"——线程雷消失了，但代价是没有任何统一形状：每个 loader 手写字节解析，没有工具能读写
它，坏值的错误路径各写一遍。

同时 loader 的注册单元形状也待定。函数指针（`task<AssetLoadResult> (*)(const AssetLoadContext&)`）
是最轻的选择，但它不能持有状态：贴图 importer 需要 `FrameUploadScheduler`，未来的 glTF
importer 需要 `AssetManager` 做嵌套加载。这些依赖只能塞进 `AssetLoadContext`，于是那个 struct
会随每个新 importer 的需求单调膨胀，且所有 importer 都被迫看见彼此的依赖。

## 决策

**导入器是虚接口 `AssetImporter`（实现自持依赖）；导入设置是强类型 `AssetImportSettings`
派生类，由 importer 声明形状、经 `JsonSerializer` / `JsonDeserializer` 编解码。**

- `AssetImporter` 拥有一个资产类型的全部知识：`GetTypeName()`（清单里的 `type` 字符串）、
  `GetFileExtensions()`（`Refresh` 扫盘认领的扩展名）、`CreateSettings()`（settings 工厂，
  返回空表示该类型无设置）、`Load(const AssetLoadContext&)`。
- importer 的依赖（`FrameUploadScheduler` 等）在**构造时**接收并自持。`AssetLoadContext`
  因此收缩为 `{ AbsolutePath, const AssetImportSettings* Settings }`，不夹带任何系统指针。
- `AssetEntry::Settings` 是 `unique_ptr<AssetImportSettings>`，在清单载入时就已解析成强类型
  对象。它是纯 CPU 值，可自由拷入加载协程——**没有任何 DOM 生命周期或线程时序约束**（这是
  相对 ADR-0036 D8 的实质改善）。
- `AssetImportSettings` 用 `RuntimeTypeTrait` 表达类型身份（与 `Asset` 同一模式）；
  `GetSettings<T>()` 经 `RuntimeTypeInfo::IsA` 判定，不符返回 `nullptr`。
- **`AssetImporter::Load` 不得自身是协程。** `task<T>` 懒启动，若 `Load` 是协程则函数体要到
  第一次 `co_await` 才执行，那时 `ctx` 指向的条目可能已被 `AddEntry` / `RemoveEntry` 改动。
  `Load` 必须是普通函数：同步读完 `ctx`，把值传给真正的协程。
- 提供 `TypedAssetImporter<TSettings>` 把这条约束固化：它 `final` 实现 `Load`（取 typed
  settings 后按值转交），子类只实现 `LoadTyped(std::filesystem::path path, TSettings settings)`。
  两个形参**刻意按值**——协程的引用形参只拷引用不拷对象，改成 `const&` 能编译、能过大多数
  测试，只在加载期间恰好发生 `AddEntry` 时炸。
- **未注册 type 的 settings 必须原样保真。** 清单里出现本进程不认识的 type 时
  `CreateSettings` 无从调用，其 settings JSON 原文留在 `AssetEntry::RawSettings`，`Save` 时
  原样吐回。否则任何人保存一次就静默毁掉别人的数据——在进版本控制的文本清单上这是最坏的
  失败模式。这条与错误分级里"未注册 type 是内容性缺损、warning 放行"配套。

## 放弃的方案及代价

- **函数指针注册（`AssetTypeDescriptor` 聚合）**：注册一个类型只需三字段初始化，最轻。但
  函数指针不能持有状态，依赖只能经 `AssetLoadContext` 传递，导致该 struct 随 importer 数量
  单调膨胀，且每个 importer 都看得见其他 importer 的依赖。仓库既有范式（`Asset`、
  `IWaitFrameProcessor`、整个 RHI）也都是虚接口。
- **`std::function` / `move_only_function` 注册**：能捕获依赖，但仓库一处都没用过这两个
  类型，且它把依赖藏进不可检视的闭包里——importer 需要什么变成运行时才知道的事。
- **settings 存已解析的 JSON 树（`JsonValue` 常驻）**：比裸文本好，但 `JsonValue` 是依附
  `JsonDocument` 的 view，会把 ADR-0036 D8 那套"只能在主线程碰、协程挂起后不得访问"的时序
  约束重新引进来，且仍然没有类型检查，loader 照样手扒节点。
- **settings 存 opaque 字节 / 裸 JSON 文本（ADR-0038 D5）**：存储层最简单，但每个 loader
  重复实现解析与错误处理，且没有工具能通用地读写导入设置。
- **丢弃未注册 type 的 settings**：实现最省事，代价是保存一次即静默数据丢失。
- **未注册 type 硬失败拒载整个清单**：不会丢数据，但一个新资产类型就让旧版本工具完全打不开
  工程，与错误分级的"身份可信则内容缺损放行"哲学冲突。

## 必须保持为真

- 一个 `type` 对应一个 `AssetImporter` 实例，由 `AssetDatabase` 拥有；importer 的依赖在其
  构造时注入，不经 `AssetLoadContext` 传递。
- `AssetLoadContext` 不含 `AssetManager*` / `FrameUploadScheduler*` 或任何其他系统指针。
- `AssetImporter::Load` 及其覆盖实现都不是协程。经 `TypedAssetImporter` 时
  `LoadTyped` 的形参按值传递，不得改为引用。
- `AssetImportSettings` 派生类是纯 CPU 值语义，可拷贝进协程帧；不持有 DOM 句柄、
  `JsonValue`、或任何依附外部文档生命周期的对象。
- `GetSettings<T>()` 在类型不符时返回 `nullptr`，不做未检查的 `static_cast`。
- 未注册 type 的条目照常进索引、照常写回，其 settings 原文逐字保留；该条目不可加载并记
  warning。
- 清单里的 `type` 字符串是进版本控制的公开标识符，一旦发布不得重命名（与
  `AGENTS.md` 禁止重命名枚举成员同源）。
