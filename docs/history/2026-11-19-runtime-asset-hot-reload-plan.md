> - 适用: 理解 RadRay 剥离 `AssetManager` 内置异步协程调度器与 `ReplaceShader` 特化后门，收敛为生而有效的 8 字节句柄仓库、正反向依赖追踪、`AssetTransaction` 原子事务热替换与零堆分配侵入式 `AssetGarbageBatch` 统一帧退役机制的重构计划
> - 权威: 经用户确认保存的设计与重构计划；不作为已实现契约，当前已实现行为见 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `modules/runtime/include/radray/runtime/asset_handle.h`, `modules/runtime/include/radray/runtime/asset_manager.h`, `modules/runtime/src/asset_manager.cpp`, `modules/runtime/include/radray/runtime/shader_asset.h`, `modules/runtime/src/shader_asset.cpp`, `modules/runtime/include/radray/runtime/material.h`, `modules/runtime/src/material.cpp`, `modules/runtime/include/radray/runtime/frame_slot.h`, `examples/mesh_drawing_pipeline/gpu_support.h`, `examples/mesh_drawing_pipeline/gpu_support.cpp`, `examples/mesh_drawing_pipeline/pipeline.h`, `examples/mesh_drawing_pipeline/pipeline.cpp`, `examples/mesh_drawing_pipeline/sample.cpp`, `modules/runtime/tests/test_asset_manager.cpp`, `modules/runtime/tests/test_shader_asset.cpp`, `modules/runtime/tests/test_material.cpp`, `modules/runtime/tests/test_shader_publish.cpp`

# Runtime 资产仓库极简收敛、事务热替换与统一帧退役重构计划

## 状态、基线与范围

- 记录日期：2026-11-19
- 源码基线：`aea0dee6`
- 状态：已由用户确认采用**“生而有效（Always Ready）8 字节稳定句柄仓库 + 正反向依赖引用计数 + `AssetTransaction` 原子事务热替换 + 零堆分配侵入式 `AssetGarbageBatch` 帧首退役”**方案，待实施
- 影响范围：`RadRay::runtime` 的 `asset_handle.h`、`asset_manager.h/.cpp`、`shader_asset.h/.cpp`、`material.h/.cpp`、`examples/mesh_drawing_pipeline` 及 `modules/runtime/tests/` 相关测试；明确不包含 OS 文件系统监控（`AssetWatcher`），不在 `AssetManager` 内置异步 I/O 或多线程加载调度器，不重做 `MaterialInstance` 按参数下标覆盖存储

---

## 一、现状病灶分析

在基线 `aea0dee6` 中，[`AssetManager`](../../modules/runtime/include/radray/runtime/asset_manager.h) 同时承担了“类型擦除句柄仓库”与“单线程协程异步加载调度器”两份职责，并仅通过特化接口 `ReplaceShader` 支持单一类型的原地替换：

```cpp
shader::DiagnosticResult<ShaderAsset> ReplaceShader(const AssetRef<ShaderAsset>& target, ShaderAsset&& candidate);
```

对照实际渲染运行时需求与模块边界，现有实现存在四处结构性问题：

1. **仓库与单线程协程加载调度器紧耦合，引入大量偶然复杂度**
   - `AssetManager` 内置了 `AssetState`（`Unloaded` / `Loading` / `Failed` / `Ready`）、`AssetMeta`、`AssetLoader<T>`、`AssetLoadContext`、`TaskScope`、`ManualCoroutineScheduler`、`Request`、`LoadAsync`、`Pump`、`HasPendingWork`、`DispatchExternal`、`Resolve` 与 `Shutdown`。
   - 实际上离线加载（[`ShaderAsset::Load`](../../modules/runtime/src/shader_asset.cpp)）本身是同步文件解析与 GPU 对象创建，实时编译（[`ShaderAsset::Compile`](../../modules/runtime/src/shader_asset.cpp)）与材质构建（[`CreateMaterial`](../../modules/runtime/src/material.cpp)）更是在仓库外部直接完成；将半成品加载态塞进资产槽位迫使所有查询路径处理未就绪分支，同时阻塞了未来在仓库上游自由组织多线程预解码与批量构建。

2. **`ReplaceShader` 特化后门破坏类型擦除边界与依赖图完整性**
   - `AssetManager` 内部基于 `detail::ErasedAsset` 与 `std::type_index` 实现了无侵入基类的类型擦除槽位，但公开头 `asset_manager.h` 与实现 `asset_manager.cpp` 却为了 `ReplaceShader` 反向包含 `shader/diagnostic_result.h` 和 `runtime/shader_asset.h`。
   - 纹理、网格、材质等其他资产无法复用同一替换机制；替换后若依赖集合增减，`Slot::Dependencies` 引用计数与环路约束也无法同步维护。

3. **缺少内容版本号、反向依赖视图与多资产原子事务**
   - [`UntypedAssetHandle`](../../modules/runtime/include/radray/runtime/asset_handle.h) 的 `Generation` 仅在槽位回收时递增，原地替换后缺少单调递增的 `Revision` 供下游增量感知。
   - [`ShaderAsset::CheckReplacement`](../../modules/runtime/src/shader_asset.cpp) 把合法的接口演进（如 CBuffer 成员偏移调整、绑定组或入口变化）与跨设备错误一并拒绝；同时仓库未维护反向依赖 `Dependents`，无法表达“子资产 `ShaderAsset` 发生破坏性布局变更时，必须连同受影响的 `MaterialAsset` 在同一原子事务内一并校验并替换”。

4. **卸载与热替换缺少统一的零分配帧退役原语**
   - 热替换在 `Slot::Refs > 0` 时就会换下旧版资产对象，仅靠 `AssetRef<T>` 无法覆盖旧版 GPU 资源在途生命周期；若为每帧退役额外引入堆分配容器或包装对象，又会增加帧首维护开销并引发空帧触发 [`FrameSlot`](../../modules/runtime/include/radray/runtime/frame_slot.h) 初始 `128 KiB` 缓冲分配。

---

## 二、目标设计契约

### 1. 收敛仓库职责：稳定身份核心与“生而有效（Always Ready）”不变量

将 `AssetManager` 彻底收敛为 owner 线程串行使用的**内存已就绪资产仓库**：文件读取（`ShaderAsset::Load`）、实时编译（`ShaderAsset::Compile`）及未来多线程预解码均在仓库上游完成后，通过 `Emplace` 入库或通过 `AssetTransaction` 原子提交。

- **保留的稳定身份与核心接口**：
  - `UntypedAssetHandle` / `AssetHandle<T>`（8 字节 `{Index, Generation}`）、`AssetRef<T>`（64 位平台 16 字节）、`AssetId` / `MakeAssetId` 保持不变；
  - `Emplace`、`EmplaceWithHandle`、`Acquire`、`Get`、`GetRefCount`、`CreateMaterial` 保持原有语义；原 `FindLoaded(AssetId)` 收敛更名为 `std::optional<UntypedAssetHandle> Find(AssetId id) const noexcept`。
- **存活槽位生而有效（Always Ready）与依赖保活边界**：
  - 槽位一旦完成发布（`Alive == true`），`Get(handle)` 必返回非空只读指针，不再存在半成品槽位；
  - `EmplaceWithHandle(deps, id, factory)` 在调用 `factory` 前预留槽位与具名 ID，校验所有 `deps` 当前均存活且不含自身；预留期间 `Alive == false`（不可 `Find`、`Acquire` 或 `Get`，且拒绝同 ID 重入），`factory` 返回 `std::nullopt` 时自动回滚预留、移除具名占用并递增 `Generation`；
  - **依赖边保活粒度契约**：`Dependencies` 保活的是**子槽位逻辑身份**而非钉住特定历史 `VersionNode`；仓库内父资产只允许保存子 `AssetHandle<T>` 或值拷贝元数据（如 CBuffer 成员偏移表），凡被父资产直接借用内部原生对象的子资产，在替换该原生对象时 `ClassifyReload` **必须返回 `Breaking`**，由 `Commit` 强制所有存活 `Dependents` 同批重建。
- **新增版本、反向依赖与零开销类型化句柄转换查询**：
  - `uint32_t GetRevision(UntypedAssetHandle handle) const noexcept`：无效或已回收句柄返回 `0`；首次 `Emplace` 发布置为 `1`；每次 `Commit` 替换该槽位递增 `++Revision`（达到 `uint32_t` 上限时 `RADRAY_ABORT`）；槽位回收时重置为 `0` 并递增 `Generation`；
  - `std::span<const UntypedAssetHandle> GetDependents(UntypedAssetHandle handle) const noexcept`：返回当前直接依赖该槽位的全部**存活**父槽位句柄视图（无效句柄返回空 `span`）；
  - `template <typename T> AssetHandle<T> AsHandle(UntypedAssetHandle handle) const noexcept` 与 `template <typename T> AssetHandle<T> Find(AssetId id) const noexcept`：校验句柄在当前 `AssetManager` 中存活、代次匹配且槽位类型为 `typeid(T)`，匹配时返回强类型 `AssetHandle<T>`，否则返回空句柄，**全程不增减引用计数**，打通从 `Find`、`GetDependents` 与 `AssetReloadEvent::Handle` 到 `Get`、`Acquire`、`StageReplace` 及 `StageMaterial` 的公开调用闭环。
- **彻底移除的组件与接口**：
  - [`asset_handle.h`](../../modules/runtime/include/radray/runtime/asset_handle.h)：移除 `AssetState` 与 `AssetLoadContext` 前置/友元声明；
  - [`asset_manager.h`](../../modules/runtime/include/radray/runtime/asset_manager.h)：移除 `AssetMeta`、`ErasedAssetLoader`、`AssetLoader<T>`、`AssetLoadContext`、`RegisterLoader`、`RegisterMeta`、`FindMeta`、`Request`、`LoadAsync`、`Resolve`、`GetState`、`ReplaceShader`、`Pump`、`HasPendingWork`、`DispatchExternal`、`Shutdown`（同步移除 `<radray/coroutine.h>` 与 `<radray/shader/diagnostic_result.h>` 依赖），以及 `AssetRef<T>` 上的 `GetState`、`IsReady`、`Resolve`；
  - [`shader_asset.h`](../../modules/runtime/include/radray/runtime/shader_asset.h)：移除 `ShaderAssetLoader` 与 `ShaderAsset::CheckReplacement`。

### 2. 三态重载分类契约（`ClassifyReload`）

在 `asset_manager.h` 中定义与具体资产类型解耦的重载分类与提交结果类型（遵守 runtime 头文件约定：纯数据记录使用 `struct`，含构造/析构或成员函数的类型使用 `class`）：

```cpp
enum class AssetReloadKind : uint8_t {
    Compatible,
    Breaking,
    Rejected
};

struct AssetReloadDecision {
    AssetReloadKind Kind{AssetReloadKind::Compatible};
    string Diagnostic{};
};

struct AssetReloadEvent {
    UntypedAssetHandle Handle{};
    AssetId Id{};
    std::type_index Type{typeid(void)};
    uint32_t Revision{};
    AssetReloadKind Kind{AssetReloadKind::Compatible};
};

struct AssetCommitResult {
    bool Succeeded{};
    vector<AssetReloadEvent> Events;
    string Diagnostic{};
};
```

- **默认与自定义分类规则**：
  - `AssetTransaction::StageReplace<T>` 与 `StageBuild<T>` 通过 `if constexpr` 检测 `const T` 是否提供返回 `AssetReloadDecision` 的 `ClassifyReload(const T& candidate)`：存在则自动绑定调用，否则默认返回 `{AssetReloadKind::Compatible, {}}`；
  - **`ShaderAsset::ClassifyReload(const ShaderAsset& candidate) const`**：
    1. `_device != candidate._device` $\rightarrow$ `{AssetReloadKind::Rejected, "shader device changed"}`；
    2. 完全一致或**仅 `VertexInputs` 变化** $\rightarrow$ `{AssetReloadKind::Compatible, {}}`；
    3. `SharedLayout`（`Groups`、`PushConstants`、`Types`）、变体数量/顺序/名称、当前后端可用性、`Stages`、`ActiveBindings`、`ActivePushConstants` 或任一活跃阶段 `EntryPoint` 变化 $\rightarrow$ `{AssetReloadKind::Breaking, "<具体变化描述>"}`。
  - **`MaterialAsset::ClassifyReload(const MaterialAsset& candidate) const`**：
    1. 参数数量、任一下标参数名或参数值类型（`variant::index()`）变化 $\rightarrow$ `{AssetReloadKind::Rejected, "material parameter layout changed"}`（保护 [`MaterialInstance`](../../modules/runtime/include/radray/runtime/material.h) 按声明下标存储的覆盖值）；
    2. 参数签名序列、`Properties`（`MaterialTagSet` 按同表集合等价比较、`MaterialRenderState` 逐字段比较）与 `Programs` 均一致（仅默认值或内部 `_bindings` 偏移/槽位更新） $\rightarrow$ `{AssetReloadKind::Compatible, {}}`；
    3. 参数签名序列一致，但 `Properties` 或 `Programs` 变化 $\rightarrow$ `{AssetReloadKind::Breaking, "material properties or programs changed"}`。

### 3. `AssetTransaction` 原子批量替换与强一致性提交（`Commit`）

- **`AssetTransaction` 暂存、延迟构建与中毒保护（Poisoning）**：
  - `explicit AssetTransaction(const AssetManager& manager) noexcept`：构造绑定目标 `AssetManager` 的事务对象；仅支持移动，不支持拷贝。暂存阶段仅在事务内部持有候选对象或延迟构建闭包及拟更新依赖列表，**不修改 `AssetManager` 任何线上槽位或引用计数**（未提交直接析构事务零回滚代价）；
  - `bool StageReplace<T>(AssetHandle<T> target, T&& candidate, std::span<const UntypedAssetHandle> deps = {})`（并提供 `const AssetRef<T>& target` 薄重载：先校验 `target` 非空且归属当前事务绑定的 `_manager`，拦截跨库强引用误传）：
    - 校验事务未中毒、`target` 在所属 `AssetManager` 存活、类型匹配 `typeid(T)`、`GetRefCount(target.Untyped()) > 0`、本事务内尚未暂存同一 `target`，且 `deps` 中每个句柄当前均存活且不等于 `target`（内部对 `deps` 保序去重）；
    - 若任一条件不满足，将事务标记为**中毒状态（Poisoned）**、保留首条错误诊断并返回 `false`；后续即使调用方忽略某步返回值继续调用 `Commit`，中毒事务也会被整批拒绝，杜绝部分提交；
  - `bool StageBuild<T, Builder>(AssetHandle<T> target, std::span<const UntypedAssetHandle> deps, Builder&& builder)`（同样提供 `const AssetRef<T>&` 薄重载）：
    - 执行与 `StageReplace` 相同的目标与依赖前置校验及 `T::ClassifyReload` 绑定，不要求在暂存当下立刻构造 `T`，而是保存类型擦除的延迟构建闭包 `builder(const AssetTransaction&) -> std::optional<T>`，留待 `Commit` 预检阶段按最终依赖拓扑序求值（保持 `asset_manager.h` 与具体派生资产描述类型彻底解耦）；
    - **只读、事务冻结与显式依赖可见性硬约束**：`builder` 与 `ClassifyReload` 对当前 `AssetManager` 和正在提交的 `AssetTransaction` **严格只读**——一旦事务进入 `Commit`（`tx._committing == true` 或 `manager._inCommit == true`），任何通过外部捕获引用再次调用 `tx.StageReplace` / `tx.StageBuild` 或修改仓库/增减引用计数的行为均立即 `RADRAY_ABORT`；`builder` 读取任何子资产**必须统一经由入参 `const AssetTransaction&` 的 `Get`（不得绕过事务直接调 `assets.Get` 或 `AssetRef::Get`）且目标必须已列入该项自身的 `deps`**，未列入 `deps` 的句柄在 `builder` 求值期间调用 `tx.Get` 直接返回 `nullptr`，从机制上保证拓扑求值顺序确定且无顺序敏感漏洞；
  - `Nullable<const T*> Get<T>(AssetHandle<T> handle) const noexcept`：在 `StageBuild` 闭包求值作用域内仅允许解析当前构建项 `deps` 中已声明的句柄（未声明直接返回 `nullptr`）；若 `handle` 在本事务中已有暂存项——已具象化且类型匹配时返回候选指针，类型不匹配或属于尚未求值的 `StageBuild` 项时返回 `nullptr`（防止误回退读取陈旧线上对象）；若未在本事务暂存则回退返回 `_manager.Get(handle)`；
  - `bool StageMaterial(AssetTransaction& tx, AssetHandle<MaterialAsset> target, const MaterialAssetDesc& desc)`（及 `const AssetRef<MaterialAsset>&` 重载，声明于 [`material.h`](../../modules/runtime/include/radray/runtime/material.h)）：
    - [`MaterialAsset`](../../modules/runtime/src/material.cpp) 内部提取共用构建核心，统一接收 Shader 解析函数对象（`CreateMaterial` 传入 `assets.Get`，`StageMaterial` 传入 `tx.Get<ShaderAsset>`），使 `desc.Programs` 与 `desc.Families` 两处读取共享同一解析入口；
    - 收集 `desc.Families` 中的 Shader 句柄作为依赖，通过 `tx.StageBuild<MaterialAsset>` 捕获 `desc` 登记延迟构建；在 `Commit` 预检求值时统一经 `tx.Get<ShaderAsset>` 读取**最终生效版本**的 `ShaderAsset`（同事务内无论先暂存材质还是后暂存 Shader，或暂存与提交之间线上 Shader 已被其他事务更新，均以提交时刻的最终 Shader 反射布局为准；若 `desc.Programs` 引用了未列入 `desc.Families` 的 Shader，`tx.Get` 返回空值并拒绝构建）重算 `MaterialParameterBindingMap`；任何暂存或构建校验失败均确保整批拒绝。

- **`AssetManager::Commit(AssetTransaction&& tx, AssetGarbageBatch& outRetired)` 强原子性协议**：
  - 独立于析构期 `_callbackDepth` 维护提交预检锁 `_inCommit` 与事务自身冻结标志 `tx._committing`：在 `_callbackDepth != 0` 或 `_inCommit` 期间重入调用 `Commit`，或在提交期间对当前 `AssetManager` 执行任何写操作/引用计数变更（`Emplace`、`EmplaceWithHandle`、`Acquire`、`Retain`、`Release`、`CollectGarbage`）、对正在提交的 `tx` 调用 `StageReplace` / `StageBuild`，或传入归属其他 `AssetManager` 的非空 `outRetired` 时，一律触发 `RADRAY_ABORT`（而资产析构函数所在的 `_callbackDepth` 作用域仍允许创建/释放临时资产，仅禁止重入 `CollectGarbage` 与 `Commit`）；传入归属其他 `AssetManager` 的 `tx` 返回失败诊断；未中毒的空事务直接返回 `{true, {}, {}}`；
  - **阶段 1：全量只读预检与拓扑延迟求值（任一失败直接返回诊断，线上状态零改动）**：
    1. 置 `_inCommit = true` 与 `tx._committing = true`（作用域退出自动复位 `_inCommit`），检查 `!tx.IsPoisoned()`；
    2. 复核每个暂存 `target` 仍在当前 `AssetManager` 存活、类型一致且 `Refs > 0`，并复核其新 `deps` 全部存活且无自环；
    3. **拟提交后依赖图三色 DFS 验环与拓扑排序**：在存活槽位有向图上（凡属于 `tx` 的槽位取其拟提交的新 `deps`，其余存活槽位取现行 `Slot::Current->Dependencies`）执行三色（`Unvisited` / `Visiting` / `Visited`）DFS；支持旧边 `A1 -> B1` 在同批事务内合法翻转为 `B2 -> A2`，拦截 `A2 <-> B2` 等任意有向环，并同时产出事务内目标在**新依赖关系下的子先父后拓扑序**；
    4. **按子先父后拓扑序具象化 `StageBuild` 项**：在 `_inCommit` 只读保护与当前项 `deps` 可见性约束下，按新依赖子先父后顺序依次执行延迟构建闭包（此时每个父构建器通过 `tx.Get` 读取到的已声明子依赖必为最终拟提交版本或最新线上版本，且无法篡改仓库状态或释放既有引用）；任一闭包返回空值立即整批拒绝；
    5. **兼容性与反向依赖覆盖校验**：对每个暂存项调用 `Classify(current, candidate)`——若任一项返回 `AssetReloadKind::Rejected` $\rightarrow$ 整批拒绝；若某项 `X` 返回 `AssetReloadKind::Breaking`，遍历 `Slot(X).Dependents`，只要存在任一存活父槽位未包含在本次 `tx` 的暂存目标集合中 $\rightarrow$ 整批拒绝；
  - **阶段 2：改动前一次性容量预留**：
    - 在触碰首个线上 `Slot` 之前，完成所有新 `VersionNode` 分配，并一次性 `reserve` `result.Events` 以及全部将新增反向边的目标子槽位 `Dependents` 容量（`outRetired` 为侵入式链表无需分配），保证阶段 3 全程 `noexcept` 零失败窗口；
  - **阶段 3：双拓扑序无分配原子切换**：
    1. **旧版本退役序（换槽前旧依赖：父先子后）**：在本次暂存目标集合上按换槽前 `Slot::Current->Dependencies` 诱导的 DAG 求父先子后拓扑序，依次将各槽位旧 `VersionNode` 经 `NextInBatch` $O(1)$ 挂入 `outRetired` 尾部（旧 `VersionNode` 持有的子槽位引用计数保持不减，直至 `outRetired` 清理析构完旧父资产后再释放）；
    2. **线上状态切换**：对每个新 `VersionNode::Dependencies` 执行 `Retain`，替换 `Slot::Current`，同步更新相关子槽位的 `Slot::Dependents`（移除不再依赖的旧边、追加新增边），并递增 `++Slot::Revision`；
    3. **变更事件通知序（换槽后新依赖：子先父后）**：在本次暂存目标集合上按换槽后新 `Dependencies` 诱导的 DAG 求子先父后拓扑序，依次生成 `AssetReloadEvent` 写入 `result.Events` 并返回。

### 4. 零堆分配侵入式 `AssetGarbageBatch` 与帧生命周期

- **`VersionNode` 单一所有权与确定性析构序**：
  ```cpp
  struct VersionNode {
      detail::ErasedAsset Asset{nullptr, nullptr};
      InlineVector<UntypedAssetHandle, 4> Dependencies;  // 每项持有子槽位 1 次 Retain，直至 Asset 析构完成后释放
      Nullable<VersionNode*> NextInBatch{nullptr};       // AssetGarbageBatch 侵入式单向链表指针
  };
  ```
  - 每个 `VersionNode` 在任意时刻只有唯一所有者：线上存活期间归 `Slot::Current` 独占；因 `CollectGarbage(outRetired)` 卸载或 `Commit(tx, outRetired)` 换下后，直接通过 `NextInBatch` 转移归 `AssetGarbageBatch` 独占；
  - `Storage` 维护外部待清理退役节点计数 `RetiredVersionNodes`：挂入 `AssetGarbageBatch` 时递增，实际销毁节点时递减。

```cpp
class AssetGarbageBatch final {
public:
    AssetGarbageBatch() noexcept = default;
    ~AssetGarbageBatch() noexcept;
    AssetGarbageBatch(AssetGarbageBatch&& other) noexcept;
    AssetGarbageBatch& operator=(AssetGarbageBatch&& other) noexcept;
    AssetGarbageBatch(const AssetGarbageBatch&) = delete;
    AssetGarbageBatch& operator=(const AssetGarbageBatch&) = delete;

    void Clear() noexcept;
    bool Empty() const noexcept;
    size_t Size() const noexcept;

private:
    friend class AssetManager;
    Nullable<AssetManager*> _manager{nullptr};
    Nullable<void*> _head{nullptr};
    Nullable<void*> _tail{nullptr};
    size_t _count{0};
};
```

- **`AssetGarbageBatch` 清理与 `CollectGarbage` 契约**：
  - `AssetGarbageBatch` 所有接口均为 `noexcept` 且零堆分配。`Clear()` 与 `~AssetGarbageBatch() noexcept` 从 `_head` 到 `_tail` 依次弹出 `VersionNode`：对每个节点，**先在 `EnterCallback()` 保护下执行 `node->Asset.reset()` 完整运行父资产析构函数，随后再遍历 `node->Dependencies` 调用 `Release(child)` 并 `delete node`**；因此无论同步回收还是跨帧延迟清理，父资产析构函数内调用 `assets.Get(child)` 均保证成功；
  - `bool HasPendingGarbage() const noexcept`：$O(1)$ 判断 `ZeroHead != kInvalidIndex`；
  - `size_t CollectGarbage(AssetGarbageBatch& outRetired) noexcept`（回调内调用或跨不同 `AssetManager` 混用非空批次触发 `RADRAY_ABORT`）：弹出当前 `ZeroHead` 链上所有 `Refs == 0` 的槽位，擦除具名表项、从子槽位 `Dependents` 中移除该死槽位、置 `Alive = false` 与 `Revision = 0`、递增 `Generation` 归入 `FreeHead`，并将原 `Slot::Current` 挂入 `outRetired` 尾部，返回本次摘除槽位数；
  - `size_t CollectGarbage() noexcept`：构造栈上局部 `AssetGarbageBatch`，循环执行 `CollectGarbage(batch)` 与 `batch.Clear()` 直至 `ZeroHead` 完全排空（覆盖资产析构函数内级联释放子槽位或新建临时资产产生的二次零引用槽位），返回累计回收槽位数；
  - `~AssetManager() noexcept`：先执行 `CollectGarbage()`，再同时校验所有槽位 `!slot.Alive` 且 `RetiredVersionNodes == 0`；若仍有外部强引用或晚于 `AssetManager` 存活的非空 `AssetGarbageBatch`，立即 `RADRAY_ABORT`。

- **帧首标准接入与下游派生 GPU 对象生命周期契约**：
  ```cpp
  FrameSlot& slot = scheduler.BeginFrame();

  AssetCommitResult commit;
  if (assets.HasPendingGarbage() || pendingTx.has_value()) {
      auto* retired = slot.Emplace<AssetGarbageBatch>();
      assets.CollectGarbage(*retired);
      if (pendingTx) {
          commit = assets.Commit(std::move(*pendingTx), *retired);
          pendingTx.reset();
      }
  }
  ```
  1. **空帧零分配快路径**：无待回收垃圾且无待提交事务的常规帧不调用 `slot.Emplace<AssetGarbageBatch>()`，不会触发 `FrameSlot` 初始 `128 KiB` 线性缓冲分配；
  2. **LIFO 逆序退役保证**：`AssetGarbageBatch` 最先登记进当帧 `FrameSlot`，下游随后登记的旧 PSO、旧 `TextureView` 或旧 `ShaderParameterSet` 位于 LIFO 链表更外层；当该 `FrameSlot` 完成 GPU Fence 等待与命令分配器重置后，必然**先销毁派生视图与管线，再销毁父资产，最后释放子资产引用**；
  3. **创建后自持型对象（如 `GraphicsPipelineState`）、Pass 固定布局校验与粘滞 `Breaking` 暂停规则**：
     - 统一采用**提交后按版本同步**路径（天然同时支持纯 `ShaderAsset` 替换与含延迟 `StageMaterial` 的级联事务）：每套管线缓存同时维护已成功应用的 `(AppliedShaderRevision, AppliedMaterialRevision)` 与已验证仍同现有旧 PSO 兼容的 `(CompatibleShaderRevision, CompatibleMaterialRevision)`；
     - **Pass 侧固定常量 ABI 校验前置**：在构建新 PSO 并推进 `Applied*Revision` 前，渲染方除校验原生组布局与顶点输入外，还必须用新版 `ShaderAsset` 反射校验 Pass 自身直接写入的固定 CPU 常量布局（例如教学样例 `PassConstants` CBuffer 的块大小及 `ViewProjection`、`LocalScale` 成员类型/偏移，以及 `MaterialPush` 块大小与 `Color` 成员类型/偏移）；校验不通过即视为管线准备失败，不推进 `Applied*Revision`；
     - 仅当收到紧邻下一版本的 `Compatible` 事件（`event.Revision == CompatibleRev + 1`）时推进 `CompatibleRev`；**凡自上一次成功应用 PSO 起经历过任意一次 `Breaking` 事件，或漏收了中间版本事件（导致 `CompatibleRev != 当前线上 Revision`），即进入粘滞不兼容状态**——若当帧管线准备失败，立即将已失效的旧 PSO 移入当帧 `FrameSlot` 退役、标脏对应 `Scene` 材质代理以清空陈旧缓存命令，并**持续暂停受影响绘制**（后续即便再提交相对于上一版为 `Compatible` 的新版本且再次失败，只要新 PSO 尚未成功应用就继续保持暂停）；
     - 仅当 `CompatibleShaderRevision == LiveShaderRevision && CompatibleMaterialRevision == LiveMaterialRevision`（即期间所有变更均连续验证为纯 `Compatible`）而新 PSO 重建失败时，才保留旧 PSO 继续正常绘制并在下一帧按 `AppliedRev != LiveRev` 自动重试；
     - **每次成功更换 PSO 必刷缓存命令**：无论是当帧直通替换、从粘滞暂停恢复，还是**纯 `Compatible` 变更在无新事件的后续帧重试成功**，只要新 PSO 换入生效并将旧 PSO 移入当帧 `FrameSlot`，**都必须在当帧 `Extract` 前标脏受影响的 `Scene` 材质代理并执行 `SyncCachedMeshDrawCommands`**，使 `MeshDrawCommand::Pipeline` 立即更新指向新 PSO，杜绝旧帧槽回收后解引用悬垂 PSO 指针；
  4. **按 `Revision` 驱动的 `MaterialRenderProxy` 参数快照刷新**：[`MaterialRenderProxy`](../../modules/runtime/include/radray/runtime/material.h) 保存解析后的参数值拷贝，而 `Scene::MarkMaterialDirty` 仅重跑缓存命令构建不会重拉材质默认值；为与漏事件恢复路径完全一致，持有 `MaterialInstance` 的上游同步维护每个代理已应用的 `AppliedMaterialRevision`——**帧同步时只要发现 `assets.GetRevision(instance.GetMaterial().Untyped()) != AppliedMaterialRevision`（无论当帧是否收到事件或先前是否漏收事件），立即重跑 `proxy.Update(assets, instance)` 并调用 `Scene::UpdateMaterial` 更新默认值快照且自动标脏关联 Batch**；
  5. **持续借用型对象（如外部缓存的 `gpu::TextureView` 或绑定了它的 `ShaderParameterSet`）**：跨帧缓存须同时持有底层资产的长期 `AssetRef<T>`；当底层资产在帧 $F$ 被事务替换进 `retired` 时，必须在帧 $F$ 将旧 View 移入帧 $F$ 的 `FrameSlot` 并停止后续帧录制——若为新版本创建新 View 失败，严禁在帧 $F+1$ 继续绑定旧 View，须切换 Fallback 或暂停该项绘制，待后续帧按 `GetRevision` 重试成功后再恢复。

---

## 三、分阶段实施与验证清单

| 阶段 | 涉及文件 | 关键工作与验收标准 |
|---|---|---|
| **Stage 1：仓库极简收敛、事务提交与材质/Shader 重载契约** | `asset_handle.h`<br>`asset_manager.h/.cpp`<br>`shader_asset.h/.cpp`<br>`material.h/.cpp` | 1. 移除 `AssetState`、`AssetMeta`、`AssetLoader<T>`、`ShaderAssetLoader`、`AssetLoadContext`、`Request`、`LoadAsync`、`Pump`、`HasPendingWork`、`DispatchExternal`、`Resolve`、`Shutdown`、`ReplaceShader` 与 `CheckReplacement`；<br>2. 实现 `Find` / `Find<T>`、`AsHandle<T>`、`GetRevision`、`GetDependents`、`HasPendingGarbage`、零分配侵入式 `AssetGarbageBatch`、双模式 `CollectGarbage` 及 `~AssetManager()` 双重残留校验；<br>3. 实现 `ShaderAsset::ClassifyReload`、`MaterialAsset::ClassifyReload`、`AssetTransaction`（`StageReplace`、`StageBuild`、`AssetRef` 归属校验薄重载、受 `deps` 约束的 `Get`、`_committing` 冻结与中毒传播）、共用单一 Shader 解析入口的 `StageMaterial` 与 `AssetManager::Commit`（`_inCommit` 只读预检锁、三色 DFS 验环、子先父后拓扑序求值 `StageBuild`、`Breaking` 反向依赖覆盖检查、容量预留、双拓扑序切换）。 |
| **Stage 2：教学样例迁移** | `examples/mesh_drawing_pipeline/gpu_support.h/.cpp`<br>`examples/mesh_drawing_pipeline/pipeline.h/.cpp`<br>`examples/mesh_drawing_pipeline/sample.cpp` | 1. `GpuResources::Initialize` 离线分支直接调用 `ShaderAsset::Load` 后 `Emplace` 入库，移除 `ShaderAssetLoader` / `RegisterMeta` / `Request` / `Pump`；<br>2. `GpuResources` 统一采用提交后同步管线入口（维护 `Applied*Revision` 与 `Compatible*Revision`，并在构建 PSO 前用反射严格校验 `PassConstants` / `MaterialPush` 块大小与成员偏移、类型），每次成功换入新 PSO（含兼容跨帧重试成功）均在 `Extract` 前标脏代理刷新缓存命令，粘滞暂停态下由各 `PassProcessor` 跳过命令生成；样例按材质线上 `Revision` 对比刷新 `MaterialRenderProxy` 并调用 `Scene::UpdateMaterial`，通过双后端离屏渲染与热替换像素校验。 |
| **Stage 3：单元测试与帧槽协同验证** | `test_asset_manager.cpp`<br>`test_shader_asset.cpp`<br>`test_material.cpp`<br>`test_shader_publish.cpp` | 覆盖并验证：<br>1. 槽位代次复用、存活恢复（`Acquire`）、具名预留失败回滚与重入拒绝，以及通过 `Find` -> `GetDependents` -> `AsHandle<T>` -> `StageMaterial` 完成公开接口闭环；<br>2. 析构期内无法重获已退役句柄且新建 32 个临时资产后无参 `CollectGarbage()` 排空返回 `33`；对比验证 `StageBuild` / `ClassifyReload` 预检期内修改仓库、释放引用或通过捕获引用再次 `tx.Stage*` 均触发 `RADRAY_ABORT`，以及 `StageBuild` 读取未列入 `deps` 的句柄返回 `nullptr`；<br>3. 父资产独占子资产时，同步 `CollectGarbage()` 与跨帧 `AssetGarbageBatch` 延迟清理下 `~Parent()` 内 `assets.Get(child)` 均成功，以及未清理外部 `AssetGarbageBatch` 先于或晚于 `AssetManager` 析构的契约；<br>4. 单资产与多级级联 `AssetTransaction` 提交、`Revision` 递增、空帧 `HasPendingGarbage() == false`，以及跨 `AssetManager` 传入 `AssetRef` 或 `tx` 的拒绝保护；<br>5. 中毒事务、`Rejected` 候选、`Breaking` 遗漏存活 `Dependents`、以及 `A2 <-> B2` 有向依赖环的整批原子回滚（线上状态与引用计数零改动）；<br>6. 旧边 `A1 -> B1` 同事务翻转为 `B2 -> A2` 时的合法提交，验证 `outRetired` 按 `A1 -> B1` 析构且 `Events` 按 `A2 -> B2` 输出；<br>7. 真实 `ShaderAsset` CBuffer 成员偏移从 `0` 移至 `16`（`Breaking`）时，分别按“先 `StageReplace(shader)` 后 `StageMaterial`”、“先 `StageMaterial` 后 `StageReplace(shader)`”及“先在 `tx_late` 暂存材质，再由同时包含新 Shader 与中间材质的 `tx_mid` 先行断言提交成功，最后提交 `tx_late`”三种路径，断言 `MaterialParameterBindingMap::WriteConstants` 均精确写入偏移 `16` 的字节；同时用一继承默认值、一显式覆盖的两个 `MaterialInstance` 验证材质默认颜色热替换（含正常消费事件与丢弃事件按 `Revision` 追平两条路径）后仅继承者变色；<br>8. 纯 `Compatible` 重载帧 `F` PSO 失败沿用旧管线、帧 `F+1` 无新事件重试成功时断言缓存 `MeshDrawCommand::Pipeline` 已更新绑定新 PSO 且在旧帧槽回收后继续安全绘制；交换 `PassConstants` 成员偏移时因反射校验失败保持暂停；以及 `v1 -> v2 (Breaking)` 失败后紧接 `v2 -> v3 (Compatible)` 再次失败（及漏传事件情形）断言持续发出零条受影响 draw，直至重建成功恢复绘制并按逆序安全析构旧资源（含双帧槽借用型 `TextureView` 连续失败场景）。 |
| **Stage 4：长期文档同步** | `docs/architecture/runtime.md`<br>`docs/architecture/overview.md` | 代码落地并通过全部测试后，同步更新 `docs/architecture/runtime.md`（及 `overview.md` 入口简述）中的资产仓库、版本查询、`AssetTransaction` 事务热替换、`AssetGarbageBatch` 帧首退役与派生 GPU 对象生命周期契约，运行 `python tools/check_docs.py` 与 `git diff --check` 零报错。 |
