> - 适用: 对齐 UE5 渲染线程 `FScene` 生命周期模型、放开 `RunFrame` 内 `Scene` 只读借用、移除每帧 `MeshDrawCommand` 快照拷贝、优化 `GatherVisibleMeshDrawCommands` 直达寻址与重排 `MeshDrawCommand` 热冷缓存行布局的实施计划
> - 权威: 经用户确认的实施计划（内联容器改造已冻结暂不实施，先验证前三阶段收益）；不作为已实现契约，当前已实现行为见 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `modules/runtime/include/radray/runtime/mesh_draw_command.h`, `modules/runtime/src/mesh_draw_command.cpp`, `modules/runtime/include/radray/runtime/scene.h`, `modules/runtime/src/scene.cpp`, `modules/runtime/include/radray/runtime/frame_slot.h`, `modules/runtime/src/frame_slot.cpp`, `modules/runtime/include/radray/runtime/frame_pipeline.h`, `modules/runtime/include/radray/runtime/render_graph_pipeline.h`, `modules/runtime/src/render_graph_pipeline.cpp`, `modules/runtime/include/radray/runtime/frame_scheduler.h`

# Runtime Scene 帧内借用对齐与 MeshDrawCommand 寻址/布局优化计划

## 状态、基线与范围

- 记录日期：2026-11-06
- 状态：阶段一至阶段三已确认（内联容器改造冻结暂不实施，先验证前三阶段实际收益）
- 范围：涵盖 `RadRay::runtime` 的 `Scene` 帧内只读借用契约、`FramePipeline` / `RenderGraphFramePipeline` 接口、`GatherVisibleMeshDrawCommands` 与 `SubmitMeshDrawCommands` 直达寻址路径，以及 `MeshDrawCommand` 热冷字段缓存行重排。

---

## 一、问题背景与起因：为什么重新审视 `Scene` 借用边界与命令布局

### 1. 每帧命令快照的复制与清理

在 [Runtime RDG 接入与 FramePipeline 跨线程接口拆分计划](2026-11-05-runtime-rdg-pipeline-interface-plan.md) 中，为了切断 `FramePipeline::Record` 对外部 `Scene` 的只读借用，引入了 `MeshPassFrameDrawList::CachedCommands`（`vector<MeshDrawCommand>`）与 `Scene::SnapshotVisibleMeshDrawCommands`：在 `Extract` 阶段按 `VisibleCommands` 逐条解析句柄，将 `Scene` 中的 `MeshDrawCommand` 深拷贝到 `FrameSlot`；并在下一轮槽位复用（`FrameSlot::BeginFrame` -> `MeshPassFrameDrawList::Clear`）时逐条析构旧命令。

### 2. 构图与录制的 Scene 读取边界

禁止 SetupGraph / Record 读取 Scene，会使资源依赖声明和可见命令收集依赖额外的中间存储。
该边界应由帧内只读借用与 CPU 命令寿命约束决定，避免为阶段切换复制本可直接读取的数据。

### 3. 架构根因反思与 UE5 `FScene` 渲染线程模型对照

回溯 `2026-11-05` 计划之所以禁止 `Record` 借用 `Scene`，是因为当时将 `RadRay::Scene` 误当成了“游戏线程（Game Thread）随时会在 `Extract` 刚结束、`Record` 仍在执行时直接调用 `RemoveMeshBatch` 修改的对象”。

对照 Unreal Engine 5 的渲染架构（`Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp`、`MeshDrawCommands.cpp`、`Public/MeshPassProcessor.h`）以及多场景运行时需求，可以明确以下三条核心事实：
1. **`Scene` 本身属于渲染线程（Render Thread）数据结构，且运行期可并存多个 `Scene` 实例**：正如 UE5 中每个 `UWorld`（主世界、预览视口、分屏/子世界）各自拥有独立的 `FScene` 且 `FCachedPassMeshDrawList` 内嵌在 `FScene` 中，`RadRay::Scene` 同样应当作为渲染线程持有的场景表示。将 `CachedDrawLists` 留在 `Scene` 内部天然支持多 `Scene` 切换与并存，不应拆出到全局或单个 `FramePipeline` 中。
2. **`Scene` 的变更与 `RunFrame` 在渲染线程时间轴上天然互斥**：游戏逻辑对场景图元/网格的增删改查，应通过帧间同步或渲染线程命令在 `FrameScheduler::RunFrame` 之外应用到 `Scene`。在整次 `FrameScheduler::RunFrame` 调用期间（从 `SyncCachedMeshDrawCommands` 开始，经过 `Extract`、`SetupGraph`、`RDGBuilder::Compile`，直到 `Record` / `SubmitMeshDrawCommands` 录制完毕返回），`Scene` 全程处于稳定的只读借用状态，不存在帧录制中途 `CachedDrawLists` 元素被销毁或移动的情况。
3. **`MeshDrawCommand` 的生命周期只需覆盖到 `Record` 结束，无需延续到 `Wait(slot)`**：一旦 `SubmitMeshDrawCommands` 在 `Record` 阶段将绑定与绘制参数编码进 RHI `CommandBuffer`，后续的 `SubmitCommandBuffer`（GPU 异步执行）以及数帧后的 `Wait(slot)` 均不再读取 CPU 侧的 `MeshDrawCommand`。真正需要跨帧保活到 `Wait(slot)` 的仅是底层 GPU 资源（通过 `FrameSlot` 与 `RetireResource` 管理）。

### 4. 命令寻址与缓存行布局的开销来源

删除每帧命令快照后，命令解析和对象布局仍需独立审视：

1. **`Submit` 阶段排序后触发逐命令的 `SparseSet` 随机二次间接跳转**：
   - 当 `VisibleMeshDrawCommand` 只存 `CachedMeshDrawCommandHandle` 时，`SortVisibleMeshDrawCommands` 按材质/PSO 排序会把句柄下标彻底打乱；
   - `SubmitMeshDrawCommands` 遍历时，每次调用 `drawList.Commands.TryGet(visibleCommand.CachedCommand)` 都要先随机查 `SparseSet::_sparse[handle.Index]`，再跳转 `_denseValues[denseIndex]`，共产生两次跨大数组的非连续内存间接访问。
2. **`MeshDrawCommand` 单体过大（`sizeof = 480 B`，跨 8 条 64B 缓存行）且热冷字段散落在首尾 6 条缓存行**：
   - 缓存池占用随命令数与单条命令大小增长，大对象中的冷热数据混排会扩大读取工作集；
   - 在当前 `MeshDrawCommand` 字段声明顺序中，每次 `Submit` 必读的热字段分散在 **6 条不同缓存行**：
     - Cache Line 0 (`0..63`): `Pipeline` (`0..7`), `VertexBuffers._size` (`16..23`)
     - Cache Line 2 (`128..191`): `IndexBuffer` (`168..191`)
     - Cache Line 3 (`192..255`): `ParameterSets._size` (`208..215`), `ParameterSets[0]` (`216..223`)
     - Cache Line 5 (`320..383`): `DynamicOffsets._size` (`328..335`)
     - Cache Line 6 (`384..447`): `PushConstants._Has_value` (`440`)
     - Cache Line 7 (`448..479`): `BaseSortKey`, `IndexCount`, `FirstIndex`, `VertexOffset`, `FirstInstance`, `InstanceCount`, `InstanceStreamBinding`, `StencilReference`
   - 排序后按指针访问命令时，即使不用额外顶点流、动态偏移或 PushConstants，分散的热字段仍会跨缓存行读取。
3. **`GatherVisibleMeshDrawCommands` 存在双倍稀疏表查找且为读 `BaseSortKey` 提前污染 `MeshDrawCommand` 缓存行**：
   - 循环内对同一 `batch` 先调 `Meshes.IsAlive(batch)` 再调 `Meshes.Get(batch)`，执行了 2 次完全相同的 `SparseSet` 查表；
   - 为了计算 `VisibleMeshDrawCommand::SortKey`，`Gather` 阶段调用 `drawList->Commands.TryGet(cachedHandle)` 去读 `command->BaseSortKey`（位于 `480 B` 对象的第 8 条缓存行），导致 `Gather` 阶段就要随机触碰 `MeshDrawCommand` 本体；且入口处未对 `outCommands` 调用 `reserve`。

---

## 二、已确认的优化方案（不含内联容器改造细则）

### 阶段一：对齐 UE5 `FScene` 渲染线程定位，放开 `RunFrame` 全程 `Scene` 只读借用并移除每帧命令快照

1. **更新 `Scene` 与 `FrameScheduler::RunFrame` 的生命周期借用契约**：
   - `Scene` 明确定义为渲染线程持有的场景容器（支持多 `Scene` 实例并存），继续直接拥有 `Primitives`、`Meshes`、`Materials` 与 `CachedDrawLists`；
   - 在 `FrameScheduler::RunFrame(Device& device, Scene& scene, uint64_t frameNumber)` 执行期间，开头调用 `scene.SyncCachedMeshDrawCommands(device)` 完成增量缓存同步后，`const Scene&` 的只读借用持续覆盖整帧的 `Extract` 与 `Record`（直至 `RunFrame` 返回）；
   - 约束：在 `RunFrame` 执行期间，外部不得对当前正在渲染的 `Scene` 发起增删改操作。
2. **移除每帧 `MeshDrawCommand` 快照拷贝与析构**：
   - 从 [`MeshPassFrameDrawList`](../../modules/runtime/include/radray/runtime/frame_slot.h) 中删除 `vector<MeshDrawCommand> CachedCommands`；
   - 从 [`Scene`](../../modules/runtime/include/radray/runtime/scene.h) 中删除 `SnapshotVisibleMeshDrawCommands`；
   - `MeshPassFrameDrawList::Clear()` 仅执行 `VisibleCommands.clear()`（由于 `VisibleMeshDrawCommand` 为平凡可析构类型，`clear()` 为 $O(1)$ 修改 `size` 且保留容量复用，避免命令快照的构造与析构）。
3. **允许 `FramePipeline::Record` 与 `RenderGraphFramePipeline::SetupGraph` 只读借用 `const Scene&`**：
   - 将 `FramePipeline::Record` 签名调整为接收 `const Scene& scene`（或在 `RenderGraphFramePipeline::SetupGraph` 中传入 `const Scene& scene`），使调用方管线无需仅为了跨越 `ExtractFrameData -> SetupGraph/Record` 边界而在 `ExtractFrameData` 中提前遍历拷贝场景缓冲集合；
   - 保留 `RenderGraphFramePipeline` 中“先调用 `SetupGraph` 与 `RDGBuilder::Compile()` 裁剪死 Pass，再仅对 `IsPassAlive` 的存活 MeshPass 调用 `ExtractMeshPass`（执行可见命令收集与排序）”的按需提取机制。

### 阶段二：单遍 `Gather` 与直达命令寻址（消除 `Submit` 期 `_sparse` 二次跳转）

参考 UE5 `FCachedMeshDrawCommandInfo` 与 `FVisibleMeshDrawCommand` 的设计（在缓存引用处保存热排序键、在可见命令中直接保存 `const FMeshDrawCommand*`）：

1. **复用 `CachedMeshDrawCommandInfo` 已缓存的元数据，并在 `Gather` 期一次性解析直达指针**：
   - [`MeshBatch::CachedCommands`](../../modules/runtime/include/radray/runtime/mesh_batch.h) 的元素类型 [`CachedMeshDrawCommandInfo`](../../modules/runtime/include/radray/runtime/mesh_draw_command.h) 已经保存了 `BaseSortKey`、`FirstInstance` 与 `InstanceCount`；
   - 在 `GatherVisibleMeshDrawCommands` 中，将原本的 `drawList->Commands.IsAlive(info.CommandHandle)` 替换为单次 `const auto* command = drawList->Commands.TryGet(info.CommandHandle)`，在不额外增加 `_sparse` 查表次数、也不解引用触碰 `MeshDrawCommand` 缓存行的前提下直接拿到 `_denseValues` 中的稳定元素地址。
2. **`VisibleMeshDrawCommand` 支持直达指针寻址（同时兼容动态命令句柄）**：
   - 由于阶段一已确认在单次 `RunFrame` 的 `SyncCachedMeshDrawCommands` 之后 `CachedPassMeshDrawList::Commands` 不会发生增删或扩容重分配，`VisibleMeshDrawCommand` 可直接保存指向缓存命令的 `Nullable<const MeshDrawCommand*> CachedCommand{nullptr}`（保留 `CommandHandle` 以兼容测试或动态命令索引）：
     ```cpp
     struct VisibleMeshDrawCommand {
         MeshDrawCommandSource Source{MeshDrawCommandSource::Cached};
         uint8_t Padding[7]{};
         Nullable<const MeshDrawCommand*> CachedCommand{nullptr};
         SparseSetHandle CommandHandle{};
         uint64_t SortKey{0};
         uint32_t FirstInstance{0};
         uint32_t InstanceCount{1};
     };
     ```
   - `ResolveVisibleMeshDrawCommand` 在 `Source == Cached && visibleCommand.CachedCommand != nullptr` 时直接返回该指针（若仅填了 `CommandHandle` 则回退到 `cachedDrawList->Commands.TryGet(visibleCommand.CommandHandle)`），从而将排序后 `SubmitMeshDrawCommands` 中的逐命令 `SparseSet::_sparse` 随机间接跳转降为 **0 次**。
3. **`GatherVisibleMeshDrawCommands` 内部单遍查表与预留容量**：
   - 函数入口若 `outCommands.capacity() < outCommands.size() + visibleBatches.size()`，先执行 `outCommands.reserve(outCommands.size() + visibleBatches.size())`；
   - 将原先的 `Meshes.IsAlive(visible.Batch)` + `Meshes.Get(visible.Batch)` 合并为单次 `const auto* batchPtr = Meshes.TryGet(visible.Batch)`，省去一半的 `Meshes` 稀疏表查找。

### 阶段三：`MeshDrawCommand` 热冷字段缓存行重排（Cache-Line Packing）

在不改动 `InlineVector` 容器类型的前提下，调整 [`MeshDrawCommand`](../../modules/runtime/include/radray/runtime/mesh_draw_command.h) 的成员声明顺序，按 `SubmitMeshDrawCommands` 的访问频率与局部性将字段划分为三个连续温区：

1. **第一温区（Cache Line 0，前 `64 B`）：100% 绘制必读的定长标量与索引绑定**
   - 将原本散落在结构体首部（偏移 `0`）、中部（偏移 `144..167`）和尾部（偏移 `448..479`）的必读定长字段全部聚拢到开头第 1 条缓存行：
     ```cpp
     struct MeshDrawCommand {
         // --- Cache Line 0 (64 Bytes): 每次 Draw 100% 必读的定长核心字段 ---
         Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};  // 8 B (offset 0..7)
         render::IndexBufferView IndexBuffer{};                       // 24 B (offset 8..31)
         std::optional<uint32_t> InstanceStreamBinding{};             // 8 B (offset 32..39)
         uint32_t IndexCount{0};                                      // 4 B (offset 40..43)
         uint32_t FirstIndex{0};                                      // 4 B (offset 44..47)
         int32_t VertexOffset{0};                                     // 4 B (offset 48..51)
         uint32_t FirstInstance{0};                                   // 4 B (offset 52..55)
         uint32_t InstanceCount{1};                                   // 4 B (offset 56..59)
         uint8_t StencilReference{0};                                 // 1 B + 3 B pad (offset 60..63)
     ```
   - 效果：单次缓存行读取（Line 0，恰好 `64 B`）即可拿到 PSO、IndexBuffer、实例流槽位以及全部绘制标量参数，消除原本跨首、中、尾三条离散缓存行的跳跃。
2. **第二温区（紧随 Line 0 之后）：每次 Draw 必读的绑定集合**
   - 紧接排列 `InlineVector<render::VertexBufferBinding, 4> VertexBuffers` 与 `InlineVector<MeshShaderParameterBinding, 2> ParameterSets`。
3. **第三温区（结构体末尾冷区）：构建期排序键与低频可选 PushConstants**
   - 将仅在构建/缓存同步期读取的 `uint64_t BaseSortKey{0}` 与绝大多数常规网格不使用的 `std::optional<MeshPushConstantCommandBinding> PushConstants{}` 置于结构体最末尾，确保不使用 PushConstants 的绘制在 `Submit` 时不触碰尾部缓存行 bytes。

### 阶段四（已冻结，暂不纳入本轮实施）：内联容器改造

- 围绕 `InlineVector` / `FixedVector` / 外部池的容器层改造先行冻结，待上述阶段一至阶段三落地并实测收益后再决定是否推进。

---

## 三、验证要求

### 1. 独立性能比较

使用固定输入规模的 RadRay 负载，分别比较命令快照、可见收集、排序与 Submit 的 CPU 耗时和分配行为。
保留绘制结果、多 Pass 参与及同键排序语义的一致性。

### 2. 测试与文档同步清单

- **单元测试**：
  - 更新 `modules/runtime/tests/test_scene.cpp`、`test_mesh_draw_command.cpp`、`test_mesh_pass_processor.cpp`、`test_render_graph_pipeline.cpp`、`test_frame_scheduler.cpp` 中涉及 `VisibleMeshDrawCommand`、`GatherVisibleMeshDrawCommands`、`SubmitMeshDrawCommands` 与 `FramePipeline::Record` / `SetupGraph` 签名的用例；
  - 验证多 Pass、材质脏更新、网格移除重建以及 RDG 死 Pass 裁剪场景下的绘制结果一致性。
- **事实文档同步**（在代码实施落地时同步更新）：
  - 更新 [`docs/architecture/runtime.md`](../architecture/runtime.md) 中关于 `Scene` 帧内只读借用边界、`VisibleMeshDrawCommand` 结构以及 `FramePipeline` / `RenderGraphFramePipeline` 的接口契约说明。
