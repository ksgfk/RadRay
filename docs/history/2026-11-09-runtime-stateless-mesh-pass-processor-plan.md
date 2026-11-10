> - 适用: 理解 RadRay 移除 `MeshPassProcessor` 内部可变 `DrawListContext` 绑定、收窄 `MeshPassDrawListContext` 为单步 `AddCommand` 发布、并取消命令构建软失败回滚与跨帧重试路径的重构计划
> - 权威: 经用户确认保存的设计与重构计划；不作为已实现契约，当前已实现行为见 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `modules/runtime/include/radray/runtime/mesh_draw_command.h`, `modules/runtime/include/radray/runtime/mesh_pass_processor.h`, `modules/runtime/src/mesh_pass_processor.cpp`, `modules/runtime/include/radray/runtime/scene.h`, `modules/runtime/src/scene.cpp`, `modules/runtime/include/radray/runtime/frame_slot.h`, `modules/runtime/src/frame_slot.cpp`, `examples/mesh_drawing_pipeline/pipeline.h`, `examples/mesh_drawing_pipeline/pipeline.cpp`, `modules/runtime/tests/test_mesh_draw_command.cpp`, `modules/runtime/tests/test_mesh_pass_processor.cpp`, `modules/runtime/tests/test_scene.cpp`, `modules/runtime/tests/test_frame_slot.cpp`, `modules/runtime/tests/test_frame_scheduler.cpp`

# Runtime MeshPassProcessor 无状态化、单步发布与移除软失败路径重构计划

## 状态、基线与范围

- 记录日期：2026-11-09
- 源码基线：`75da7a9`
- 状态：已由用户确认采用**显式传入输出 Context + 单步 `AddCommand` 发布 + 前置条件保证不失败（全链路 `void`，违约直接 `RADRAY_ABORT`）**，待实施
- 影响范围：`RadRay::runtime` 的 `mesh_pass_processor.h/.cpp`、`scene.h/.cpp`、`examples/mesh_drawing_pipeline` 及 `modules/runtime/tests/` 相关测试

---

## 一、现状病灶分析

在基线 `75da7a9` 中，[`Scene::SyncCachedMeshDrawCommands`](../../modules/runtime/src/scene.cpp) 通过临时切换 [`MeshPassProcessor`](../../modules/runtime/include/radray/runtime/mesh_pass_processor.h) 内部持有的 `MeshPassDrawListContext*` 指针来决定命令输出位置，并维护一套软失败回滚与脏队列重试逻辑：

```cpp
size_t retryCount = 0;
for (size_t index = 0; index < _dirtyBatches.size(); ++index) {
    ...
    bool failed = false;
    for (const auto& pass : passes) {
        CachedPassMeshDrawListContext output{_cachedDrawLists[pass.Pass.Value], handle.GetIndex()};
        output._validatedBatch = &batch;
        auto& previous = pass.Processor->DrawListContext();
        pass.Processor->SetDrawListContext(output);
        pass.Processor->AddMeshBatch(context);
        pass.Processor->SetDrawListContext(previous);
        failed |= output._buildFailed;
    }
    if (failed) {
        InvalidateBatchCachedCommands(handle.GetIndex());
        _dirtyBatches[retryCount++] = handle;
    } else {
        _batchDirty[handle.GetIndex()] = false;
        --_dirtyCount;
    }
}
_dirtyBatches.resize(retryCount);
```

这一设计存在三处结构性扭曲：

1. **输出目标错位为 Processor 成员状态，迫使调用方构造无意义的“哑 Context”**
   - `MeshPassProcessor` 的职责是结合 Pass 规则与材质属性筛选 `MeshBatch`、选择 PSO 并组装 [`MeshDrawCommandSharedState`](../../modules/runtime/include/radray/runtime/mesh_pass_processor.h)，本质上是与单次输出位置无关的规则对象。
   - 由于构造函数 `MeshPassProcessor(MeshPassDrawListContext&)` 强制要求绑定默认输出上下文，而常驻 Processor 在构造期并不指向任何具体的 `Scene` 缓存槽或当帧 `FrameSlot` 动态列表，调用方被迫声明从不使用的占位成员（如 [`MeshDrawingFramePipeline::_defaultOutput`](../../examples/mesh_drawing_pipeline/pipeline.h)、[`SceneTest::DefaultOutput`](../../modules/runtime/tests/test_scene.cpp) 以及 [`FrameSchedulerTest`](../../modules/runtime/tests/test_frame_scheduler.cpp) 中的临时 `DynamicMeshPassDrawListContext`）。
   - 每次在持久缓存同步或当帧动态命令生成时复用 Processor，都必须执行“保存旧 Context → `SetDrawListContext` → `AddMeshBatch` → 恢复旧 Context”的样板操作，破坏了无状态性与可重入性。

2. **命令构建的“软失败回滚与跨帧重试”是自相矛盾的伪需求**
   - `MeshBatch` 在 [`Scene::AddMeshBatch`](../../modules/runtime/src/scene.cpp) / `UpdateMeshBatch` 入库时已通过 `ValidateBatch` 校验了材质句柄存活、顶点输入与索引缓冲范围。
   - 构建期唯一可能失败的情形是调用方违反前置条件（如 Processor 传入空 `Pipeline`、传入未按 4 字节对齐的 `PushConstants`，或入库后在外部将借用的 `VertexInput` 篡改为越界状态）。这些均属编程错误（Bug），不可能靠“保留脏位等下一帧重试”自愈。
   - 更矛盾的是，`FrameScheduler::RunFrame` 在 `SyncCachedMeshDrawCommands` 之后立即进入 `Extract`；一旦有 Batch 保留脏位（或 `SyncCachedMeshDrawCommands` 因 Pass 登记非法而记录日志提前 `return`），`Scene::GatherVisibleMeshDrawCommands` 会立刻拒绝该脏 Batch，进而触发 Pipeline `RADRAY_ABORT`（如 [`pipeline.cpp:283`](../../examples/mesh_drawing_pipeline/pipeline.cpp)），所谓的“留到下次同步重试”在真实帧循环中根本不会发生。
   - 为了支持这条伪可恢复路径，`MeshPassDrawListContext` 被迫声明 `friend class Scene; friend class MeshPassProcessor;` 并维护 `bool _buildFailed`，`Scene` 被迫维护 `retryCount` 与二次 `InvalidateBatchCachedCommands` 回滚，教学样例的四个 Processor 也各自维护了冗余的 `bool HasErrors{false}`。

3. **历史遗留的 `AddCommand` + `FinalizeCommand` 两阶段虚接口造成冗余状态与重复赋值**
   - 早期按 Element 逐条发射命令时引入了 `AddCommand(initializer, stateSortKey)` 与 `FinalizeCommand(context, elementIndex, command)` 两阶段协议；在 [Batch 级命令收敛](2026-11-06-runtime-batch-command-and-fast-submit-plan.md) 后，每个 Batch 在单次 Pass 中至多生成一条命令，且 [`MeshPassProcessor::BuildMeshDrawCommands`](../../modules/runtime/src/mesh_pass_processor.cpp) 在调用 `AddCommand` 前就已经填好了主元素的 `IndexBuffer`、`IndexCount`、`FirstIndex` 和 `VertexOffset`。
   - 保留两阶段拆分导致：
     1. `BuildMeshDrawCommands` 在第 184～185 行紧挨着连续调用 `AddCommand` 和 `FinalizeCommand`，`FinalizeCommand` 又把主元素的 4 个几何字段重复赋值了一遍；
     2. `CachedPassMeshDrawListContext` 与 `DynamicMeshPassDrawListContext` 被迫各自维护 `_pending` / `_pendingSortKey` 中间状态，只为了把 `stateSortKey` 从 `AddCommand` 传递到下一行的 `FinalizeCommand`。

---

## 二、目标设计契约

### 1. `MeshPassProcessor` 无状态化与全链路 `void` 契约

将输出目标从 `MeshPassProcessor` 成员变量改为显式参数；对齐 `FramePipeline::Extract` / `Record` 的前置条件契约，取消命令构建的软失败返回值，违约直接 `RADRAY_ABORT`：

- **契约定义**：
  - `AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) -> void`：
    - 若当前 Batch 被 Pass 筛选规则合法跳过（不参与本 Pass），不向 `output` 提交命令，直接返回；
    - 若当前 Batch 参与本 Pass，调用 `BuildMeshDrawCommands(context, sharedState, output)`。
  - `BuildMeshDrawCommands(const MeshBatchContext& context, const MeshDrawCommandSharedState& sharedState, MeshPassDrawListContext& output) -> void`：
    - 继续作为 `MeshPassProcessor` 的 `protected static` 辅助函数（直接复用 [`MeshDrawCommandBindingStorage`](../../modules/runtime/include/radray/runtime/mesh_draw_command.h) 已有的 `friend class MeshPassProcessor;` 访问私有 `ValidateConstruction`，对 `output` 只调用公开接口 `HasValidatedElements`、`GetBindingStorage` 和 `AddCommand`）；
    - 元素数量不超过 32 且无可绘制元素（全为空或零索引/零实例）是合法空 Batch，不输出命令直接返回；
    - 传入的 `context`（含借用的 `VertexInput` 与不超过 32 个 `Elements`）与 `sharedState`（`Pipeline`、`ParameterSets`、`PushConstants` 等）必须满足构建前置条件，任何校验失败均视为编程错误，直接通过 `RADRAY_ABORT` 带诊断终止程序。
  - 移除 `MeshPassProcessor` 的有参构造函数、`SetDrawListContext`、`DrawListContext()` 与 `_drawListContext` 成员。

```cpp
class MeshPassProcessor {
public:
    MeshPassProcessor() noexcept = default;
    virtual ~MeshPassProcessor() noexcept = default;

    // 向输出上下文生成至多一条 Batch 命令；未生成命令就返回表示跳过该 Batch。
    virtual void AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) = 0;

protected:
    static void BuildMeshDrawCommands(const MeshBatchContext& context,
                                      const MeshDrawCommandSharedState& sharedState,
                                      MeshPassDrawListContext& output);
};
```

- **`MeshPassProcessor::BuildMeshDrawCommands` 严格执行顺序**：
  1. **前置检查元素数量上限**：检查 `batch.Elements.size() <= 32`，若超过 32（无论元素是否为空）立即 `RADRAY_ABORT`（防止后续掩码左移 `uint32_t{1} << index` 触发未定义行为，也避免 33 个空元素绕过检查提前返回）；
  2. **判定合法空 Batch**：查找首个满足 `IndexCount != 0 && InstanceCount != 0` 的可绘制元素 `firstElement`；若不存在（`firstElement == batch.Elements.end()`），属于合法空 Batch，不访问 `VertexInput` 直接返回；
  3. **前置检查 `VertexInput` 与 `PushConstants` 指针**：在解引用 `batch.VertexInput->Streams` 之前检查 `batch.VertexInput != nullptr && ValidateVertexInput(*batch.VertexInput, sharedState.AllowDeferredInstanceStream)`，若提供了 `sharedState.PushConstants` 则检查 `!source.Data.empty() && source.Data.data() != nullptr`；任一不满足立即 `RADRAY_ABORT`；
  4. **组装命令与基础绑定**：在 `initializer` 中一次性填好 `Pipeline`、`StencilReference`、主元素几何（`IndexBuffer`、`IndexCount`、`FirstIndex`、`VertexOffset`）与 `ValidElementMask`，并组装 `MeshDrawCommandBindings` 的 `ParameterSets`、`PushConstants`、排序后的 `VertexBuffers`、`ExtraElements`、`ElementInstances`、`AllElementsSharePrimaryIndexBuffer` 与 `ContiguousMergeMask`（此时暂不设置 `bindings.InstanceStreamBinding`）；
  5. **调用 `ValidateConstruction` 校验**：调用 `MeshDrawCommandBindingStorage::ValidateConstruction(initializer, bindings, visibleDefaults, !output.HasValidatedElements(batch))` 校验已组装的 `initializer` 与 `bindings`，返回 `false` 时立即 `RADRAY_ABORT`；
  6. **记录延迟实例流槽位（校验后、入池前）**：若 `sharedState.AllowDeferredInstanceStream` 为真且 `batch.VertexInput->Layouts` 中恰有一个 `VertexStepMode::Instance` 布局，将其 `Binding` 写入 `bindings.InstanceStreamBinding`（保持在 `ValidateConstruction` 之后设置，避免构建期无 `submitArgs.InstanceStream` 时将缺失默认实例流误判为违约，提交期仍由 `SubmitMeshDrawCommands` / `ValidateMeshDrawCommand` 严格校验）；
  7. **入池与单步发布**：调用 `output.GetBindingStorage().Store(initializer, bindings)`，计算 `stateSortKey`，并调用 `output.AddCommand(context, primaryElementIndex, initializer, stateSortKey)` 单步发布命令。

### 2. `MeshPassDrawListContext` 收窄为单步发布协议并移除 `friend` 后门

消除 `AddCommand` + `FinalizeCommand` 的两阶段拆分以及 `_pending`、`_pendingSortKey`、`_buildFailed` 和 `friend` 声明：

- **基类 `MeshPassDrawListContext`**：
  - 通过受保护构造函数 `explicit MeshPassDrawListContext(Nullable<const MeshBatch*> validatedBatch = nullptr) noexcept` 初始化 `_validatedBatch`，保留公开只读查询 `HasValidatedElements(const MeshBatch& batch) const noexcept`；
  - 将底层虚接口合并为单步 `AddCommand`（**提交阶段不再重写 `MeshDrawCommand` 的几何字段**，仅转移命令与绑定引用并记录槽位/可见项的排序键及默认实例范围）：
    ```cpp
    // 从已组装完成的 initializer 转移绑定引用所有权，并结合主元素默认实例范围与 stateSortKey 单步发布命令。
    // primaryElementIndex 必须对应 initializer.ValidElementMask 的最低有效位。
    virtual MeshDrawCommand& AddCommand(const MeshBatchContext& context,
                                        size_t primaryElementIndex,
                                        MeshDrawCommand& initializer,
                                        uint32_t stateSortKey = 0) = 0;
    ```
  - 彻底删除 `FinalizeCommand`、`friend class Scene;`、`friend class MeshPassProcessor;` 与 `bool _buildFailed`。

- **缓存实现 `CachedPassMeshDrawListContext`**：
  - 构造函数升级为：
    ```cpp
    CachedPassMeshDrawListContext(CachedPassMeshDrawList& passDrawList,
                                  uint32_t batchIndex,
                                  Nullable<const MeshBatch*> validatedBatch = nullptr) noexcept;
    ```
  - `AddCommand(context, primaryElementIndex, initializer, stateSortKey)` 在单次调用内完成：
    1. 断言本上下文尚未提交过命令（`!_committed`）且 `primaryElementIndex` 对应 `initializer.ValidElementMask` 最低有效位，置 `_committed = true`；
    2. 调用 `_passDrawList->AddCommand(_batchIndex, initializer)` 写入命令并转移绑定引用（不重写命令几何字段）；
    3. 从 `context.Batch.Elements[primaryElementIndex]` 读取默认实例范围，将 `stateSortKey`、`FirstInstance`、`InstanceCount` 写入 `_passDrawList->_batchSlots[_batchIndex]`。
  - 移除 `_pending` 与 `_pendingSortKey`。

- **动态实现 `DynamicMeshPassDrawListContext`**：
  - `AddCommand(context, primaryElementIndex, initializer, stateSortKey)` 在单次调用内完成：
    1. 断言 `!_finalized`、绑定池归属合法且 `primaryElementIndex` 对应 `initializer.ValidElementMask` 最低有效位；
    2. 将已组装的 `initializer` 移入 `Commands` 并清空 `initializer` 的绑定引用（不重写命令几何字段）；
    3. 若已绑定 `_visibleCommands`，从 `context.Batch.Elements[primaryElementIndex]` 读取默认实例范围，立即压入 `_pendingVisibleCommands` 并向 `*_visibleCommands` 追加合成好 `SortKey` 与默认实例范围的 `VisibleMeshDrawCommand`。
  - 移除 `_pendingSortKey`。

### 3. `Scene::SyncCachedMeshDrawCommands` 统一前置契约与简化

[`modules/runtime/src/scene.cpp`](../../modules/runtime/src/scene.cpp) 中：
1. **Pass 登记前置校验（修改缓存前违约直接 `RADRAY_ABORT`）**：
   - 将入口处 `passes[index].Pass.Value == std::numeric_limits<uint32_t>::max()`、重复 `Pass` ID 以及 `passes[index].Processor == nullptr` 从原本的“记录日志并提前 `return`（留下脏 Batch 与旧缓存）”统一升级为 `RADRAY_ABORT`，在触碰任何缓存状态前立即终止。
2. **脏 Batch 同步循环去除重试账本**：
   - 移除 `retryCount`、`failed`、二次 `InvalidateBatchCachedCommands` 回滚与 `_dirtyBatches.resize(retryCount)`；函数返回后 `!HasDirtyBatches()` 恒成立：

```cpp
for (const auto handle : _dirtyBatches) {
    if (!IsBatchDirty(handle)) continue;
    InvalidateBatchCachedCommands(handle.GetIndex());
    const auto& batch = _meshes.Get(handle._handle);
    const auto primitive = _batchOwners[handle.GetIndex()];
    const MeshBatchContext context{_primitives.Get(primitive._handle), batch, primitive, handle, &_materials.Get(batch.Material._handle)};
    for (const auto& pass : passes) {
        CachedPassMeshDrawListContext output{_cachedDrawLists[pass.Pass.Value], handle.GetIndex(), &batch};
        pass.Processor->AddMeshBatch(context, output);
    }
    _batchDirty[handle.GetIndex()] = false;
}
_dirtyBatches.clear();
_dirtyCount = 0;
```

### 4. 教学样例与测试清理

1. **`examples/mesh_drawing_pipeline/pipeline.h/.cpp`**：
   - 删除 `MeshDrawingFramePipeline::_defaultOutput` 冗余成员；
   - `DepthPassProcessor`、`ColorPassProcessor`、`OutlinePassProcessor`、`TransparentPassProcessor` 构造函数移除 `MeshPassDrawListContext&` 参数；
   - 删除四个 Processor 上的 `bool HasErrors{false}` 成员以及 `MeshDrawingFramePipeline::HasErrors()`；
   - 简化 `AcceptsMaterial`（移除 `bool& hasErrors` 参数）：标签匹配返回 `true`，未绑定标签表或标签不匹配打印跳过日志并返回 `false`；材质句柄失效、Proxy 未同步、标签表身份不匹配、缺失 `BatchPassPolicy`、缺失 PSO 或材质参数提取失败等违约情形直接 `RADRAY_ABORT`；
   - 四个 Processor 的 `AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output)` 保持 `void`，末尾直接调用 `BuildMeshDrawCommands(context, {...}, output);`。
2. **单元测试更新（`modules/runtime/tests/`）**：
   - `test_mesh_draw_command.cpp`：迁移 `CachedListMoveRebindsPayloadIdentityAndCanBeReused`（第 459 行）中的 `AddCommand` + `FinalizeCommand` 为单步 `AddCommand({primitive, batch}, 0, initializer)`（`MakeCommand()` 已预填好 `IndexBuffer` 与 `IndexCount`），保留缓存列表移动后的池指针重绑定与复用验证；底层 `ValidateVertexInput` 与 `ValidateMeshDrawCommand` 的非法输入单元测试全部保留在此文件；
   - `test_mesh_pass_processor.cpp`：更新 `BuildingProcessor` 为无参构造与新 `AddMeshBatch` 签名，将原两阶段测试更新为对单步 `AddCommand` 与 `BuildMeshDrawCommands` 的验证；移除针对 `BuildMeshDrawCommands` 返回 `false` 的软失败测试（非法输入校验已由 `test_mesh_draw_command.cpp` 的 `ValidateVertexInput` / `ValidateMeshDrawCommand` 覆盖），在 `BuildRequiresInstanceStreamUnlessDeferred` 中验证 `ValidateVertexInput(Input, false)` 与 `AllowDeferredInstanceStream = true` 下 `InstanceStreamBinding` 的正确记录；
   - `test_scene.cpp`：删除 `SceneTest::DefaultOutput` 与 `RecordingProcessor::BuildSucceeded`，移除 `SyncRestoresProcessorContext`，将 `BorrowedVertexInputIsRevalidatedAfterMarkBatchDirty` 更新为验证修改借用 `VertexInput` 的合法流范围并调用 `MarkBatchDirty` + `Sync` 后缓存命令绑定随之更新；
   - `test_frame_slot.cpp`：将手动构造动态命令的 `AddCommand` + `FinalizeCommand` 两步调用更新为单步 `AddCommand`（在需要实际 `Submit` 的用例中于 `initializer` 上填好主元素 `IndexBuffer`/`IndexCount`/`FirstIndex`）；
   - `test_frame_scheduler.cpp`：删除 `CountingProcessor` 构造所需的临时 `DynamicMeshPassDrawListContext output`。

---

## 三、分阶段实施与验证清单

| 阶段 | 涉及文件 | 关键工作与验收标准 |
|---|---|---|
| **Stage 1：核心接口与实现重构** | `mesh_pass_processor.h/.cpp`<br>`scene.h/.cpp` | 1. 移除 `MeshPassProcessor` 的 `_drawListContext`、`SetDrawListContext`、`DrawListContext()`，将 `AddMeshBatch` 与 `BuildMeshDrawCommands` 统一为接收 `MeshPassDrawListContext& output` 且返回 `void`（违约直接 `RADRAY_ABORT`），保持“检查元素数量上限 → 判定合法空 Batch → 检查 `VertexInput` 与常量数据 → 组装 → `ValidateConstruction` → 设置 `InstanceStreamBinding` → `Store` 入池与单步 `AddCommand` 发布”的严格顺序；<br>2. 将 `MeshPassDrawListContext` 升级为单步 `AddCommand`，提交阶段不再重写几何字段，移除 `FinalizeCommand`、`_buildFailed`、`_pending`、`_pendingSortKey` 及 `friend` 声明；<br>3. `Scene::SyncCachedMeshDrawCommands` 将 Pass 登记违约升级为 `RADRAY_ABORT`，并移除脏队列循环的失败回滚与 `retryCount`。 |
| **Stage 2：教学样例与测试迁移** | `examples/mesh_drawing_pipeline/pipeline.h/.cpp`<br>`test_mesh_draw_command.cpp`<br>`test_mesh_pass_processor.cpp`<br>`test_scene.cpp`<br>`test_frame_slot.cpp`<br>`test_frame_scheduler.cpp` | 1. 清除 `_defaultOutput` / `DefaultOutput` 等全部哑上下文实例以及样例中的 `HasErrors` 账本；<br>2. 迁移全部 5 个测试文件中的单步 `AddCommand` 与 `AddMeshBatch` 新签名；<br>3. 编译并运行 `radrayruntime` 单元测试与 `radray_mesh_drawing_sample` 全部通过。 |
| **Stage 3：长期文档同步** | `docs/architecture/runtime.md`<br>`docs/history/README.md` | 实施完成后同步 `docs/architecture/runtime.md` 中关于 `MeshPassProcessor`、`MeshPassDrawListContext` 与 `Scene::SyncCachedMeshDrawCommands` 的契约说明，运行 `python tools/check_docs.py` 与 `git diff --check` 零报错。 |
