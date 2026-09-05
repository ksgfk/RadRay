# ADR-0055 C++ RTTI 是运行时对象查询权威

状态: 生效；取代 ADR-0042 中由自定义描述符判断 settings 类型的部分；构建选项归属部分已被 ADR-0056 取代
日期: 2026-09
影响: `radraycore` 的稳定类型 GUID；runtime 的资产、导入设置、组件与服务装配；公共 C++ 构建契约

> - 适用: 判断 C++ 对象类型、转换资产/组件/settings 视图，或登记和解析 runtime 服务
> - 权威: 本文记录 2026-09 的类型机制决策；当前接口以对应 architecture 文档为准
> - 锚点: `modules/core/include/radray/runtime_type.h`, `modules/core/CMakeLists.txt`, `modules/runtime/include/radray/runtime/asset_manager.h`, `modules/runtime/include/radray/runtime/asset_database.h`, `modules/runtime/include/radray/runtime/game_framework/actor.h`, `modules/runtime/include/radray/runtime/service_registry.h`

## 背景

旧机制让每个可查询类型同时维护固定 GUID、`Bases` 图、对象虚函数和静态描述符。它复制了 C++
继承事实，却不能从一个运行期 GUID 计算多继承子对象偏移；资产还要交叉核对 loader 声明与对象
自报类型。查询一个基类、横向接口或虚继承接口，取决于人工图是否完整，而不是实际对象能否转换。

固定 GUID 仍有独立价值：它可以作为稳定、跨构建的协议或持久身份。这个用途与进程内 C++ 对象
关系不是同一个概念，不应由一套全局映射绑定。

## 决策

1. `runtime_type.h` 只保留 `RuntimeTypeId = Guid`、`RuntimeTypeTrait<T>::value` 和
   `runtime_type_id_v<T>`。显式读取按 `remove_cvref_t` 归一化并要求非空特化；使用 RTTI 查询对象
   本身不要求 GUID。
2. 资产、组件和 settings 的可空查询返回 `Nullable`，对实际多态对象使用指针形式
   `dynamic_cast`。查询目标可以是任意完整类类型；精确动态类型在确认对象存在后使用 `typeid`。
   不增加替代类型虚函数或通用对象基类。
3. `AssetLoadResult` 和 slot 只保存实际 `Asset` 对象，不保存声明类型描述符。Loading 视图可以先
   建立；最终不匹配时既有视图保留 slot 与终态观察权，但不暴露对象，新建的不匹配 Ready 视图为空。
4. `ServiceRegistry::Add<Interfaces...>(T*)` 用 `std::type_index(typeid(T))` 登记静态类型，只额外
   登记显式接口。指针调整发生在知道 `T` 的 Add 调用中；每个对象只有一条 Wire/Initialize
   lifecycle entry，`Resolve<T>()` 只按精确键查询并返回 `Nullable<T*>`。
5. RTTI 是 `radraycore` 的公共 CMake usage requirement：MSVC/ClangCL 使用 `/GR`，其他前端使用
   `-frtti`。所有静态库和最终程序必须采用一致的 RTTI ABI。
6. 现有生产类型的 GUID 数值、资产持久 ID、manifest 格式和 LTO 策略不变。不建立 GUID 与
   `std::type_info` 的全局映射，正确性不依赖 LTO 消除转换。

## 放弃的方案及代价

- **保留 GUID 继承图并补足 pointer-adjust thunk**：仍需为每个类型重复声明真实继承，横向转换、
  虚继承和无 GUID 接口继续增加另一套规则。
- **建立 GUID ↔ RTTI 全局注册表**：把稳定协议身份与进程内 ABI 身份强耦合，并引入注册顺序、
  唯一性和跨模块生命周期问题，本次没有消费方需要这张表。
- **增加统一 `GetTypeInfo` / `GetTypeId` 虚基类**：恢复每个生产类必须覆写的平行事实，也违背
  settings 接口和横向视图保持领域基类边界的目标。
- **使用引用形式 `dynamic_cast`**：不匹配会抛出 `std::bad_cast`；可空查询天然适合指针形式失败
  返回 null，本次不引入异常控制流。
- **为 typed asset ref 缓存调整后指针**：会增加状态与失效推理。当前每次访问至多做一次必要转换，
  先以清楚、可验证的语义为准。
- **服务自动登记所有 C++ 基类**：让 registry 暗中扩大可解析面。显式接口列表更容易审计，且静态
  `static_cast` 已足以在 Add 点正确处理多继承偏移。

## 必须保持为真

- GUID 只表达独立稳定标识；它不表达 is-a，不参与对象查询或服务索引。现有公开 GUID 不得因本
  决策重编号。
- `dynamic_cast` 查询目标必须是完整类类型，source 必须是仍在生命周期内的实际多态对象；可空
  接口使用指针转换，不增加为失败捕获异常的路径。
- 对对象使用 `typeid(*pointer)` 前必须先确认 pointer 非空。
- 资产和组件的创建入口仍受各自领域基类约束；只有查询/引用视图放宽到其他完整类类型。
- 不匹配的 Loading asset 视图仍持有 slot；Ready 后新发起的不匹配转换不取得 slot。
- 服务的接口 binding 只参与 resolve，不重复 Wire/Initialize；null、重复键和缺失必需依赖继续
  fail-fast。
- 构建图中任何消费 RadRay C++ 头和对象的目标都不得关闭 RTTI。
