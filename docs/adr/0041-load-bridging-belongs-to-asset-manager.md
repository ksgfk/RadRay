# ADR-0041 加载桥接归 AssetManager，资产来源经 IAssetSource 反转

状态: 生效
日期: 2026-08
影响: `modules/runtime/include/radray/runtime/asset_manager.h`、`asset_database.h`、
`application.h` 与 `src/application.cpp` 的装配与关停顺序；部分取代 ADR-0039

## 背景

ADR-0036 D7 把清单到 `AssetManager` 的桥接定为自由函数 `LoadFromDatabase`，动机是
"`AssetDatabase` 与 `AssetManager` 头文件互不包含，各自单一职责"。ADR-0039 更进一步删掉了
桥接与 loader 注册表，并把"`AssetDatabase` 不接触 `AssetManager`"写成必须保持为真。

结果是 `AssetDatabase` 没有任何出口：它能登记身份、能解析路径，但拿不出资产。调用方要加载
一份资产仍须自己拼 `AssetLoadRequest`，那意味着自己认识 loader、自己决定 `AssetLoadRequest::Id`
——而 id 一旦选错，`AssetManager` 的去重就失效。必须决定"按身份加载"这个动作归谁。

## 决策

**加载桥接是 `AssetManager` 的职责。资产来源经 `IAssetSource` 虚接口注入，依赖方向为
`AssetManager → IAssetSource ← AssetDatabase`。**

- `AssetManager` 新增按身份加载的入口：`Load(const AssetId&)`、`Load<T>(const AssetId&)`、
  `Load<T>(std::string_view relPath)`。既有的 `Load(AssetLoadRequest)` 完全不变。
- `AssetManager` 只认识 `IAssetSource`：`CreateLoadTask(const AssetId&)` 与
  `ResolveId(std::string_view relPath)`。资产来自 JSON 清单、包文件还是网络与它无关。
- `AssetDatabase` 实现 `IAssetSource`，因此**它确实不包含 `asset_manager.h`**——ADR-0039
  那条约束在这个方向上反而成立了，只是实现手段从"自由函数桥接"换成了接口反转。
- 装配沿用 `IWaitFrameProcessor` 的既有范式（`asset_manager.h` 前置声明接口，`Application`
  在 phase 2 注入具体实现）。区别是**资产数据库是可选服务**：`ServiceRegistry::Wire` 对
  解析不到的依赖是 `RADRAY_ABORT`，所以 `SetAssetSource` 不进 `ServiceTraits<AssetManager>::Inject`，
  而是在 `registry.Wire()` 之后按需手工注入一行。不带资产根的进程（测试、无资产 sample）
  照常启动。
- **关停顺序变为 `World → RenderSystem → AssetManager → AssetDatabase → GpuSystem`。**
  在飞的加载协程会引用数据库持有的 importer 与条目 settings，故数据库必须活过 `AssetManager`
  的析构。
- 这条**部分取代 ADR-0039**：那条记录的"不改 `AssetManager`" / "`AssetDatabase` 不接触
  `AssetManager`，加载桥接留待后续"由本决策落实为接口反转。ADR-0039 的其余内容（放弃
  per-bundle 组织、身份规则保留）继续生效。

## 放弃的方案及代价

- **`AssetDatabase::Load` 返回 `StreamingAssetRef`（本轮曾提出的形状）**：调用点最短，但
  去重权威前面多了一个入口——两个 `Load` 迟早在 id 口径上不一致，而 id 是去重的全部依据。
  返回 slot 引用的对象也理应是 slot 的拥有者。
- **自由函数 `LoadFromDatabase(manager, db, id)`（ADR-0036 D7）**：两个类互不认识，但调用方
  必须同时持有两个对象并记住这个函数存在；且它无法成为 `Load<T>(path)` 那样的自然入口。
  头文件耦合本来就不是真问题（两者同在 `modules/runtime`，单向依赖），为它付出可用性代价
  不划算。
- **`AssetManager` 直接包含 `AssetDatabase`**：最少概念，但把"身份登记的具体形态"焊进生命
  周期管理器。将来换成包文件或远程来源就要改 `AssetManager`，而它是被测试覆盖最重的类。
- **`SetAssetSource` 放进 `ServiceTraits::Inject`**：与 `SetWaitFrameProcessor` 完全对称、
  无需手工装配，但 `Wire` 对缺失依赖 `RADRAY_ABORT`，会让不带资产根的进程直接崩——包括
  现有的全部 runtime 测试。可选依赖不适合走 `Inject`。

## 必须保持为真

- 按身份加载的唯一入口在 `AssetManager`；`AssetDatabase` 的公开接口里没有返回
  `StreamingAssetRef` / `StreamingAssetRefAny` 的成员。
- `AssetManager` 只依赖 `IAssetSource`，不包含 `asset_database.h`，也不出现
  `AssetDatabase` 这个名字（前置声明亦不需要）。
- `AssetDatabase` 不包含 `asset_manager.h`。
- `IAssetSource::CreateLoadTask` 的实现必须在返回前同步取齐一切所需数据；返回的 task 挂起
  之后不得再回查 source（条目可能已被 `AddEntry` / `RemoveEntry` 改动）。
- `SetAssetSource` 不出现在 `ServiceTraits<AssetManager>::Inject` 里；资产来源缺失时
  `Load(AssetId)` 记错误日志并返回无效引用，不 abort。
- `AssetDatabase` 在关停顺序里销毁于 `AssetManager` 之后、`GpuSystem` 之前。
- 既有的 `Load(AssetLoadRequest)` 路径语义不变，不要求经过 `IAssetSource`。
