> - 适用: 理解 `RadRay::runtime` 在 2.2 万驻留批次、多块几何缓冲池（Arena）与持续流式增删高压负载下暴露出的底层架构边界缺陷，以及通过收紧 `Scene` 封装边界、正交拆分 `StateSortKey`/`VisibilitySortKey`、构建期预计算 `ContiguousMergeMask`、收敛 `64 B` 单缓存行命令与治理容器尾延迟的下一阶段架构重构计划
> - 权威: 经用户确认保存的架构重构与演进计划；不作为已实现契约，当前已实现行为见 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `modules/core/include/radray/sparse_set.h`, `modules/runtime/include/radray/runtime/mesh_draw_command.h`, `modules/runtime/src/mesh_draw_command.cpp`, `modules/runtime/include/radray/runtime/mesh_batch.h`, `modules/runtime/include/radray/runtime/mesh_pass_processor.h`, `modules/runtime/src/mesh_pass_processor.cpp`, `modules/runtime/include/radray/runtime/scene.h`, `modules/runtime/src/scene.cpp`, `modules/runtime/include/radray/runtime/frame_slot.h`, `modules/runtime/src/frame_slot.cpp`, `modules/runtime/include/radray/runtime/rdg.h`, `modules/runtime/src/rdg.cpp`

# Runtime 架构边界收紧、状态驻留与数据导向重构计划

## 状态、基线与范围

- 记录日期：2026-11-08
- 源码基线：2026-11-08 当前工作树（完成 [Batch 级命令收敛与热路径直达优化计划](2026-11-06-runtime-batch-command-and-fast-submit-plan.md) 后的 `RadRay::runtime` 实现）
- 状态：基于大规模流式体素地形压测与多轮架构边界审视确认的下一阶段重构计划，待实施
- 核心原则：**直接修正底层架构边界，而非在破坏封装的旧契约上堆叠防御性补丁**。涵盖：
  1. 收紧 `Scene`、`CachedPassMeshDrawList` 与 `MeshDrawCommandBindingStorage` 的封装边界，废除外部绕过 `Scene` 直接篡改内部容器与缓存的后门契约；
  2. `VertexBuffersPool` 与 `Payloads` 跨 Batch 引用计数驻留去重（State Interning），并将 `MeshDrawCommand` 压入 **`64 Bytes` 单条缓存行**；
  3. 废除硬编码的 `BaseSortKey | uint32_t(VisibilitySortKey)`，正交拆分 32 位 `StateSortKey` 与 32 位 `VisibilitySortKey`，由 Pass 声明 `StateFirst`（不透明）或 `DepthFirst`（半透明）合成模式；
  4. 将子网格连续合并条件前移至 `Build` 期一次性预计算 `ContiguousMergeMask`，`Submit` 期通过单条位运算执行单实例连续区间合并；
  5. Pass 级紧凑缓存热表、预分配稳定排序（小数组稳定插入排序 + 大数组 LSD 基数排序）、`SparseSet` 几何扩容修复与 RDG 独立外部所有权边界。

---

## 一、背景与压测暴露的真实瓶颈

在完成 Batch 级 `MeshDrawCommand` 收敛与 `ElementMask` 连发绘制后，使用大规模流式场景（常驻 `18,175–22,374` 个 `MeshBatch`、每帧可见 `2,592–3,306` 个 Batch、展开 `8,456–10,249` 次 `DrawIndexed`、几何顶点与索引按 8 MiB Arena 分块子分配并伴随持续流式加载/卸载）对 `RadRay::runtime` 进行了 60 秒级全链路 Tracy CPU/GPU 采样。

扣除外部调用方逻辑后，`RadRay::runtime` 自身的 CPU 耗时分布与异常现象如下：

| 指标 / 阶段 | 原世界 32 区块 (`18.2k` 驻留) | 城市穿行 (`22.3k` 驻留，动态流式) | 城市环视 (`22.3k` 驻留，相机旋转) | 城市固定俯瞰 (`22.4k` 驻留，纯静态) | 压测暴露的核心问题 |
|---|---:|---:|---:|---:|---|
| `RadRay.DrawList.Submit` (均值) | `0.440 ms` | **`0.417 ms`** | `0.124 ms` | `0.182 ms` | 穿行后段随流式碎片化恶化至 **`0.592 ms/帧`** |
| 每帧 `DrawIndexed` 调用数 | `8,456` | `8,139` | `6,348` | **`10,249`** | 固定俯瞰 Draw 最多，但 `Submit` 反而比原世界快 2.4 倍 |
| 每帧 `VertexBuffer` 绑定次数 | **`1,555`** | **`1,252`** (初段 `226` $\to$ 末段 **`2,033`**) | `180` | `294` | `Submit` 耗时与 VB 绑定次数强相关（$r = 0.91 \sim 0.93$），与 Draw 数弱相关（$r = 0.21$） |
| `RadRay.Scene.GatherVisibleHandles` (均值) | `0.093 ms` | **`0.109 ms`** | `0.044 ms` | `0.057 ms` | 每帧对 ~3,000 个可见 Batch 做双层 `SparseSet` 随机查表 |
| `RadRay.DrawList.SortVisible` (均值 / 峰值) | `0.065 ms` | `0.067 ms` | `0.051 ms` / **`2.89 ms`** | `0.064 ms` | `std::stable_sort` 临时堆分配引发 **`2.89 ms`** 尾延迟尖峰 |
| `RadRay.Scene.SyncCachedCommands` (峰值) | **`4.44 ms`** (`BuildBatch` 单次 **`4.21 ms`**) | `0.19 ms` | `0.04 ms` | `0.00 ms` | 驻留数跨过 `18,207` 时触发 18 个 `std::vector` 同步扩容搬移 |

---

## 二、深层架构审视：为什么局部打补丁会陷入边界泥潭？

对上述性能问题及多轮边界推敲的复盘表明，当前 `RadRay::runtime` 存在 **五处底层架构设计缺陷**。若不修正这些架构边界，任何局部优化都会被迫编写大量防御性状态机：

### 架构缺陷一：`Scene` 与 `CachedPassMeshDrawList` 暴露为“全公开可写数据袋”，破坏了不变量封装

1. **现状病灶**：
   - [`Scene`](../../modules/runtime/include/radray/runtime/scene.h) 将底层容器 `Primitives`、`Meshes`、`Materials`、`CachedDrawLists` 全部暴露为 `public` 可写成员，[`MeshBatch`](../../modules/runtime/include/radray/runtime/mesh_batch.h) 也将内部维护账本 `OwningPrimitive`、`Dirty`、`CachedCommands` 暴露为 `public` 可写字段。
   - 現有契约甚至允许调用方绕过 `Scene` 直接调用 `Meshes.Emplace` / `Destroy`、直接改写 `Meshes.Get(h).Material` 或 `Dirty`、以及直接调用 `CachedDrawLists[pass].DestroyCommand` / `Clear()` 破坏内部缓存，再要求 `Scene` 在后续查询中自动检测并兜底。
2. **导致的架构恶果**：
   - 任何高性能的数据导向结构（SoA Pass 热表、材质到 Batch 的反向索引、紧凑池索引）都要求**单一受控的状态变更入口（Narrow Mutation Boundary）**；
   - 敞开内部容器任由外部直接篡改，不仅迫使 `Scene` 在 `RemoveMaterial` / `MarkMaterialDirty` 中退化为 $\Theta(N)$ 全表扫描，还迫使命令与绑定池之间必须携带冗余的 `Generation` 代次校验，使 `MeshDrawCommand` 膨胀至跨缓存行的 `80 Bytes`。

---

### 架构缺陷二：“伪间接句柄池”——1:1 独占分配使提交期状态短路 100% 失效

1. **现状病灶**：
   - [`SubmitMeshDrawCommands`](../../modules/runtime/src/mesh_draw_command.cpp) 设计了 `stateCache.LastVertexBuffersHandle != command.VertexBuffersHandle` 的句柄相等性短路，前提是共享相同顶点流绑定的不同命令持有相同句柄。
   - 然而 [`MeshDrawCommandBindingStorage::Store`](../../modules/runtime/src/mesh_draw_command.cpp) 实现为 **1:1 独占分配**：每条 `MeshDrawCommand` 无条件向 `VertexBuffersPool` 和 `Payloads` 插入新副本。
2. **导致的架构恶果**：
   - `22,374` 个共享约 10 个 8 MiB Arena Buffer 块和 1 套材质的 `MeshBatch`，在池中生成了 `22,374` 份完全相同的副本；
   - 每条缓存命令的句柄互不相同，`LastVertexBuffersHandle` 短路**命中率为 0%**，每帧 ~3,000 条可见命令 100% 触发二级查表与逐槽比对，同时 `Payload` 缺乏句柄级短路缓存。

---

### 架构缺陷三：`Gather` 硬编码 `BaseSortKey | uint32_t(VisibilitySortKey)`，死锁了高低位排序语义

1. **现状病灶**：
   - [`VisibleMeshBatch::VisibilitySortKey`](../../modules/runtime/include/radray/runtime/scene.h) 固定为 `uint32_t`，且 [`Scene::GatherVisibleMeshDrawCommands`](../../modules/runtime/src/scene.cpp) 硬编码了 `.SortKey = info.BaseSortKey | visible.VisibilitySortKey`。
   - 这一公式将“构建期静态属性（`BaseSortKey`）”死锁在高 32 位，将“当帧视角相关属性（`VisibilitySortKey`，如相机深度）”死锁在低 32 位。
2. **导致的架构恶果**：
   - **半透明 Pass 无法实现深度优先排序**：半透明 Pass（如 `Transparent`）要求高 32 位按当帧相机从后到前（Back-to-Front）深度排序、低 32 位按静态状态次级聚类；但在现有 `Gather` 拼接公式下，当帧传入的 32 位 `VisibilitySortKey` 永远进不了高 32 位！
   - **流式槽位复用引发状态抖动**：由于缺乏去重后的状态桶编号，上层将 `BatchHandle.Index` 放入高 32 位，在 [`SparseSet::Destroy`](../../modules/core/include/radray/sparse_set.h) 的 Swap-and-Pop 与 LIFO `_freeHead` 槽位复用后，句柄顺序与物理 Arena Buffer 彻底脱钩，导致 `newisle-flythrough` 运行 40 秒后 `VertexBufferBinds` 从 `226` 暴涨 9 倍至 `2,033` 次/帧，`Submit` 耗时从 `0.158 ms` 恶化至 `0.592 ms`。

---

### 架构缺陷四：子网格可合并性未在构建期归一化，推迟到 `Submit` 期逐位解释

1. **现状病灶**：
   - 同一个 `MeshBatch` 下相邻的 `TriangleList` 子网格（如体素区块的 6 个轴向面组）往往位于同一 `IndexBuffer`、同 `VertexOffset`、单实例且索引区间严格首尾相连（`FirstIndex + IndexCount == next.FirstIndex`）。
   - 当前架构在 `BuildMeshDrawCommands` 期未预计算相邻元素的连续可合并性，[`SubmitMeshDrawCommands`](../../modules/runtime/src/mesh_draw_command.cpp) 对每个可见 bit 单独调用 `std::popcount` 查表并发射独立的 `DrawIndexed`（每帧展开 `8,456–10,249` 次 Draw）。
   - 若把拓扑检查、对齐检查、溢出检查和索引连续性判断直接塞进 `Submit` 内层热循环，又会增加每帧命令录制的分支负担。

---

### 架构缺陷五：AoS 冷热混装、`std::stable_sort` 临时堆分配、`SparseSet` 漏预留与 RDG 边界缺失

1. **`GatherVisible` 双层查表与冷缓存行污染**：每次 `Gather` 都需经 `Meshes.TryGet` 访问包含冷数据的 `MeshBatch`，再遍历 `CachedCommands` 并二次查表 `Commands.TryGet`。
2. **`SortVisible` 临时堆分配尖峰**：[`MeshPassFrameDrawList::SortVisibleCommands`](../../modules/runtime/src/frame_slot.cpp) 使用 `std::stable_sort`，在 MSVC 下动态分配临时归并缓冲并搬移 `32 B` 结构体，引发 **`2.89 ms`** 尾延迟尖峰。
3. **`SparseSet::Reserve` 漏预留与同步扩容尖峰**：[`SparseSet::Reserve`](../../modules/core/include/radray/sparse_set.h) 仅预留了 `_sparse`，漏掉了 `_denseValues` 和 `_sparseIndices`；跨过 `18,207` 阈值时十余个大数组同帧重分配搬移（且 `MeshBatch::Elements` 独立堆分配），造成单次 `BuildBatch` **`4.21 ms`** 尖峰。
4. **RDG 外部队列所有权缺失**：[`RDGBuilder`](../../modules/runtime/include/radray/runtime/rdg.h) 缺乏导入资源的外部队列所有权转移（Acquire / Release）声明，迫使下游每帧额外录制 2 条独立 `CommandBuffer`。

---

## 三、根源架构重构方案

### 模块一：收紧 `Scene` 与 `CachedPassMeshDrawList` 的封装边界（Single Mutation Boundary）

彻底终结“外部绕过 `Scene` 直接修改内部容器与缓存”的后门契约，使所有内部加速结构的不变量由类型系统天然保证：

1. **私有化 `Scene` 内部存储，分离“公开输入描述符”与“内部场景记录”**：
   - 将 `Scene` 的底层存储（Primitives、Batches、Materials、Pass 缓存表、脏队列及材质反向引用表）全部设为 **`private`**；
   - 外部通过受控方法修改场景：
     - `AddPrimitive` / `UpdatePrimitiveTransform` / `UpdatePrimitiveBounds` / `RemovePrimitive`
     - `AddMeshBatch(primitiveHandle, MeshBatchDescriptor)` / `UpdateMeshBatch(batchHandle, MeshBatchDescriptor)` / `RemoveMeshBatch(batchHandle)` / `MarkBatchDirty(batchHandle)`
     - `AddMaterial(MaterialRenderProxy)` / `UpdateMaterial(materialHandle, MaterialRenderProxy)` / `RemoveMaterial(materialHandle)` / `MarkMaterialDirty(materialHandle)`
   - 对外只提供 `const` 只读访问接口（`GetPrimitive`、`GetMeshBatch`、`GetMaterial`、`GetPassCache`），外部不可直接向内部 `SparseSet` 插入/删除元素，不可直接改写 `Dirty` 或 `Material` 字段，也不可直接调用缓存列表的 `DestroyCommand` 或 `Clear()`。
2. **`MeshBatchDescriptor` 内联化与内部账本剥离**：
   - 公开的 `MeshBatchDescriptor`（或只读视图 `MeshBatch`）仅包含业务输入字段：
     ```cpp
     struct MeshBatch {
         SparseSetHandle Material{};
         Nullable<const radray::VertexInput*> VertexInput{nullptr};
         InlineVector<MeshBatchElement, 8> Elements; // 1~8 个子网格 100% 内联，零独立堆分配
         render::PrimitiveTopology Topology{render::PrimitiveTopology::TriangleList};
     };
     ```
   - `OwningPrimitive`、`Dirty` 标记、材质反向关联链索引以及各 Pass 的缓存槽位全部移入 `Scene` 私有 SoA 表中维护。
3. **封闭边界后的直接简化收益**：
   - **材质反向索引零兜底开销**：由于所有 Batch 的创建、更新和销毁必须经过 `Scene` 成员函数，`Scene` 直接维护每个 Material 的引用 Batch 列表；`RemoveMaterial` 恒为严格 **$O(1)$** 引用列表判空，`MarkMaterialDirty` 恒为严格 **$O(N_{\text{referencing\_batches}})$**；
   - **删除脏 Batch 零线性扫描**：`RemoveMeshBatch` 直接将内部 `Dirty` 位置为 `false`（无需 `std::erase(_dirtyBatches, ...)`），`SyncCachedMeshDrawCommands` 弹出时 $O(1)$ 过滤；
   - **按所有权边界拆分缓存同步校验**：
     - `MeshBatch::Elements` 由 `Scene` 私有表**按值独占存储**，外部在 `AddMeshBatch` / `UpdateMeshBatch` 之后无法直接篡改任何子网格元素；因此针对 1~32 个子网格的 `ValidateElement`（含 `IndexBuffer->GetDesc()` 范围检查、`IndexStride` 与索引溢出校验）仅在 `AddMeshBatch` / `UpdateMeshBatch` 入库时执行一次，`SyncCachedMeshDrawCommands` 内部重建时直接跳过逐元素 `IndexBuffer->GetDesc()` 虚调用；
     - `MeshBatch::VertexInput` 仍为外部借用指针（调用方可在外部修改 `VertexInput::Streams` 后调用 `MarkBatchDirty`），且 `AllowDeferredInstanceStream` 由各 Pass 的 `MeshPassProcessor` 在 `BuildMeshDrawCommands` 期决定；因此 `BuildMeshDrawCommands` 始终对 `*batch.VertexInput` 执行一次 `ValidateVertexInput(*batch.VertexInput, sharedState.AllowDeferredInstanceStream)` 及 Processor 提供的 PSO / ParameterSets / PushConstants 校验，校验通过后才将顶点流按值拷入 `VertexBuffersPool`，杜绝外部修改 `VertexInput` 绕过缓冲范围校验。

---

### 模块二：绑定池跨 Batch 驻留去重与 `64 Bytes` 单缓存行 `MeshDrawCommand`

#### 1. `VertexBuffersPool` 与 `Payloads` 引用计数驻留（State Interning）

在 [`MeshDrawCommandBindingStorage`](../../modules/runtime/include/radray/runtime/mesh_draw_command.h) 中，将 `VertexBuffersPool` 与 `Payloads` 升级为基于内容哈希的引用计数驻留池（`ExtraElementsPool` 继续按多元素命令分配）：

```cpp
template <typename T>
struct InternedBindingEntry {
    T Value{};
    uint64_t Hash{0};
    uint32_t RefCount{1};
};
```

- **`Store` 驻留复用**：对已按 `Binding` 升序排列的 `MeshVertexBufferBindings` 和 `MeshDrawCommandPayload` 计算 64 位哈希；命中等价条目时递增 `RefCount` 并复用同一池索引，未命中时分配新槽位。
- **`Destroy` 引用递减**：仅当所属列表销毁命令时递减 `RefCount`，归零时回收槽位。
- **提交期 O(1) 状态跳过与 PSO 切换重置不变量**：
  - 同一 Arena Buffer 块上的数千条命令共享同一个 `VertexBuffersIndex`，[`SubmitMeshDrawCommands`](../../modules/runtime/src/mesh_draw_command.cpp) 中 `stateCache.LastVertexStorage == storage && stateCache.LastVertexBuffersIndex == command.VertexBuffersIndex` 命中率从 **0% 提升至 > 99%**；
  - 在 `MeshDrawCommandStateCache` 中新增 `LastPayloadStorage` 与 `LastPayloadIndex`；
  - **PSO 切换强制重置**：[`MeshDrawCommandStateCache::OnPipelineChanged`](../../modules/runtime/src/mesh_draw_command.cpp) 必须同时清空 `LastVertexStorage`、`LastVertexBuffersIndex`、`LastPayloadStorage`、`LastPayloadIndex` 与 `CommandParameterSets`，确保切换 PSO 后即使下一条命令共享同一 `PayloadIndex` 也会重新绑定 `ParameterSets`；
  - 当 `!pipelineChanged && stateCache.LastPayloadStorage == storage && stateCache.LastPayloadIndex == command.PayloadIndex` 时：若无 `PushConstants`，直接 O(1) 跳过 `GetPayload` 与 `UpdateParameterSet`；若有 `PushConstants`，跳过 `ParameterSets` 比对，仅执行 `SetPushConstants`。

#### 2. `MeshDrawCommand` 收敛为 `64 Bytes`（精确对齐单条 CPU 缓存行）

由于绑定池生命周期已由所属列表完全封装（不再允许外部越权单独销毁池中条目），命令内部无需携带 3 个 64 位 `SparseSetHandle` 的冗余 `Generation` 字段，改用 32 位池槽位索引 `uint32_t`（空索引为 `kInvalidBindingIndex = UINT32_MAX`），使整条命令从 `80 Bytes` 压入 **`64 Bytes` 单条缓存行**：

```cpp
inline constexpr uint32_t kInvalidBindingIndex = std::numeric_limits<uint32_t>::max();

struct alignas(64) MeshDrawCommand {
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};             // 8 B (0..7)
    render::IndexBufferView IndexBuffer{};                                  // 16 B (8..23)
    Nullable<const MeshDrawCommandBindingStorage*> BindingStorage{nullptr}; // 8 B (24..31)
    uint32_t IndexCount{0};                                                 // 4 B (32..35)
    uint32_t FirstIndex{0};                                                 // 4 B (36..39)
    int32_t VertexOffset{0};                                                // 4 B (40..43)
    uint32_t ValidElementMask{1};                                           // 4 B (44..47)
    uint32_t VertexBuffersIndex{kInvalidBindingIndex};                      // 4 B (48..51)
    uint32_t PayloadIndex{kInvalidBindingIndex};                            // 4 B (52..55)
    uint32_t ExtraElementsIndex{kInvalidBindingIndex};                      // 4 B (56..59)
    uint8_t StencilReference{0};                                            // 1 B (60)
    uint8_t Padding[3]{};                                                   // 3 B (61..63)
};
static_assert(sizeof(MeshDrawCommand) == 64 && alignof(MeshDrawCommand) == 64);
```

---

### 模块三：正交拆分 `StateSortKey (32-bit)` 与 `VisibilitySortKey (32-bit)`，缓存与动态命令统一合成律

废除将静态键锁死在高 32 位、将动态键锁死在低 32 位的 `info.BaseSortKey | visible.VisibilitySortKey` 公式，改为缓存路径与动态路径统一的正交双 32 位键合成架构：

1. **构建期 32 位状态分桶键（`uint32_t StateSortKey`）**：
   - 在 `BuildMeshDrawCommands` 完成池驻留、拿到去重后的 `PayloadIndex` 和 `VertexBuffersIndex` 后，默认按 `[PipelineBucket(8) | PayloadBucket(12) | VertexBuffersBucket(12)]` 生成 32 位 `StateSortKey`（Processor 也可在 `MeshDrawCommandSharedState` 中显式指定自定义 `std::optional<uint32_t> CustomStateSortKey`），并通过 `MeshPassDrawListContext::AddCommand(initializer, stateSortKey)` 发布：
     - **空索引编码为 `0`**：`kInvalidBindingIndex` 固定映射为桶 `0`；
     - **有界映射与哈希折叠防溢出**：有效索引 `< MaxBucket`（`255` 或 `4095`）时映射为 `1 + index`；`>= MaxBucket` 时映射为 `1 + (Mix(index) % MaxBucket)` 并与位宽掩码按位与，绝不跨位域污染。
2. **当帧 32 位可见性排序键（`uint32_t VisibilitySortKey`）**：
   - **缓存命令路径**：调用方在 `Gather` 期通过 `VisibleMeshBatch::VisibilitySortKey` 传入当帧视角相关键（如不透明物体的前向深度 Front-to-Back，或半透明物体的后向深度 Back-to-Front）；
   - **动态命令路径**：调用方在当帧构造 [`MeshBatchContext`](../../modules/runtime/include/radray/runtime/mesh_pass_processor.h) 时通过 `MeshBatchContext::VisibilitySortKey`（默认 `0`）传入当帧视角相关键。
3. **Pass 级排序合成模式（`MeshPassSortMode`）与统一合成函数 `ComposeMeshDrawSortKey`**：
   - 在 [`MeshPassRegistration`](../../modules/runtime/include/radray/runtime/mesh_pass_processor.h) 与 `DynamicMeshPassDrawListContext`（含 `MeshPassFrameDrawList`）中声明该 Pass 的合成律：
     ```cpp
     enum class MeshPassSortMode : uint8_t {
         // 不透明 / 深度 Pass 默认：高 32 位按状态桶聚类以最小化绑定切换，低 32 位按前向深度排序
         StateFirst = 0,
         // 半透明 Pass：高 32 位严格按当帧从后到前深度排序，低 32 位在同深度下按状态桶次级聚类
         DepthFirst = 1,
     };

     constexpr uint64_t ComposeMeshDrawSortKey(MeshPassSortMode mode,
                                               uint32_t stateSortKey,
                                               uint32_t visibilitySortKey) noexcept {
         return mode == MeshPassSortMode::DepthFirst
                    ? ((uint64_t{visibilitySortKey} << 32) | stateSortKey)
                    : ((uint64_t{stateSortKey} << 32) | visibilitySortKey);
     }
     ```
   - **缓存与动态路径完全同构**：
     - `CachedPassMeshDrawListContext::FinalizeCommand` 将 `stateSortKey` 写入 `PassBatchCacheSlot::StateSortKey`，`Scene::GatherVisibleMeshDrawCommands` 调用 `ComposeMeshDrawSortKey(passCache.SortMode, slot.StateSortKey, visible.VisibilitySortKey)` 生成 `VisibleMeshDrawCommand::SortKey`；
     - `DynamicMeshPassDrawListContext::FinalizeCommand` 调用同一个 `ComposeMeshDrawSortKey(_sortMode, _pendingStateSortKey, context.VisibilitySortKey)` 生成 `VisibleMeshDrawCommand::SortKey`，确保同一 `MeshPassFrameDrawList` 中缓存命令与动态命令混排时遵循完全一致的 64 位键布局；
   - **架构收益**：相机移动时，半透明 Pass **无需重建任何缓存命令**即可在高 32 位按当帧深度严格执行跨材质从后到前排序（含缓存与动态命令混排）；不透明 Pass 则在任意流式增删后始终将共享同一 PSO、材质和 Arena 顶点缓冲的命令严格聚类。

---

### 模块四：构建期预计算 `ContiguousMergeMask`，提交期单条位运算合并子网格

将子网格连续区间合并的全部复杂校验（拓扑、对齐、同 IB、同 VertexOffset、单实例、索引连续与不溢出）**从每帧 `Submit` 期前移到单次 `Build` 期**：

1. **`MeshDrawCommandElements` 构建期预计算**：
   在 [`MeshDrawCommandElements`](../../modules/runtime/include/radray/runtime/mesh_draw_command.h) 中增加两个由 `BuildMeshDrawCommands` 一次性算好的字段（不增加 `MeshDrawCommand` 本体大小）：
   ```cpp
   struct MeshDrawCommandElements {
       InlineVector<MeshDrawElement, 6> ExtraElements;
       InlineVector<MeshDrawElementInstances, 7> ElementInstances;
       // Bit i (对应源 Elements[i]) 为 1 表示源元素 i 与紧邻的源元素 i+1 皆有效，且满足全部静态连续合并条件
       uint32_t ContiguousMergeMask{0};
       bool AllElementsSharePrimaryIndexBuffer{false};
   };
   ```
   - 在 `BuildMeshDrawCommands` 中，仅当 `sharedState.AllowElementMerging == true`（默认 `false`）且 `context.Batch.Topology` 为独立 List 拓扑（`TriangleList` 各元素 `IndexCount % 3 == 0`、`LineList` `IndexCount % 2 == 0`、`PointList` `IndexCount >= 1`；禁止一切 Strip / Adjacency 拓扑）时，检查源下标相邻的两个有效元素 `elem[i]` 与 `elem[i + 1]`。若同时满足：
     1. 均为默认单实例：`elem[i].InstanceCount == 1 && elem[i + 1].InstanceCount == 1 && elem[i].FirstInstance == elem[i + 1].FirstInstance`（避免多实例合并改变图元交错顺序）；
     2. `SameIndexBufferView(elem[i].IndexBuffer, elem[i + 1].IndexBuffer)`；
     3. `elem[i].VertexOffset == elem[i + 1].VertexOffset`；
     4. `uint64_t{elem[i].FirstIndex} + elem[i].IndexCount == elem[i + 1].FirstIndex`；
     5. 从当前连续段起点累计的 `IndexCount` 不超过 `std::numeric_limits<uint32_t>::max()`；
     则将 `ContiguousMergeMask` 的 bit $i$ 置为 `1`。
2. **`SubmitMeshDrawCommands` 内层极简合并与首 Draw IB 绑定守卫**：
   - 在单条命令的内层循环中维护 `bool boundCommandIndexBuffer = false;`：
     - **本条命令第一个实际发出 Draw（`instanceCount > 0`）的元素**必须与 `stateCache.IndexBuffer` 执行 `SameIndexBufferView` 比对并按需 `BindIndexBuffer`，随后置 `boundCommandIndexBuffer = true`；
     - 仅当 `boundCommandIndexBuffer && elements && elements->AllElementsSharePrimaryIndexBuffer` 为真时，本命令后续实际绘制的元素直接跳过 `SameIndexBufferView` 比较。
   - **基于 `VisibleMeshDrawCommand` 与 `ElementInstances` 解析结果的精确单实例合并守卫**：
     - 在内层循环中，根据 `visible.FirstInstance`、`visible.InstanceCount`、`visible.UseElementFirstInstance`、`visible.UseElementInstanceCount` 与 `elements->ElementInstances[ordinal]` 解析出当前元素 `bit` 的实际 `(firstInstance, instanceCount)`；
     - 由于 `ContiguousMergeMask` 的 bit `bit` 为 `1` 时已在构建期保证源元素 `bit` 与 `bit + 1` 的默认 `InstanceCount == 1` 且默认 `FirstInstance` 相等，无论当帧 `visible` 保持元素默认实例参数（`UseElement* == true` 且 `!ElementInstances.empty()`）还是使用 `visible.FirstInstance` / `visible.InstanceCount` 统一覆盖（`!UseElement*` 或 `ElementInstances.empty()`，涵盖 `Gather` 期 `InstanceCountOverride`、动态命令与手写 `VisibleMeshDrawCommand`），相邻元素 `bit + 1` 的实际 `FirstInstance` 恒与 `firstInstance` 相等，且其实际 `InstanceCount` 为 `1` **当且仅当当前元素解析出的实际 `instanceCount == 1`**；
     - 因此当 **`instanceCount == 1 && elements && elements->ContiguousMergeMask != 0`** 时，内层位扫描只需检查 **`((elements->ContiguousMergeMask & (uint32_t{1} << bit)) != 0) && ((mask & (uint32_t{1} << (bit + 1))) != 0)`**，即可通过 `while` 连续吞并后续相邻 bit 并累加 `IndexCount`，发射单次 `encoder.DrawIndexed`——**内层热循环 0 拓扑分支、0 结构体字段比对、0 溢出重算**！

---

### 模块五：`Scene` Pass 级紧凑热表、零分配稳定排序、几何扩容与 RDG 外部所有权边界

1. **`Scene` Pass 级紧凑缓存热表（`PassBatchCacheSlot`）与稠密命令反向索引维护**：
   - 在收紧 `Scene` 封装边界后，`CachedPassMeshDrawList` 内部采用稠密连续数组 `vector<MeshDrawCommand> Commands` 与反向归属数组 `vector<uint32_t> CommandBatchIndices`（记录每个 `CommandDenseIndex` 所属的 `BatchHandle.Index`），同时为每个已注册 Pass 维护按 `BatchHandle.Index` 直接下标寻址的紧凑热表：
     ```cpp
     struct PassBatchCacheSlot {
         uint32_t CommandDenseIndex{kInvalidBindingIndex}; // 4 B: Commands 稠密数组下标
         uint32_t StateSortKey{0};                         // 4 B: 32 位静态状态桶键
         uint32_t FirstInstance{0};                        // 4 B
         uint32_t InstanceCount{1};                        // 4 B
     };
     static_assert(sizeof(PassBatchCacheSlot) == 16);
     ```
   - **扩容与流式删除下的 $O(1)$ 索引维护**：
     - **新增/扩容**：`AddCommand(batchIndex, command)` 将命令追加至 `Commands` 尾部（下标 `denseIndex`），同步在 `CommandBatchIndices` 记录 `batchIndex` 并写入 `BatchSlots[batchIndex].CommandDenseIndex = denseIndex`；`Commands` 扩容重分配时，整数下标 `CommandDenseIndex` 天然保持有效；
     - **流式删除/重建搬移（Swap-and-Pop）**：销毁 `batchIndex` 在某 Pass 下的旧命令（位于 `removedDenseIndex`）时，先调用 `Bindings.Destroy(Commands[removedDenseIndex])`；若 `removedDenseIndex != Commands.size() - 1`，将末尾命令与末尾 `movedBatchIndex = CommandBatchIndices.back()` 交换搬移至 `removedDenseIndex`，并 **$O(1)$ 修正被搬移 Batch 的热表槽位 `BatchSlots[movedBatchIndex].CommandDenseIndex = removedDenseIndex`**，最后 `pop_back()` 并置 `BatchSlots[batchIndex].CommandDenseIndex = kInvalidBindingIndex`；
   - **单跳直达 Gather**：`GatherVisibleMeshDrawCommands` 先在 `Scene` 的 Batch 状态表中 $O(1)$ 校验 `visible.Batch` 存活且 `!Dirty`（若无效或为脏则立即回滚并返回 `false`）；随后对已注册的 `pass` 直接读取 `16 Bytes` 的 `PassBatchCacheSlot`（若 `pass` 未注册或 `CommandDenseIndex == kInvalidBindingIndex` 则合法跳过），通过 `&passCache.DrawList.Commands[slot.CommandDenseIndex]` **单次数组下标寻址**取得命令地址，彻底消除 `batch.CachedCommands` 遍历与二次 `SparseSet` 稀疏表跳转。
2. **`MeshPassFrameDrawList` 预分配双缓冲稳定排序**：
   - `VisibleCommands.size() <= 64`：使用**原地稳定插入排序（In-place Stable Insertion Sort）**，0 堆分配且保持同键顺序；
   - `VisibleCommands.size() > 64`：复用 `MeshPassFrameDrawList::ScratchCommands` 常驻容量执行 **LSD Radix Sort（最低位优先基数排序）**，自动跳过全数组无差异的字节段，彻底消灭 `std::stable_sort` 的临时堆分配与 `2.89 ms` 尾延迟尖峰。
3. **修复 [`SparseSet::Reserve`](../../modules/core/include/radray/sparse_set.h) 与几何增长 `ReserveGeometric`**：
   - `SparseSet::Reserve(capacity)` 同时预留 `_sparse`、`_denseValues` 与 `_sparseIndices`，供 `Scene::Reserve(primitives, batches, materials)` 高水位预算使用；
   - 新增 `SparseSet::ReserveGeometric(requiredCapacity)`，按 `std::max(requiredCapacity, currentCapacity + currentCapacity / 2)` 几何扩容，杜绝在逐帧小幅流式增长时调用精确 `reserve` 导致的 $O(N^2)$ 逐帧全量重分配。
4. **RDG 独立于执行组的外部队列所有权边界与静态缓冲集合**：
   - 在 `RDGBuilder::ImportTexture` / `ImportBuffer` 中支持声明外部所有权 Acquire / Release 边界；
   - 在 `RDGExecution` 中按资源维护 `PendingExternalAcquire` 与 `NeedsExternalRelease`：无论是首次在 `GroupBarriers` 中被访问，还是在**零执行组图（`_data.Groups.empty()`，仅 `MarkOutput`）**中直接进入 `FinalizeStates`，均消费 `PendingExternalAcquire` 并发出所有权获取屏障；且所有权屏障不受 `CurrentState == TargetState` 常规状态去重省略。

---

## 四、分阶段实施与验证计划

| 阶段 | 涉及文件 | 关键验证指标 |
|---|---|---|
| **Stage 1：收紧 `Scene` 与缓存池封装边界 + `MeshBatch` 内联化 + `SparseSet` 几何扩容** | `sparse_set.h`、`mesh_batch.h`、`scene.h/.cpp`、`test_scene.cpp` | 1. 私有化 `Scene` 内部容器与账本，迁移单测至受控 API；<br>2. 验证 `RemoveMaterial` $O(1)$ 引用检查、`MarkMaterialDirty` 精准标脏、`RemoveMeshBatch` $O(1)$ 惰性清脏，以及外部修改借用 `VertexInput::Streams` 后 `MarkBatchDirty` 重建时仍严格执行顶点流范围校验；<br>3. 验证 `SparseSet::Reserve` 与 `ReserveGeometric`，消除 `18,207` 扩容点处 `4.21 ms` 与 `3.59 ms` 的重分配尖峰。 |
| **Stage 2：状态驻留、`64 B` 命令、`StateSortKey`/`VisibilitySortKey` 正交合成与构建期 `ContiguousMergeMask`** | `mesh_draw_command.h/.cpp`、`mesh_pass_processor.h/.cpp`、`test_mesh_draw_command.cpp`、`test_mesh_pass_processor.cpp` | 1. `static_assert(sizeof(MeshDrawCommand) == 64)`；验证跨 Batch 共享 VB / Payload 池索引与引用计数回收，验证 **PSO 切换后强制重绑 ParameterSets**；<br>2. 验证 `ComposeMeshDrawSortKey` 在 `MeshPassSortMode::StateFirst`（不透明）与 `DepthFirst`（半透明跨材质从后到前排序）下对缓存命令与动态命令混排的一致性及桶号溢出保护；<br>3. 验证 `ContiguousMergeMask` 构建期仅对 `AllowElementMerging == true`、List 拓扑、单实例、连续索引置位，以及 `Submit` 期首实际绘制元素 IB 绑定守卫、多实例覆盖禁用合并与单实例连续合并。 |
| **Stage 3：`16 B` Pass 紧凑热表与零分配稳定排序** | `scene.h/.cpp`、`frame_slot.h/.cpp`、`test_scene.cpp`、`test_frame_slot.cpp` | 1. 验证 `PassBatchCacheSlot` 直查下脏 Batch 拒绝回滚、未注册/已撤销/筛选跳过 Pass 合法空输出、以及缓存扩容与 Swap-and-Pop 删除其他 Batch 后 `CommandDenseIndex` 反向修正与地址正确；<br>2. 验证 `SortVisibleCommands` 在 `<= 64`（稳定插入排序）与 `> 64`（LSD 基数排序）下严格保持同键输入顺序且帧间零额外堆分配。 |
| **Stage 4：RDG 外部所有权边界与文档同步** | `rdg.h/.cpp`、`test_rdg.cpp`、`docs/architecture/runtime.md` | 1. 验证包含 Acquire/Release 的导入资源在常规图、**零执行组图（仅 `MarkOutput`）**及 `BeforeState == AfterState` 下均正确发出所有权屏障；<br>2. 运行 `radray_runtime_tests`、`radray_mesh_drawing_sample` 全部通过；同步更新 `docs/architecture/runtime.md`，运行 `python tools/check_docs.py` 与 `git diff --check` 零报错。 |
