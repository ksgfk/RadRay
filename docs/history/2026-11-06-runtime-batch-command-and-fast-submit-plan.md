> - 适用: 理解 `RadRay::runtime` 在前两轮优化后暴露出的三大结构性缺陷，以及采用“方向 A（Batch 级命令缓存 + `ElementMask` 连发 Draw）+ 方向 B（预排序连续顶点流 + 定长槽位直查 + `32 B` 可见项）”根治 CPU 瓶颈并保证通用多流场景零负优化的实施计划
> - 权威: 经用户确认的结构重构与性能优化计划；不作为已实现契约，当前已实现行为见 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `modules/runtime/include/radray/runtime/mesh_draw_command.h`, `modules/runtime/src/mesh_draw_command.cpp`, `modules/runtime/include/radray/runtime/mesh_batch.h`, `modules/runtime/include/radray/runtime/mesh_pass_processor.h`, `modules/runtime/src/mesh_pass_processor.cpp`, `modules/runtime/include/radray/runtime/scene.h`, `modules/runtime/src/scene.cpp`, `modules/runtime/include/radray/runtime/frame_slot.h`, `modules/runtime/src/frame_slot.cpp`

# Runtime Batch 级命令收敛与热路径直达优化计划（A + B 通用方案）

## 状态、基线与范围

- 记录日期：2026-11-06
- 源码基线：`7e465fe44f6feb0bf0e30a17feaf1e40d5d93266`
- 状态：经调查与通用性推敲确认的下一阶段结构收敛及性能优化计划，待实施
- 范围：涵盖 `RadRay::runtime` 的 `MeshBatch` / `MeshDrawCommand` 缓存粒度收敛、`VisibleMeshBatch` 位掩码可见性收集、`VertexBuffers` 构建期预排序、`VisibleMeshDrawCommand` `32 B` 紧凑对齐，以及 `SubmitMeshDrawCommands` 定长槽位直查与内层连发录制。

---

## 一、遇到的问题：为什么前两轮优化像“按下葫芦浮起瓢”？

### 1. 前序演进复盘

回顾 `2026-11-04` 至 `2026-11-06` 的四次迭代：
1. **[11-04 基础修复](2026-11-04-runtime-mdc-performance-plan.md)**：移除了 Release 下 `SubmitMeshDrawCommands` 每帧重复执行的 `ValidateMeshDrawCommand` 预检（省去 `1.133 ms/帧` 的高频 `Buffer::GetDesc()` 虚调用），并为 `Scene` 补齐 `OwningPrimitive` 反向句柄与 $O(1)$ `Dirty` 标记（消除批量更新时 `7.52 ms` 的 $O(K \times P)$ 线性扫描尖峰）。这部分属于清理原型期实现硬伤。
2. **[11-05 接口拆分](2026-11-05-runtime-rdg-pipeline-interface-plan.md) $\to$ [11-06 第一轮提交 (`842d8f2`)](2026-11-06-runtime-scene-borrow-and-mdc-layout-plan.md)**：`11-05` 误将 `Scene` 视为在 `Record` 期间会被并发修改的对象，为切断 `Record` 对 `Scene` 的借用而引入每帧 `SnapshotCommands` 深拷贝，导致每帧多出 `0.310 ms` 的拷贝与析构开销，并迫使下游在 `ExtractFrameData` 与 `ExtractMeshPass` 中对可见批次扫描两遍。`11-06` 第一轮提交对齐 UE5 `FScene` 渲染线程模型，恢复 `RunFrame` 内 `Scene` 只读借用并移除快照，使收集与清理合计省下 `0.307 ms/帧`。
3. **[11-06 第二轮提交 (`7e465fe`)](2026-11-06-runtime-mdc-soa-pools-plan.md)**：移除快照后，`SnapshotCommands` 不再充当“隐式缓存预取器 + 连续内存紧缩器”，`SubmitMeshDrawCommands` 直接面对 `Scene` 缓存池的跨页稀疏冷读取，耗时从 `0.118 ms` 回升至 `0.261 ms`。为缩小 `472 B` 的胖 `MeshDrawCommand` 并让同一 `MeshBatch` 的多个 Submesh 共享绑定，第二轮提交将 `VertexBuffers`、`ParameterSets`、`PushConstants` 拆入 `MeshDrawCommandBindingStorage` 的三个带引用计数（`References` / `Retain` / `Release`）的 `SparseSet` 池中。

正是第二轮提交中“为了同 Batch 多 Submesh 去重被迫引入引用计数池、二次稀疏表跳转与每条可见命令携带池指针”的复杂性，暴露出当前 Runtime 的数据结构与抽象粒度本身存在三处深层缺陷。

---

### 2. 调查定位的三大结构性缺陷（Structural Mismatches）

#### 缺陷一：缓存与可见性粒度错位 —— 将“1 Batch : N Submesh (Element)”过早拍扁为独立 `MeshDrawCommand`

1. **契约与存储模型的自相矛盾**：
   - 在 [`MeshBatch`](../../modules/runtime/include/radray/runtime/mesh_batch.h) 与 [`MeshPassProcessor::BuildMeshDrawCommands`](../../modules/runtime/src/mesh_pass_processor.cpp) 的契约中，同一个 `MeshBatch` 下的所有 [`MeshBatchElement`](../../modules/runtime/include/radray/runtime/mesh_batch.h) **强制共享同一个 `Material`、同一个 `VertexInput`（全部顶点流）、同一个 `Topology`、同一个 `Pipeline`、同一个 `ParameterSets`、同一个 `PushConstants`、`StencilReference` 和 `BaseSortKey`**。各个 `Element` 之间唯一不同的只有索引范围（`IndexBuffer`、`FirstIndex`、`IndexCount`、`VertexOffset`、`FirstInstance`、`InstanceCount`）。
   - 然而在存储实现上，`BuildMeshDrawCommands` 照搬了 UE5“1 Batch $\approx$ 1 Element”的假设，在缓存构建期通过循环将每个 `Element` 拆散并生成了完全独立的 [`MeshDrawCommand`](../../modules/runtime/include/radray/runtime/mesh_draw_command.h)。
2. **在多 Submesh 场景（如地形每个 Section 含最多 7 个轴向面组 Element，或通用多子网格同材质模型）下的四重连锁放大**：
   - **缓存规模膨胀 5.4 倍**：约 `2,700` 个驻留 `MeshBatch` 被拍扁为 `14,510` 条缓存命令；每帧视锥与朝向剔除后仅 `~19%`（`2,752` 条）可见，导致可见命令在内存中高度稀疏。
   - **`MeshBatch::CachedCommands` 发生隐蔽的堆分配爆炸**：
     [`MeshBatch::CachedCommands`](../../modules/runtime/include/radray/runtime/mesh_batch.h) 定义为 `InlineVector<CachedMeshDrawCommandInfo, 4>`，内联容量仅为 `4`。当一个 `MeshBatch` 含有 $> 4$ 个子网格命令（地形 Section 单 Pass 平均 `5.4` 个、最多 7 个，跨多 Pass 则达 10～14 条）时，**几乎所有 `MeshBatch::CachedCommands` 都会撑爆内联容量 `4`，退化为散落在堆上的独立动态数组**。
   - **`GatherVisibleMeshDrawCommands` 退化为 3 倍重复查表与堆上内层线性扫描**：
     由于 [`VisibleMeshBatch`](../../modules/runtime/include/radray/runtime/scene.h) 每次只能指定单个 `ElementIndex`，当一个可见 Batch 有 3 个子网格可见时，上层被迫传入 3 个 `VisibleMeshBatch`。[`Scene::GatherVisibleMeshDrawCommands`](../../modules/runtime/src/scene.cpp) 对同一个 Batch：
     - 重复执行 3 次 `Meshes.TryGet(visible.Batch)`；
     - 3 次解引用已溢出到堆上的 `batch.CachedCommands`，并在内层 `for` 循环中线性扫描 5～7 个元素去匹配 `info.Pass == pass && info.ElementIndex == visible.ElementIndex`；
     - 3 次调用 `drawList->Commands.TryGet(info.CommandHandle)`。
   - **`Sort` 与 `Submit` 规模膨胀 3 倍，被迫用引用计数池打补丁**：
     同一 Batch 的 3 个可见子网格具有完全相同的 `SortKey`，却生成 3 条 `VisibleMeshDrawCommand` 参与 `std::stable_sort`；且正因为构建期把同属一个 Batch 的共享绑定拆到了多条命令上，才不得不引入 [`MeshDrawBindingEntry`](../../modules/runtime/include/radray/runtime/mesh_draw_command.h) 引用计数池，在构建时 `Store` + 循环 `Retain` + `Release`，再在提交时靠比较 `LastVertexBuffersHandle` 把同一个 Batch 的相邻 Element 重新识别出来。

#### 缺陷二：`VisibleMeshDrawCommand` 存在 7 字节对齐空洞并膨胀至 `40 Bytes`

查看 [`mesh_draw_command.h`](../../modules/runtime/include/radray/runtime/mesh_draw_command.h) 中的 `VisibleMeshDrawCommand` 布局：
```cpp
struct VisibleMeshDrawCommand {
    MeshDrawCommandSource Source{MeshDrawCommandSource::Cached};            // 1B + 7B padding!
    Nullable<const MeshDrawCommand*> CachedCommand{nullptr};                // 8B
    Nullable<const MeshDrawCommandBindingStorage*> BindingStorage{nullptr}; // 8B
    uint64_t SortKey{0};                                                    // 8B
    uint32_t FirstInstance{0};                                              // 4B
    uint32_t InstanceCount{1};                                              // 4B
};
static_assert(sizeof(VisibleMeshDrawCommand) == 40);
```
- `Source` 在 `FinalizeVisibleCommands` 完成后、`SubmitMeshDrawCommands` 执行期间**从未被读取**，却因排在 8 字节指针前造成了 **7 字节 Padding**；
- 结构体从 `32 B` 膨胀到 **`40 B`**（非 2 的幂），导致 `std::stable_sort` 搬移带宽增加 25%，且每两个元素必跨越 `64 B` 缓存行边界。

#### 缺陷三：`SubmitMeshDrawCommands` 内层循环的“通用解释器”开销

即便数据已命中缓存，[`SubmitMeshDrawCommands`](../../modules/runtime/src/mesh_draw_command.cpp) 在每次切换 Batch（每帧 ~913 次）时仍在运行一套代价高昂的动态重排逻辑：
1. 在栈上构造并析构 `264 Bytes` 的 `InlineVector<render::VertexBufferBinding, 8> dirtyVertexBuffers`（[`InlineVector`](../../modules/core/include/radray/inline_vector.h) 含 `24 B` 头部与 `ReleaseStorage()` 析构分支）；
2. 对每个顶点流在 `stateCache.VertexBuffers` 中调用 `std::find_if` 线性查找槽位；
3. 在录制热循环内对 `dirtyVertexBuffers` 调用 `std::sort` 并扫描连续区间；
4. `submitArgs.InstanceStream` 雖在整次 `SubmitMeshDrawCommands` 调用期内恒定不变，却仍混在 `dirtyVertexBuffers` 中参与合并与排序。

---

### 3. 通用性审视：为什么不采用“仅内联 1 个 `PrimaryVertexBuffer` + 溢出池”的特化方案？

在推敲优化方案时，曾考虑过“在 `MeshDrawCommand` 首条缓存行内联 1 个 `PrimaryVertexBuffer`（16B），将第 2～N 个顶点流放入溢出池”。经面向通用 3D 渲染场景的交叉检验，**明确排除该单流特化做法**，原因如下：

1. **现代通用渲染器普遍采用分离多流（De-interleaved Streams）而非单流**：
   - 在标准 glTF / PBR / 现代引擎管线中，常规静态与动态网格通常包含 2～3 个顶点流：
     - **Slot 0（Position Stream）**：仅含 `float3 Position`，供 Depth Prepass 与 Shadow Pass 单独绑定以节省一半带宽；
     - **Slot 1（Attribute Stream）**：含 `Normal / Tangent / UV0`，在 Base / Color Pass 中与 Slot 0 一同绑定；
     - **Slot 2（Color / LightmapUV / Skinning Stream）**：顶点色或蒙皮权重。
2. **若将顶点流劈裂为“1 个内联 + N-1 个池化”，在通用多流场景下会产生三重负优化**：
   - **两头落空**：Color Pass 中几乎所有网格都有 $\ge 2$ 个流，此时既在每条命令中白占了 16 字节内联空间，又依然必须访问外部池读取剩余流；
   - **破坏连续 `span` 下发**：D3D12 `IASetVertexBuffers` 与 Vulkan `vkCmdBindVertexBuffers` 均期望一次接收连续槽位的绑定数组。若 Slot 0 在命令内联字段、Slot 1..N 在池中，`Submit` 被迫拆成 2 次 RHI 调用或在栈上重新拷贝拼装临时数组；
   - **破坏多 Batch 共享同一 `VertexInput` 时的单句柄 $O(1)$ 短路**：一个多材质复杂模型的多个 `MeshBatch`（或共享 MegaBuffer 的批次）往往拥有完全相同的 2～3 个顶点流。保持全部顶点流作为一个整体存储在 `VertexBuffersPool` 中，切换 Batch 时只需比对 1 次 `VertexBuffersHandle` 即可跳过全部流的检查。
3. **配合方向 A 后，池体积已自然缩小 5.4 倍，无需靠劈裂内联赌单流特例**：
   - 采用方向 A 按 Batch 收敛命令后，常驻命令与绑定池条目均从 `14,510` 降至 `~2,700`（合计仅数百 KB，完全常驻 L2/L3 Cache）。保持 `VertexBuffers` 在池中作为**构建期预排序的连续整体**，可让单流地形与多流通用场景走完全一致的高速路径。

---

## 二、核心重构设计方案（方向 A + 方向 B 通用方案）

### 模块一（方向 A）：Batch 级命令缓存 + `ElementMask` 可见收集 + 内层紧凑连发 Draw

#### 1. 缓存粒度收敛：1 个 `MeshBatch` 在 1 个 Pass 下只生成 1 条 `MeshDrawCommand`

将单条 `MeshBatchElement` 的索引绘制参数定义为平凡紧凑的 `MeshDrawElement`（`32 Bytes`），一条 `MeshDrawCommand` 直接代表整个 `MeshBatch` 在该 Pass 下的绘制命令：

```cpp
// 单个 Submesh Element 的紧凑索引绘制参数（32 Bytes，100% 平凡可拷贝/析构）
struct MeshDrawElement {
    render::IndexBufferView IndexBuffer{}; // 16 B: Target(8) + Offset(4) + Stride(4)
    uint32_t IndexCount{0};                // 4 B
    uint32_t FirstIndex{0};                // 4 B
    int32_t VertexOffset{0};               // 4 B
    uint32_t ElementIndex{0};              // 4 B (对应源 MeshBatch::Elements 下标)
};
static_assert(sizeof(MeshDrawElement) == 32 && std::is_trivially_copyable_v<MeshDrawElement>);
```

- **单 Submesh 与多 Submesh 统一表达**：
  - `MeshDrawCommand` 首条缓存行直接保存第 0 个有效元素的索引绘制参数（`IndexBuffer`、`IndexCount`、`FirstIndex`、`VertexOffset`）以及 `ValidElementMask`；
  - 当 `MeshBatch` 仅有 1 个有效 `Element`（常规单分段网格）时，无需分配任何额外元素池条目；
  - 当 `MeshBatch` 拥有多个有效 `Element`（如地形 Section 的 2～7 个轴向面组，或多子网格同材质模型）时，其余有效 `MeshDrawElement` 存入 `MeshDrawCommandBindingStorage::ElementsPool`。
- **`MeshBatch::CachedCommands` 彻底告别堆溢出**：
  - 1 个 `MeshBatch` 在 1 个 Pass 下**最多生成 1 条 `MeshDrawCommand`**，[`CachedMeshDrawCommandInfo`](../../modules/runtime/include/radray/runtime/mesh_draw_command.h) 移除 `ElementIndex` 字段：
    ```cpp
    struct CachedMeshDrawCommandInfo {
        MeshPassId Pass{};
        SparseSetHandle CommandHandle{};
        uint64_t BaseSortKey{0};
        uint32_t FirstInstance{0};
        uint32_t InstanceCount{1};
    };
    ```
  - 只要场景注册的 Pass 数 $\le 4$，`MeshBatch::CachedCommands`（`InlineVector<CachedMeshDrawCommandInfo, 4>`）**100% 保持内联，0 次堆内存分配**。
- **废除 `MeshDrawCommandBindingStorage` 的引用计数（`References` / `Retain` / `Release`）**：
  - 不再把 1 个 Batch 拆成 $N$ 条共享绑定的 Element 命令后，每条 `MeshDrawCommand` 与其池中绑定条目恢复严格的 **1:1 独占所有权**（或在构建时直接移动存入），`DestroyCommand` 直接销毁对应句柄，彻底移除 `MeshDrawBindingEntry` 引用计数包装。

#### 2. 可见性收集（`Gather`）：`VisibleMeshBatch` 支持 `ElementMask` 位掩码

扩展 [`VisibleMeshBatch`](../../modules/runtime/include/radray/runtime/scene.h)，允许调用方通过 32 位掩码一次性提交同一个 `MeshBatch` 下所有可见的 Submesh（单个 `MeshBatch` 最多支持 32 个 Submesh；同时提供辅助方法兼容单 `ElementIndex`）：

```cpp
struct VisibleMeshBatch {
    SparseSetHandle Batch{};
    // Bit i (1u << i) selects Batch.Elements[i]; all bits set selects every valid element in the batch.
    uint32_t ElementMask{std::numeric_limits<uint32_t>::max()};
    uint32_t VisibilitySortKey{0};
    std::optional<uint32_t> FirstInstanceOverride{};
    std::optional<uint32_t> InstanceCountOverride{};

    static constexpr uint32_t AllElementsMask = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t MaskForElement(uint32_t elementIndex) noexcept {
        return elementIndex < 32 ? (uint32_t{1} << elementIndex) : 0u;
    }
};
```

- 在 [`Scene::GatherVisibleMeshDrawCommands`](../../modules/runtime/src/scene.cpp) 中：
  - 913 个可见地形 Section 只需传入 **913 个 `VisibleMeshBatch`**（而非 `2,752` 个）；
  - 每个可见 Batch 仅执行 **1 次** `Meshes.TryGet(visible.Batch)`；
  - 在内联的 `batch.CachedCommands`（仅 1～4 项）中直接匹配 `info.Pass == pass`；
  - 计算 `const uint32_t activeMask = visible.ElementMask & command->ValidElementMask;`，若非零则仅向 `outCommands` 追加 **1 条** `VisibleMeshDrawCommand`（携带 `activeMask`）。

#### 3. 排序与录制（`Sort` & `Submit`）：按 Batch 排序与绑定，内层位扫描连发 `DrawIndexed`

- **`SortVisibleCommands`**：仅对 **913 条** Batch 级 `VisibleMeshDrawCommand` 排序（规模缩减 **67%**）。
- **`SubmitMeshDrawCommands`**：
  - 外层循环每条可见 Batch 命令只执行 **1 次** PSO、Pass 参数组、`VertexBuffers`、模板参考值、`ParameterSets` 与 `PushConstants` 的状态去重与绑定；
  - 内层循环根据 `visible.ElementMask`（已在 Gather 期或入口与 `command.ValidElementMask` 求交）连续发射该 Batch 下所有可见的 Submesh：
    ```cpp
    uint32_t mask = visible.ElementMask & command.ValidElementMask;
    while (mask != 0) {
        const uint32_t bit = static_cast<uint32_t>(std::countr_zero(mask));
        mask &= (mask - 1);
        const auto elem = ResolveCommandElement(command, storage, bit);
        if (!stateCache.IndexBuffer || !SameIndexBufferView(*stateCache.IndexBuffer, elem.IndexBuffer)) {
            encoder.BindIndexBuffer(elem.IndexBuffer);
            stateCache.IndexBuffer = elem.IndexBuffer;
        }
        encoder.DrawIndexed(elem.IndexCount, visible.InstanceCount, elem.FirstIndex, elem.VertexOffset, visible.FirstInstance);
    }
    ```
  - 对于通用 `1 Batch = 1 Element` 网格，`mask` 仅有 1 个 bit 为 1，内层循环只执行 1 次，零额外开销；
  - 对于多 Submesh 网格（如地形 Section 的 3 个可见面组，或多分段同材质网格），内层循环直接连发 `DrawIndexed`，中间**零 PSO/VB/ParameterSet/PushConstants 状态检查分支**。

---

### 模块二（方向 B）：预排序连续顶点流、定长槽位直查与 `32 B` 可见项对齐

#### 1. `MeshDrawCommand` 保持 `80 B` 平凡热结构，`VertexBuffers` 在构建期预排序并整体连续存储

```cpp
struct MeshDrawCommand {
    // --- Cache Line 0 (64 Bytes): 每次 Batch 切换必读的热数据与句柄 ---
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr}; // 8 B (0..7)
    render::IndexBufferView PrimaryIndexBuffer{};               // 16 B (8..23): Target(8) + Offset(4) + Stride(4)
    SparseSetHandle VertexBuffersHandle{};                      // 8 B (24..31)
    SparseSetHandle ParameterSetsHandle{};                      // 8 B (32..39)
    SparseSetHandle PushConstantsHandle{};                      // 8 B (40..47)
    SparseSetHandle ExtraElementsHandle{};                      // 8 B (48..55, 仅当 ValidElementMask 含 >1 个元素时有效)
    uint32_t ValidElementMask{0};                               // 4 B (56..59)
    uint8_t StencilReference{0};                                // 1 B (60)
    uint8_t Padding[3]{};                                       // 3 B (61..63)

    // --- 第二缓存行 (16 Bytes): 首元素绘制标量（构建/Gather 元数据移入 CachedMeshDrawCommandInfo） ---
    uint32_t PrimaryIndexCount{0};                              // 4 B (64..67)
    uint32_t PrimaryFirstIndex{0};                              // 4 B (68..71)
    int32_t PrimaryVertexOffset{0};                             // 4 B (72..75)
    uint32_t PrimaryElementIndex{0};                            // 4 B (76..79)
};
static_assert(sizeof(MeshDrawCommand) == 80);
static_assert(std::is_trivially_copyable_v<MeshDrawCommand> && std::is_trivially_destructible_v<MeshDrawCommand>);
```

- **构建期预排序 `VertexBuffers`**：
  在 [`MeshPassProcessor::BuildMeshDrawCommands`](../../modules/runtime/src/mesh_pass_processor.cpp) 中，将 `batch.VertexInput->Streams` 转为 `bindings.VertexBuffers` 后，**立即按 `Binding` 槽位号升序排列**再存入 `VertexBuffersPool`。
  这样在缓存有效期内，无论是 1 个流还是 4 个分离流，`VertexBuffersPool` 中存储的永远是**已按槽位排好序的连续 `VertexBufferBinding` 数组**。

#### 2. `MeshDrawCommandStateCache` 改用定长槽位数组 $O(1)$ 直查，消除内层 `InlineVector`、`find_if` 与 `std::sort`

```cpp
struct MeshDrawCommandStateCache {
    static constexpr uint32_t kMaxFastVertexSlots = 16;
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    std::optional<render::IndexBufferView> IndexBuffer{};
    std::optional<uint8_t> StencilReference{};
    std::array<render::VertexBufferView, kMaxFastVertexSlots> BoundVertexSlots{};
    uint16_t BoundVertexMask{0};
    ...
};
```

1. **Pass 级 `submitArgs.InstanceStream` 提至 PSO 切换时单次绑定**：
   - `submitArgs.InstanceStream` 在整个 Pass 提交期间恒定不变。当 `pipelineChanged && submitArgs.InstanceStream` 时，直接绑定该实例流槽位并更新 `BoundVertexSlots[instSlot]`，不再在每条命令的 VB 更新中重复合并与排序。
2. **命令级 `VertexBuffers` 零排序、按槽位 $O(1)$ 过滤与原位连续子区间下发**：
   - 当 `command.VertexBuffersHandle != stateCache.LastVertexBuffersHandle` 时，从 `VertexBuffersPool` 取出**已按 `Binding` 升序排列**的 `std::span<const render::VertexBufferBinding> bindings`；
   - 遍历 `bindings`（1～4 项），若 `binding.Binding < kMaxFastVertexSlots`，直接通过下标 `BoundVertexSlots[binding.Binding]` $O(1)$ 判断视图是否变化（彻底消除 `std::find_if`）；
   - 因为 `bindings` 本身已按 `Binding` 升序排列，对于发生变化的连续槽位区间（例如通用 PBR 网格同时切换 Slot 0 Position 与 Slot 1 Attribute），**直接将 `bindings` 的连续子 `span` 原位传给 `encoder.BindVertexBuffers( bindings.subspan(runStart, runLen) )`**——**0 次 `dirtyVertexBuffers` 栈容器构造、0 次元素拷贝、0 次 `std::sort`**！

#### 3. 将 `VisibleMeshDrawCommand` 从 `40 B` 压回 `32 B`（精确对齐半条缓存行）

移除 `Submit` 期不读取的 `Source` 字段（消除 7 字节空洞），将 `SortKey` 置于结构体偏移 `0`，并合入 `ElementMask`（`BindingStorage*` 由所属列表或命令上下文提供，或紧凑编码）：

```cpp
struct VisibleMeshDrawCommand {
    uint64_t SortKey{0};                                                    // 8 B (0..7)
    Nullable<const MeshDrawCommand*> CachedCommand{nullptr};                // 8 B (8..15)
    Nullable<const MeshDrawCommandBindingStorage*> BindingStorage{nullptr}; // 8 B (16..23)
    uint32_t FirstInstance{0};                                              // 4 B (24..27)
    uint32_t InstanceCount : 24 {1};                                        // 3 B } 合用 4 B (28..31) 或独立字段
    uint32_t ElementMask : 8 {0xFF};                                        // 1 B }
};
```
> 或将 `BindingStorage*` 存于 `MeshPassFrameDrawList`（静态列表指针 + 动态列表指针，通过命令来源区分），即可让 `VisibleMeshDrawCommand` 在 `32 Bytes` 内拥有完整的 32 位 `FirstInstance` (4B)、32 位 `InstanceCount` (4B) 与 32 位 `ElementMask` (4B)：
```cpp
struct VisibleMeshDrawCommand {
    uint64_t SortKey{0};                                     // 8 B (0..7)
    Nullable<const MeshDrawCommand*> CachedCommand{nullptr}; // 8 B (8..15)
    uint32_t FirstInstance{0};                               // 4 B (16..19)
    uint32_t InstanceCount{1};                               // 4 B (20..23)
    uint32_t ElementMask{std::numeric_limits<uint32_t>::max()}; // 4 B (24..27)
    MeshDrawCommandSource Source{MeshDrawCommandSource::Cached}; // 1 B + 3 B pad (28..31)
};
static_assert(sizeof(VisibleMeshDrawCommand) == 32);
```
- 恰好 **`32 Bytes`**（每条 `64 B` 缓存行精确容纳 2 个元素，无跨行撕裂），且 `SortKey` 位于偏移 `0`，加速 `std::stable_sort` 比较。

---

## 三、全链路收益对比（兼顾体素地形与通用 PBR 多流场景）

| 维度 | 当前基线 (`7e465fe`，逐 Element + 动态重排) | A + B 通用方案（Batch 级命令 + 预排序连续 VB + 掩码连发） | 改善机理与通用性保证 |
|---|---|---|---|
| 常驻 `Commands` 数量（以 2,700 Section 地形为例） | **`14,510` 条**（`1.16 MB`） | **`~2,700` 条**（`0.21 MB`，常驻 L2 Cache） | 消除 1 Batch $\to$ $N$ Element 的过早拍扁；对 `1 Batch = 1 Element` 通用网格则为 1:1 零额外开销 |
| `MeshBatch::CachedCommands` 堆分配 | 超过 4 个子网格命令时 **100% 触发堆内存分配** | 每个 Pass 仅 1 条记录，**0 次堆内存分配**（100% 内联） | 无论单个 Batch 有多少个 Submesh，每个 Pass 固定只占 1 个槽位 |
| 绑定池引用计数管理 | 3 个 `SparseSet` 均维护 `References` 计数与 `Retain`/`Release` | **0 引用计数**（命令与绑定条目严格 1:1 独占） | 同 Batch 的多个 Element 聚于同一命令内，无需跨命令共享句柄 |
| `GatherVisible` 查表与扫描 | 传入 `2,752` 项，重复查表与堆上线性扫描 | 传入 **`913` 项**（带 `ElementMask`），单次查表直取 | 多 Submesh 场景查表次数砍掉 **67%**；单 Submesh 场景保持单次直取 |
| `SortVisible` 排序规模与体积 | `2,752 × 40 B =` **`110 KB`**（跨缓存行边界） | `913 × 32 B =` **`29.2 KB`**（精确对齐 64B 缓存行） | 任何场景下排序元素体积均缩小 **20%**；多 Submesh 场景总排序带宽减少 **73.5%** |
| `Submit` 顶点流绑定（单流 & 通用 2～4 分离流） | 每次切换在栈上构造 `264B` `InlineVector` + `std::find_if` + `std::sort` | 构建期预排序 + `BoundVertexSlots[slot]` **$O(1)$ 直查** + 原位连续 `subspan` 下发 | **流越多收益越大**：零栈容器构造、零 `find_if`、零 `std::sort`，不劈裂多流数据 |
| `Submit` 多 Submesh 绘制 | 每个 Submesh 均跑一遍完整外层状态机 | 外层每 Batch 绑定 1 次，内层 5 行代码位扫描连发 `DrawIndexed` | 消除同 Batch 子网格间的全部冗余状态比对 |

---

## 四、分步实施与验证计划

### 1. 实施步骤清单

1. **第一步：重构 `MeshDrawCommand`、`MeshDrawElement` 与 `VisibleMeshDrawCommand`（`mesh_draw_command.h/.cpp`）**
   - 在 `MeshDrawCommand` 中保留首元素索引参数与 `ValidElementMask`，在 `MeshDrawCommandBindingStorage` 中新增 `ExtraElementsPool` 并移除 `MeshDrawBindingEntry` 引用计数包装；
   - 将 `VisibleMeshDrawCommand` 收敛为 **`32 Bytes`**（`SortKey` 置首，包含 `ElementMask`），添加 `static_assert(sizeof(VisibleMeshDrawCommand) == 32)`。
2. **第二步：更新 `MeshPassProcessor` 构建期 VB 预排序与 Batch 级命令生成（`mesh_pass_processor.h/.cpp`、`scene.h/.cpp`）**
   - `MeshPassProcessor::BuildMeshDrawCommands` 将 `bindings.VertexBuffers` 按 `Binding` 升序排列后存入池，并将整个 `MeshBatch` 的所有非空 `Elements` 汇总生成 **1 条** `MeshDrawCommand`；
   - `CachedMeshDrawCommandInfo` 移除 `ElementIndex` 字段；`VisibleMeshBatch` 支持 `ElementMask`，更新 `Scene::GatherVisibleMeshDrawCommands` 执行单次查表与掩码过滤。
3. **第三步：优化 `SubmitMeshDrawCommands` 为定长槽位直查 + 原位连续 `subspan` 下发 + 内层掩码连发（`mesh_draw_command.cpp`）**
   - `MeshDrawCommandStateCache` 使用定长 `BoundVertexSlots` 数组按槽位下标 $O(1)$ 比对，在 PSO 切换时单独绑定 `submitArgs.InstanceStream`，对已预排序的 `VertexBuffers` 直接切分连续脏区间 `subspan` 下发，移除 `dirtyVertexBuffers` 与 `std::sort`；
   - 外层完成 Batch 级绑定后，内层按 `visible.ElementMask & command.ValidElementMask` 位扫描连续调用 `encoder.DrawIndexed`。
4. **第四步：同步单元测试与架构文档**
   - 更新 `modules/runtime/tests/` 下的 `test_mesh_draw_command.cpp`、`test_mesh_pass_processor.cpp`、`test_scene.cpp`、`test_frame_slot.cpp`（覆盖单流、分离多流、单 Element 与多 Element `ElementMask` 场景）；
   - 运行 `radray_runtime_tests`、`radray_mesh_drawing_sample` 与 `python tools/check_docs.py` 验证通过；
   - 实施完成后同步更新 [`docs/architecture/runtime.md`](../architecture/runtime.md)。
