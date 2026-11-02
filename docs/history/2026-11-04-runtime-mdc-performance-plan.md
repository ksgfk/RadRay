> - 适用: 实施 `RadRay::runtime` Mesh Drawing Pipeline（MDC）的 CPU 录制、去重、脏追踪与可见收集性能优化
> - 权威: 经用户确认的实施计划；不作为已实现契约，当前行为与待实施差异见 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `modules/runtime/include/radray/runtime/mesh_draw_command.h`, `modules/runtime/src/mesh_draw_command.cpp`, `modules/runtime/include/radray/runtime/mesh_batch.h`, `modules/runtime/include/radray/runtime/scene.h`, `modules/runtime/src/scene.cpp`, `modules/runtime/src/mesh_pass_processor.cpp`, `modules/runtime/src/mesh_pass_draw_list_context.cpp`, `modules/runtime/tests/test_mesh_drawing_pipeline.cpp`

# Runtime Mesh Drawing Pipeline 性能优化实施计划

## 状态、基线与范围

- 记录日期：2026-11-04
- 源码基线：`c8bc3bdc035b52433c7eab72cc2d92c714cbda46`
- 状态：设计已确认，待实施
- 范围：严格限定在 `modules/runtime/`（含其单测 `modules/runtime/tests/test_mesh_drawing_pipeline.cpp`），**不向 `modules/render/`（RHI 层）外溢改动**。
- 背景：实机 Tracy 采样（大场景稳态：平均 3,464 个驻留网格、3,687 条可见子网格命令、19,098 条缓存绘制命令）定位出 `RadRay::runtime` 的 4 处 CPU 性能退化：
  1. `SubmitMeshDrawCommands` 每帧对全部可见命令重复执行重量级 `ValidateMeshDrawCommand` 预检（含高频 `Buffer::GetDesc()` 虚调用与二次 `SparseSet` 查找），耗时约 **1.133 ms / 帧**；
  2. `SubmitMeshDrawCommands` 录制循环仅对 `Pipeline` 去重，其余状态（`VertexBuffers`、`InstanceStream`、`IndexBuffer`、`StencilReference`、命令级 `ParameterSets`）在每条命令上无条件重绑；
  3. `Scene` 缺乏 `MeshBatch -> OwningPrimitive` 反向句柄与 $O(1)$ `Dirty` 标记，批量更新 2,116 个 `MeshBatch` 时退化为 $O(K \times P)$ 与 $O(D^2)$ 线性扫描（约 447 万次遍历），产生 **7.52 ms** 帧尖峰；
  4. `GatherVisibleMeshDrawCommands` 为获取默认实例范围无条件解引用 `Commands` 稀疏集大对象数组，且 `MeshDrawCommand::VertexBuffers` 使用堆分配 `vector` 并在 `BuildMeshDrawCommands` 中产生冗余深拷贝。

---

## 一、已确认的设计决策

| 编号 | 决策项 | 确认方案 | 设计理由与 UE 5.8.4 对照 |
|---|---|---|---|
| **D1** | `SubmitMeshDrawCommands` 预检策略 | **对齐 UE5：Release 下跳过提交期预检循环**，仅在命令构建期（`BuildMeshDrawCommands`）与 Debug 构建（`RADRAY_IS_DEBUG`）下执行提交期预检 | UE5 在 `FMeshBatch::Validate` 与 `FMeshDrawShaderBindings::Finalize`（`DO_GUARD_SLOW`）完成校验，`SubmitDraw` 零预检、零 `GetDesc()` 虚调用。Release 单遍解析并录制可彻底消除每帧数万次虚函数分发与二次 `SparseSet::TryGet` |
| **D2** | `SubmitMeshDrawCommands` 状态去重范围 | **在 `runtime` 层实现全状态去重（`MeshDrawCommandStateCache`）**，覆盖 `Pipeline`、按槽位 `VertexBuffers`（含 `InstanceStream`）合并下发、`IndexBuffer`、`StencilReference`、按 `GroupIndex` 的命令级 `ParameterSets`；**不修改 `modules/render`（不外溢）** | 对齐 UE5 `FMeshDrawCommandStateCache`。在 `SubmitMeshDrawCommands` 局部作用域内完成按槽位过滤与单次 `BindVertexBuffers` 合并下发后，Pass 级 `InstanceStream` 每个 PSO 仅绑定 1 次、同 Batch 相邻子网格 Slot 0 顶点流仅绑定 1 次，无需改动 D3D12/Vulkan RHI 实现 |
| **D3** | `Scene` 反向句柄与 $O(1)$ 脏追踪 | 在 `MeshBatch` 上新增由 `Scene` 维护的 `SparseSetHandle OwningPrimitive{}` 与 `bool Dirty{false}` | 对齐 UE5 `FStaticMeshBatch::PrimitiveSceneInfo` 反向指针与 `bPendingAddStaticMeshes` 1-bit 标志，使 `FindOwningPrimitive`、标脏查重和 `GatherVisible` 的脏检查全部降为 $O(1)$ |
| **D4** | `CachedMeshDrawCommandInfo` 缓存默认实例范围与 `VertexBuffers` 内联 | `CachedMeshDrawCommandInfo` 新增 `FirstInstance` / `InstanceCount`；`MeshDrawCommand::VertexBuffers` 改为 `InlineVector<render::VertexBufferBinding, 4>`；`BuildMeshDrawCommands` 消除逐 Element 预检深拷贝 | 对齐 UE5 `FCachedMeshDrawCommandInfo`（`InitViews` 不触碰 `FMeshDrawCommand` 缓存行）与 `FVertexInputStreamArray` 内联存储 |

---

## 二、分项详细设计

### 1. 提交期预检收敛与单遍解析（问题 1 / D1）

#### 1.1 `SubmitMeshDrawCommands` 执行流
在 `modules/runtime/src/mesh_draw_command.cpp` 中：
1. 入口检查 `if (!encoder.IsValid()) return false;`。
2. **Debug 预检（`#if RADRAY_IS_DEBUG`）**：
   - 先对 Pass 级 `submitArgs` 执行 **1 次**预校验：
     - `ValidateParameterSets(submitArgs.ParameterSets)`；
     - 若 `submitArgs.InstanceStream` 存在，校验 `ValidateVertexBuffer(submitArgs.InstanceStream->View)`。
   - 再遍历 `visibleCommands` 解析 `command` 并调用 `ValidateMeshDrawCommand(*command, visible, submitArgs)`，任一失败立即返回 `false`（不触碰 `encoder`）。
3. **Release 路径（`#if !RADRAY_IS_DEBUG`）**：
   - 不执行预检循环，直接进入单遍录制循环。
   - 在录制循环内：
     - 先判断 `if (visible.InstanceCount == 0) continue;`（零实例条目甚至无需解引用命令）；
     - 调用 1 次 `const auto commandPtr = ResolveVisibleMeshDrawCommand(visible, cachedDrawList, dynamicCommands); if (!commandPtr) return false;`，避免二次 `SparseSet::TryGet`。

#### 1.2 构建期零冗余拷贝校验（配合问题 4）
在 `modules/runtime/src/mesh_pass_processor.cpp` 的 `MeshPassProcessor::BuildMeshDrawCommands` 中：
- 共享状态（`sharedState.Pipeline`、`batch.VertexInput`、`sharedState.ParameterSets`、`sharedState.PushConstants`、`initializer.VertexBuffers`）在构造 `initializer` 时**只校验 1 次**，不再在每个 `Element` 循环中执行 `auto command = initializer;` 触发深拷贝。
- 对每个非零索引/实例的 `MeshBatchElement`，仅通过轻量辅助函数 `ValidateMeshBatchElement(element)` 校验：
  - `element.IndexBuffer` 非空且 `IsValid()`；
  - `element.IndexStride == 2 || element.IndexStride == 4`，且 `element.IndexBufferOffset % element.IndexStride == 0`；
  - `element.IndexBuffer->GetDesc()` 具备 `render::BufferUse::Index` 且字节范围不越界；
  - `uint64_t{element.FirstInstance} + element.InstanceCount - 1 <= std::numeric_limits<uint32_t>::max()`。
- 当 `batch` 中有效 `Element` 数量为 1 时，向 `DrawListContext().AddCommand(...)` 传递 `initializer`（静态/动态 Context 可直接消费），彻底消除额外对象构造。

---

### 2. Runtime 层全状态去重 `MeshDrawCommandStateCache`（问题 2 / D2）

在 `modules/runtime/src/mesh_draw_command.cpp` 的匿名命名空间内新增局部状态缓存结构 `MeshDrawCommandStateCache`（仅在单次 `SubmitMeshDrawCommands` 调用内有效，不跨调用保存，不改动 `modules/render`）：

#### 2.1 状态缓存数据结构
```cpp
struct MeshDrawCommandStateCache {
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    std::optional<render::IndexBufferView> IndexBuffer{};
    std::optional<uint8_t> StencilReference{};
    InlineVector<render::VertexBufferBinding, 8> VertexBuffers;
    InlineVector<MeshShaderParameterBinding, 4> CommandParameterSets;

    void OnPipelineChanged(render::GraphicsPipelineState* newPipeline) noexcept {
        Pipeline = newPipeline;
        // 遵守 RHI 契约：切换 PSO 后必须重新绑定 VB 与 ShaderParameterSet
        VertexBuffers.clear();
        CommandParameterSets.clear();
    }
};
```

#### 2.2 各状态判等与去重规则
1. **PSO 与 Pass 级 `ParameterSets`**：
   - 当 `stateCache.Pipeline.Get() != command.Pipeline.Get()` 时：
     - 调用 `encoder.BindGraphicsPipelineState(command.Pipeline.Get())`；
     - 调用 `stateCache.OnPipelineChanged(command.Pipeline.Get())` 清空已缓存的 `VertexBuffers` 与 `CommandParameterSets`（保留 `IndexBuffer` 与 `StencilReference`，因为 RHI 契约保证 PSO 切换不改变 IB 与 StencilRef）；
     - 依次绑定 `submitArgs.ParameterSets`。
2. **VertexBuffers（含 `submitArgs.InstanceStream` 覆盖）按槽位去重 + 单次合并下发**：
   - 定义 `VertexBufferView` 判等：`a.Target == b.Target && a.Offset == b.Offset && a.Size == b.Size`。
   - 对当前 `command`：
     - 用 `InlineVector<render::VertexBufferBinding, 8> dirtyVertexBuffers;` 收集需要下发的槽位；
     - 遍历 `command.VertexBuffers`：若该槽位等于 `submitArgs.InstanceStream->Binding` 则跳过（由 `submitArgs.InstanceStream` 接管）；否则在 `stateCache.VertexBuffers` 中按 `Binding` 查找：未命中或 `View` 不同时，更新 `stateCache.VertexBuffers` 并追加到 `dirtyVertexBuffers`。
     - 若存在 `submitArgs.InstanceStream`：在 `stateCache.VertexBuffers` 中查找 `submitArgs.InstanceStream->Binding`：未命中或 `View` 不同时，更新 `stateCache.VertexBuffers` 并追加到 `dirtyVertexBuffers`。
     - 若 `!dirtyVertexBuffers.empty()`，调用 **1 次** `encoder.BindVertexBuffers(dirtyVertexBuffers)`（若连续的槽位可一次传入，或若 RHI `BindVertexBuffers` 会将 `[lowest, highest]` 之间的未提供槽位视为空槽，需注意！）。
   - **关键边界核查（稀疏槽位安全）**：
     - 核查 `d3d12_impl.cpp:3092` 与 `vulkan_impl.cpp:3769`：
       - D3D12 的 `BindVertexBuffers(bindings)` 计算 `lowest` 和 `highest`，然后调用 `_cmdList->IASetVertexBuffers(lowest, highest - lowest + 1, rawViews.data())`！如果本次只传 `{slot 0}` 和 `{slot 2}`（中间跳过了已绑定的 `{slot 1}`），`IASetVertexBuffers(0, 3, rawViews)` 会用零视图**冲掉** `slot 1`！
       - Vulkan 的 `BindVertexBuffers(bindings)` 同样调用 `vkCmdBindVertexBuffers(cmd, lowest, highest - lowest + 1, buffers, offsets)`，如果 `bindings` 中有不连续槽位，中间的 `buffers[index]` 是 `VK_NULL_HANDLE`！
     - **因此，`runtime` 层的安全合并策略为**：
       - 将 `dirtyVertexBuffers` 按 `Binding` 升序排列（通常只有 1～2 个元素），将**连续的 `Binding` 区间（contiguous runs）**合并为一次 `encoder.BindVertexBuffers(runSpan)` 调用！
       - 例如：首条命令同时绑定 `slot 0` 和 `slot 1` $\to$ 构成连续区间 `[0, 1]`，合并为 **1 次** `encoder.BindVertexBuffers` 调用；后续命令 `slot 1`（`InstanceStream`）已被缓存命中，只有 `slot 0` 变化 $\to$ 仅对 `[0, 0]` 调用 **1 次** `encoder.BindVertexBuffers`；同一 `MeshBatch` 的后续子网格 `slot 0` 与 `slot 1` 均命中缓存 $\to$ **0 次**调用！
       - 这样既完全不修改 `modules/render`，又 100% 规避了 D3D12/Vulkan `lowest..highest` 连续区间覆盖问题。
3. **IndexBuffer 去重**：
   - 定义 `IndexBufferView` 判等：`a.Target == b.Target && a.Offset == b.Offset && a.Stride == b.Stride`。
   - 仅当 `!stateCache.IndexBuffer || !SameIndexBufferView(*stateCache.IndexBuffer, command.IndexBuffer)` 时调用 `encoder.BindIndexBuffer(command.IndexBuffer)` 并更新 `stateCache.IndexBuffer`。
4. **StencilReference 去重**：
   - 仅当 `!stateCache.StencilReference || *stateCache.StencilReference != command.StencilReference` 时调用 `encoder.SetStencilReference(command.StencilReference)` 并更新 `stateCache.StencilReference`。
5. **命令级 `ParameterSets` 按 `GroupIndex` 去重**：
   - 定义 `MeshShaderParameterBinding` 判等：`GroupIndex` 相同、`ParameterSet.Get()` 指针相同，且 `DynamicOffsets` 数量与每项 `(BindingId, ArrayIndex, Offset)` 完全相同。
   - 对 `command.ParameterSets` 中的每项 `binding`：在 `stateCache.CommandParameterSets` 中查找同 `GroupIndex` 条目；若已存在且完全相等则跳过 `encoder.BindShaderParameterSet`，否则更新缓存并下发绑定。
6. **命令级 `PushConstants`**：
   - 若 `command.PushConstants` 有值，保持现状调用 `encoder.SetPushConstants`，失败立即返回 `false`。

---

### 3. `Scene` 反向句柄与 $O(1)$ 脏追踪（问题 3 / D3）

#### 3.1 `MeshBatch` 结构扩展（`modules/runtime/include/radray/runtime/mesh_batch.h`）
```cpp
struct MeshBatch {
    // Resolves only in the owning Scene's Materials container.
    SparseSetHandle Material{};
    // Borrowed; must remain at a stable address for all referencing batches.
    Nullable<const radray::VertexInput*> VertexInput{nullptr};
    vector<MeshBatchElement> Elements;
    render::PrimitiveTopology Topology{render::PrimitiveTopology::TriangleList};
    // Managed by Scene; callers must not copy cache entries or scene links into another batch.
    SparseSetHandle OwningPrimitive{};
    bool Dirty{false};
    InlineVector<CachedMeshDrawCommandInfo, 4> CachedCommands;
};
```

#### 3.2 `Scene` 方法复杂度与不变量（`modules/runtime/src/scene.cpp`）
1. **`AddUniqueDirtyBatch(SparseSetHandle batchHandle)` — $O(1)$**：
   ```cpp
   void Scene::AddUniqueDirtyBatch(SparseSetHandle batchHandle) {
       if (!Meshes.IsAlive(batchHandle)) return;
       auto& batch = Meshes.Get(batchHandle);
       if (!batch.Dirty) {
           batch.Dirty = true;
           _dirtyBatches.push_back(batchHandle);
       }
   }
   ```
2. **`ResolveOwningPrimitive(SparseSetHandle batchHandle)` — $O(1)$ 快路径 + 兼容回退**：
   - 若 `batch.OwningPrimitive.IsValid()` 且 `Primitives.IsAlive(batch.OwningPrimitive)`，直接返回 `batch.OwningPrimitive`（$O(1)$）；
   - 仅当调用方绕过 `Scene::AddMeshBatch` 直接向公开 `Primitives` / `Meshes` 塞入数据导致 `OwningPrimitive` 为空时，回退调用 `FindOwningPrimitive(batchHandle)` 并将结果写回 `batch.OwningPrimitive`。
3. **`AddPrimitive` / `AddMeshBatch` / `UpdateMeshBatch` / `RemoveMeshBatch`**：
   - `AddPrimitive`：校验 `primitive.Meshes` 中每个 `handle` 存活且尚未关联其他 Primitive（检查 `ResolveOwningPrimitive(handle)` 无效）；`Primitives.Emplace` 拿到 `primitiveHandle` 后，将每个子 Batch 的 `OwningPrimitive` 设为 `primitiveHandle` 并调用 `AddUniqueDirtyBatch(handle)`。
   - `AddMeshBatch`：设置 `batch.OwningPrimitive = primitiveHandle; batch.Dirty = false; batch.CachedCommands.clear();`，插入后调用 `AddUniqueDirtyBatch(handle)`。
   - `UpdateMeshBatch`：通过 $O(1)$ `ResolveOwningPrimitive(batchHandle)` 验证归属；保留原 `target.OwningPrimitive`、`target.Dirty` 与 `target.CachedCommands`，移动赋值后调用 `AddUniqueDirtyBatch(batchHandle)`。
   - `RemoveMeshBatch`：若 `batch.Dirty` 为 `true`，才执行 `std::erase(_dirtyBatches, batchHandle)` 并置 `batch.Dirty = false`；对于稳态非脏 Batch，删除无需扫描 `_dirtyBatches`（$O(1)$）。
4. **`SyncCachedMeshDrawCommands`**：
   - 当首次初始化或 Pass 注册变化需要全量重建时：先清空 `_dirtyBatches`，将所有 Batch 的 `Dirty` 置 `false` 并清理缓存命令；随后遍历 `Primitives` 及其 `Meshes`，回填 `batch.OwningPrimitive = primitiveHandle` 并调用 `AddUniqueDirtyBatch(handle)`。
   - 处理 `auto dirty = std::move(_dirtyBatches); _dirtyBatches.clear();` 时：
     - 先对每个存活的 `handle` 将 `Meshes.Get(handle).Dirty = false;`；
     - 通过 $O(1)$ `ResolveOwningPrimitive(handle)` 取得 `primitiveHandle`；若校验失败，调用 `AddUniqueDirtyBatch(handle)` 重新标脏（此时 `Dirty` 重新置为 `true` 并回到 `_dirtyBatches`）。
5. **`GatherVisibleMeshDrawCommands` — $O(1)$ 脏检查**：
   - 将原有的 `if (!Meshes.IsAlive(visible.Batch) || std::find(_dirtyBatches.begin(), _dirtyBatches.end(), visible.Batch) != _dirtyBatches.end())` 替换为：
     ```cpp
     if (!Meshes.IsAlive(visible.Batch)) {
         outCommands.resize(initialSize);
         return false;
     }
     const auto& batch = Meshes.Get(visible.Batch);
     if (batch.Dirty) {
         outCommands.resize(initialSize);
         return false;
     }
     ```

---

### 4. 可见命令收集零解引用与 `VertexBuffers` 内联（问题 4 / D4）

#### 4.1 `CachedMeshDrawCommandInfo` 缓存默认实例范围（`mesh_draw_command.h` & `mesh_pass_draw_list_context.cpp`）
- 在 `CachedMeshDrawCommandInfo` 中新增默认实例范围字段：
  ```cpp
  struct CachedMeshDrawCommandInfo {
      MeshPassId Pass{};
      uint32_t ElementIndex{0};
      SparseSetHandle CommandHandle{};
      uint64_t BaseSortKey{0};
      uint32_t FirstInstance{0};
      uint32_t InstanceCount{1};
  };
  ```
- 在 `CachedPassMeshDrawListContext::FinalizeCommand` 中记录：
  ```cpp
  _targetBatch->CachedCommands.push_back({
      _passId,
      static_cast<uint32_t>(elementIndex),
      _pendingHandle,
      command.BaseSortKey,
      command.FirstInstance,
      command.InstanceCount});
  ```
- 在 `Scene::GatherVisibleMeshDrawCommands` 中：
  不再调用 `drawList->Commands.Get(info.CommandHandle)`，只保留 `drawList->Commands.IsAlive(info.CommandHandle)` 句柄存活校验，直接使用 `info.FirstInstance` 与 `info.InstanceCount`：
  ```cpp
  outCommands.push_back({.Source = MeshDrawCommandSource::Cached,
                         .CommandHandle = info.CommandHandle,
                         .SortKey = info.BaseSortKey | visible.VisibilitySortKey,
                         .FirstInstance = visible.FirstInstanceOverride.value_or(info.FirstInstance),
                         .InstanceCount = visible.InstanceCountOverride.value_or(info.InstanceCount)});
  ```

#### 4.2 `MeshDrawCommand::VertexBuffers` 改为 `InlineVector`（`mesh_draw_command.h`）
- 将 `MeshDrawCommand::VertexBuffers` 定义由：
  `vector<render::VertexBufferBinding> VertexBuffers;`
  改为：
  `InlineVector<render::VertexBufferBinding, 4> VertexBuffers;`
- 常规 1～4 个顶点/实例缓冲流完全内联在 `MeshDrawCommand` 内部，零堆内存分配。

---

## 三、文件级改动清单

| 文件路径 | 改动内容 |
|---|---|
| `modules/runtime/include/radray/runtime/mesh_draw_command.h` | `MeshDrawCommand::VertexBuffers` 改为 `InlineVector<render::VertexBufferBinding, 4>`；`CachedMeshDrawCommandInfo` 增加 `FirstInstance` 与 `InstanceCount`；更新 `SubmitMeshDrawCommands` 注释说明 Debug 预检与状态去重契约 |
| `modules/runtime/src/mesh_draw_command.cpp` | `SubmitMeshDrawCommands` 的预检循环改为 `#if RADRAY_IS_DEBUG`（且先单次校验 `submitArgs`）；录制循环引入 `MeshDrawCommandStateCache` 对 PSO、连续槽位 VB、IB、StencilRef、命令级 ParameterSets 去重，并合并单次 `ResolveVisibleMeshDrawCommand` |
| `modules/runtime/include/radray/runtime/mesh_batch.h` | `MeshBatch` 增加 `SparseSetHandle OwningPrimitive{}` 与 `bool Dirty{false}` |
| `modules/runtime/include/radray/runtime/scene.h` | 增加私有辅助声明 `SparseSetHandle ResolveOwningPrimitive(SparseSetHandle batchHandle) noexcept;` |
| `modules/runtime/src/scene.cpp` | `AddUniqueDirtyBatch`、`RemoveMeshBatch`、`UpdateMeshBatch`、`SyncCachedMeshDrawCommands` 接入 `OwningPrimitive` 与 `Dirty` 实现 $O(1)$ 查找与去重；`GatherVisibleMeshDrawCommands` 检查 `batch.Dirty` 并从 `CachedMeshDrawCommandInfo` 直接读取默认实例范围 |
| `modules/runtime/src/mesh_pass_draw_list_context.cpp` | `CachedPassMeshDrawListContext::FinalizeCommand` 将 `command.FirstInstance` 与 `command.InstanceCount` 写入 `CachedMeshDrawCommandInfo` |
| `modules/runtime/src/mesh_pass_processor.cpp` | `MeshPassProcessor::BuildMeshDrawCommands` 消除逐 Element 的 `auto command = initializer;` 深拷贝，改为共享状态校验 1 次 + 逐 Element 校验 IB 与实例范围 |
| `modules/runtime/tests/test_mesh_drawing_pipeline.cpp` | 1. 更新 `PreflightsPassParametersAndRebindsAfterPipelineChanges` 中相邻同 PSO 同命令级 `ParameterSet` 去重后的绑定次数断言（从 10 次更新为 9 次：`{0, 1, 2, 0, 1, 2, 0, 1, 2}`），并验证 IB/VB/StencilRef 去重行为；<br>2. 将依赖 `SubmitMeshDrawCommands` 在提交期拦截非法 `submitArgs` / 篡改后 `Dynamic` 命令的断言与 `RADRAY_IS_DEBUG` 对齐（Release 下补充对 `ValidateMeshDrawCommand` 的直接断言）；<br>3. 新增批量更新/标脏与多子网格共享 VB/IB/Stencil 的状态去重测试 |
| `docs/architecture/runtime.md` | 落地实施后同步更新 `Scene`、`MeshDrawCommand` 与 `SubmitMeshDrawCommands` 的当前契约描述，并移除临时「已确认、未实现」段落 |

---

## 四、验证与验收条件

1. **构建与单元测试**：
   - 构建 Debug 与 Release 配置下的 `test_radray_runtime` 与 `radray_mesh_drawing_sample`；
   - 运行 `ctest --preset win-x64-debug -R "MeshDrawingPipeline|FrameLoop|Material"` 与 Release 对应测试，全部通过。
2. **教学样例双后端验证**：
   - 运行 `radray_mesh_drawing_sample --backend d3d12` 与 `--backend vulkan`，开启 RHI validation layer 无报错，输出图像正确。
3. **关键行为与性能不变量核验**：
   - `CpuEncoder` 录制观测：同一 `MeshBatch` 的多个 `Element` 或相邻同状态命令在 `SubmitMeshDrawCommands` 中不重复调用 `BindVertexBuffers`、`BindIndexBuffer`、`SetStencilReference` 和 `BindShaderParameterSet`；切换 PSO 后正确重绑 Pass 级与命令级 `ParameterSets` 及 `VertexBuffers`；
   - `GatherVisibleMeshDrawCommands` 在无 Override 与有 Override 两种情况下均输出正确的 `FirstInstance` / `InstanceCount`，且不访问 `CachedPassMeshDrawList::Commands` 的稠密命令元素；
   - 文档检查：`python tools/check_docs.py` 与 `git diff --check` 零错误。
