> - 适用: 将 `MeshDrawCommand` 中的非平凡变长绑定容器（`VertexBuffers`、`ParameterSets`、`PushConstants`）拆出至独立 SoA 数组/池、将 `MeshDrawCommand` 收敛为平凡定长热结构并在 `SubmitMeshDrawCommands` 中按句柄短路去重的实施计划
> - 权威: 经用户确认立项的 SoA 拆分优化计划；不作为已实现契约，当前已实现行为见 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `modules/runtime/include/radray/runtime/mesh_draw_command.h`, `modules/runtime/src/mesh_draw_command.cpp`, `modules/runtime/include/radray/runtime/mesh_pass_processor.h`, `modules/runtime/src/mesh_pass_processor.cpp`, `modules/runtime/include/radray/runtime/scene.h`, `modules/runtime/src/scene.cpp`, `modules/runtime/include/radray/runtime/frame_slot.h`, `modules/runtime/src/frame_slot.cpp`

# Runtime MeshDrawCommand SoA 绑定池拆分优化计划

## 状态、基线与范围

- 记录日期：2026-11-06
- 状态：用户提出并要求制定的下一阶段优化计划（承接 [Runtime Scene 帧内借用对齐与 MeshDrawCommand 寻址/布局优化计划](2026-11-06-runtime-scene-borrow-and-mdc-layout-plan.md) 阶段一至三落地后的实测分析）
- 范围：涵盖 `RadRay::runtime` 中 `MeshDrawCommand` 的 SoA（Structure of Arrays）数据拆分、`CachedPassMeshDrawList` 与 `DynamicMeshPassDrawListContext` 的分池存储、`MeshPassProcessor::BuildMeshDrawCommands` 的批次级共享写入，以及 `SubmitMeshDrawCommands` 的句柄级状态去重。

---

## 一、遇到了什么问题：移除每帧命令快照与字段重排后的实测瓶颈

### 1. 三轮配对 Tracy 采样的完整表现

在实施完上一阶段的前三步改造（① 放开 `RunFrame` 内 `Scene` 只读借用并移除每帧 `SnapshotCommands` / `ClearSnapshots`；② `VisibleMeshDrawCommand` 直接保存 `const MeshDrawCommand*` 指针；③ 不改内联容器的前提下重排 `MeshDrawCommand` 字段顺序）后，采集 `tracy-session-20261106-175340-deep` 并与前两轮采集在相同稳态负载（`steady_2500_3000`：无几何上传、无纹理变化、无脏缓存重建，常驻约 `14,500–14,800` 条缓存命令，每帧 `2,714–2,752` 次可见 Draw）下对比：

| 指标（稳态每帧均值） | 无快照最初基线 (`20261105-1623`) | 带快照基线 (`20261106-153753`) | 本轮无快照+指针+重排 (`20261106-175340`) | 本轮 vs 带快照基线 |
|---|---:|---:|---:|---:|
| native `render_ms` 总耗时 | `0.796 ms` | `1.067 ms` | **`0.891 ms`** | **−16.5%** |
| 可见命令收集 (`GatherVisible`) | `0.150 ms` | `0.364 ms`（含快照 `0.255 ms`） | **`0.113 ms`** | **−69.0%** |
| 帧槽数据清理 (`ResetFrameData`) | `~0.002 ms` | `0.057 ms`（含清理 `0.055 ms`） | **`0.002 ms`** | **−96.8%** |
| **`RadRay.MeshDraw.Submit`** | **`0.283 ms`** | **`0.118 ms`** | **`0.261 ms`** | **+122% (+0.143 ms)** |
| `RDG.Compile` | — | `0.00475 ms` | `0.00475 ms` | 持平 |
| native GPU 耗时 (`gpu_ms`) | `0.190 ms` | `0.188 ms` | `0.185 ms` | −1.6% |

数据反映出两个核心事实：
1. **移除每帧快照的收益完全符合预期**：收集与清理两项合计省下 **`0.307 ms/帧`**（`0.364 + 0.057 = 0.421 ms` 降至 `0.113 + 0.002 = 0.115 ms`），推动整帧 native CPU 耗时下降 `16.5%`（严格按 Draw、实例数、几何块数与驻留规模分层匹配后仍稳定改善 `11%–16%`）。
2. **`MeshDraw.Submit` 从 `0.118 ms` 回升到 `0.261 ms`（增加 `0.143 ms/帧`），抵消了约 47% 的快照移除收益**：它目前占整帧 native CPU 耗时的 **29%**；且对比最初无快照基线的 `0.283 ms`，直达指针寻址 + `MeshDrawCommand` 字段顺序重排仅将 `Submit` 降低了 `0.022 ms`（`−7.8%`）。

---

### 2. 根因剖析：为什么 `MeshDraw.Submit` 变慢了 `0.143 ms`？

通过对比三轮采集的时间线、本机缓存层级（`i7-11700F`：单核 **512 KB L2**、共享 **16 MB L3**）以及 `layout-probe` 实测结构体偏移（`sizes.json`），定位到以下三层物理根因：

#### （1）上轮 `SnapshotCommands` 充当了“隐式缓存预取器 + 工作集紧缩器”，移除后冷访存成本回归 `Submit`
* 在两帧之间（约 `3.4 ms` 间隔），宿主游戏主渲染线程在同核上运行原版世界 Vulkan 录制、区块处理与 GC/JFR，会将 `Scene::CachedDrawLists[0].Commands` 从 L1/L2 中彻底冲刷掉。
* **带快照基线（`153753`）**：`SnapshotCommands` 花费 `0.255 ms` 从 `Scene` 缓存池中跨页读取 `2,714` 条命令（**在此处承担了全部冷缓存未命中代价**），并将其拷贝紧缩到帧槽的连续数组 `CachedCommands`（`2,714 × 472 B = 1.28 MB`）。从 `SnapshotCommands` 结束到 `MeshDraw.Submit` 开始仅相隔 `~0.10 ms`（中间仅执行访问 `88 KB` 的 `SortVisible` 和 `CommandBegin`），因此上轮 `Submit` 读取的是 **100% 刚写入 L2/L3 的 `1.28 MB` 热连续内存**。
* **本轮无快照（`175340`）**：`GatherVisibleHandles` 仅计算指针地址不解引用，`SortVisible` 仅重排 `32 B` 的 `VisibleMeshDrawCommand`；**`MeshDraw.Submit` 是整帧第一个解引用 `MeshDrawCommand*` 的阶段**，必须直接去 `Scene` 的大缓存池中执行冷读取。

#### （2）`472 B` 的胖 `MeshDrawCommand` 从宏观与微观两端放大了内存拉取延迟
只要非平凡的内联容器还嵌在 `MeshDrawCommand` 内部，单靠调整字段声明顺序无法改变其 **`sizeof(MeshDrawCommand) == 472 B`（跨越 7.375 条 64B Cache Line）** 的体积：

```text
previous: command_bytes=472 visible_bytes=32 vertex_offset=8  parameters_offset=184 index_count_offset=448 push_constants_offset=336
current:  command_bytes=472 visible_bytes=32 vertex_offset=56 parameters_offset=208 index_count_offset=32  push_constants_offset=368
```

1. **宏观池膨胀与跨页稀疏访问（`6.85 MB` 工作集）**：
   * `14,510` 条缓存命令 × `472 B` = **`6.85 MB`**（跨越约 `1,712` 个 4KB 页），是单核 L2（`512 KB`）的 **13.7 倍**，占共享 L3（`16 MB`）近一半。
   * 每个地形 Section 缓存最多 7 个轴向面组（平均每 Batch 约 `5.4` 条命令），但每帧视锥与朝向剔除后平均只有 1～2 个面组可见（命中率仅 `~19%`）。命令单体高达 `472 B` 导致相邻可见命令之间的平均地址跨度达 **`~2.5 KB`**，硬件空间预取器（Spatial Prefetcher）难以跨页跟进，触发大量 L2/L3 Miss 与 dTLB/STLB Miss。
2. **微观单命令跨触 5 条 Cache Line（每 Draw 拉取 `320 B`）**：
   * 在当前布局下，`SubmitMeshDrawCommands` 每处理 1 条命令仍需触碰 **5 条不同的 64B 缓存行**：
     * **CL 0 (`0..63`)**：读取 `Pipeline` (`0`)、`IndexBuffer` (`8..23`)、`IndexCount`/`FirstIndex`/`VertexOffset` (`32..43`)、`StencilReference` (`52`)、`VertexBuffers._data` (`56`)。其中 `InstanceStreamBinding` (`24..31`)、`FirstInstance` (`44..47`)、`InstanceCount` (`48..51`) 共 `16 B` 在 Release 的 `Submit` 中根本不读，却把 `VertexBuffers._size` 挤出了 CL 0。
     * **CL 1 (`64..127`)**：读取 `VertexBuffers._size` (`64`) 与 `VertexBuffers[0]` (`80..111`)。由于 `VertexBuffers` 内联了 4 个槽位（`152 B`），后 3 个空槽（`96 B`）把 `ParameterSets` 推到了偏移 `208`。
     * **CL 3 (`192..255`)**：读取 `ParameterSets._data` (`208`)、`ParameterSets._size` (`216`)、`ParameterSets[0].GroupIndex` (`232`)、`ParameterSet` (`240`)、`DynamicOffsets._data` (`248`)。
     * **CL 4 (`256..319`)**：`SameParameterBinding` 比较 `DynamicOffsets` 时读取 `ParameterSets[0].DynamicOffsets._size`（偏移 `256`）。
     * **CL 7 (`448..471`)**：`PushConstants` 为 `std::optional<MeshPushConstantCommandBinding>`（`96 B`，偏移 `368..463`）。MSVC STL 将 `std::optional<T>::_Has_value` 放在对象末尾（偏移 **`456`**），导致 `if (command.PushConstants)` 每次 Draw 都必须加载结构体最末尾的第 7 条缓存行，只为了读取一个恒为 `false` 的布尔字节。
   * `2,752 draws × 5 cache lines = 13,760` 次缓存行访问散落在 `6.85 MB` 中，直接构成了这 `0.143 ms`（约 `52 ns/draw`）的访存停顿。

#### （3）同一 `MeshBatch` 的多条 Submesh 命令在 AoS 下重复存储相同的绑定容器
* 在 [`MeshPassProcessor::BuildMeshDrawCommands`](../../modules/runtime/src/mesh_pass_processor.cpp) 中，同一个 `MeshBatch` 的所有 `Elements`（例如一个地形 Section 的 1～7 个轴向面组 Submesh）共享**完全相同**的 `VertexBuffers`、`ParameterSets` 和 `PushConstants`，仅 `IndexBuffer`、`IndexCount`、`FirstIndex`、`VertexOffset` 等绘制标量按 Element 变化。
* 但在当前 AoS 结构下，每个 Submesh Element 都会把 `152 B` 的 `VertexBuffers`、`152 B` 的 `ParameterSets` 和 `96 B` 的 `PushConstants`（合计 **`400 B` 的非平凡容器**）原样复制一份，不仅造成 `Scene` 缓存内存 5 倍以上的冗余膨胀，还使 `SubmitMeshDrawCommands` 在连续绘制同一 Batch 的多个可见 Submesh 时，被迫在每条命令上重复遍历和比对相同的 `VertexBuffers` 与 `ParameterSets`。

---

## 二、用户提出的方案：`MeshDrawCommand` SoA 绑定池拆分

### 1. 核心设计思路

将 `MeshDrawCommand` 中所有非平凡拷贝/析构的变长容器成员（`VertexBuffers`、`ParameterSets`、`PushConstants`）从 `MeshDrawCommand` 本体中剥离，放入所属绘制列表容器（`CachedPassMeshDrawList` / `DynamicMeshPassDrawListContext`）管理的独立 **SoA（Structure of Arrays）池** 中读写：
1. **`VertexBuffers` 单独放一个池**（存储顶点流绑定数组）；
2. **`ParameterSets` 单独放一个池**（存储着色器参数集与动态偏移绑定数组）；
3. **`PushConstants` 单独放一个池**（仅当命令实际使用 PushConstants 时才在池中分配条目；不使用时句柄为空，占 `0` 池空间、在 `Submit` 时 `0` 缓存行访问）；
4. **`MeshDrawCommand` 本体彻底转化为 100% 平凡可拷贝、平凡可析构（Trivially Copyable & Trivially Destructible）的紧凑定长结构**，只保留每次 Draw 必读的标量/指针热数据与指向各 SoA 池的 Handle。

---

### 2. 数据结构与内存布局设计

#### （1）SoA 绑定载荷类型与分池容器 `MeshDrawCommandStorage`

定义统一的命令与绑定池存储结构（供缓存列表 `CachedPassMeshDrawList` 与动态列表 `DynamicMeshPassDrawListContext` 共同使用，或作为底层存储组合）：

```cpp
using MeshVertexBufferBindings = InlineVector<render::VertexBufferBinding, 4>;
using MeshParameterSetBindings = InlineVector<MeshShaderParameterBinding, 2>;

struct MeshDrawCommand {
    // --- 64 Bytes (单条 Cache Line): 100% 平凡可拷贝/析构的热数据与 SoA 句柄 ---
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};  // 8 B (0..7)
    render::IndexBufferView IndexBuffer{};                       // 24 B (8..31)
    uint32_t IndexCount{0};                                      // 4 B (32..35)
    uint32_t FirstIndex{0};                                      // 4 B (36..39)
    int32_t VertexOffset{0};                                     // 4 B (40..43)
    uint8_t StencilReference{0};                                 // 1 B (44)
    bool HasDeferredInstanceStream{false};                       // 1 B (45)
    uint16_t DeferredInstanceStreamBinding{0};                   // 2 B (46..47)
    SparseSetHandle VertexBuffersHandle{};                       // 8 B (48..55)
    SparseSetHandle ParameterSetsHandle{};                       // 8 B (56..63)
    SparseSetHandle PushConstantsHandle{};                       // 8 B (64..71) - 若压入 64B 可用紧凑句柄或合入热行
    uint64_t BaseSortKey{0};                                     // 8 B
    uint32_t FirstInstance{0};                                   // 4 B
    uint32_t InstanceCount{1};                                   // 4 B
};
```

> **缓存行对齐与进一步紧缩（64B 单缓存行目标）**：
> * `SubmitMeshDrawCommands` 在 Release 录制循环中**从不读取** `BaseSortKey`、`FirstInstance`、`InstanceCount`（后两者来自 `VisibleMeshDrawCommand`）以及 `DeferredInstanceStreamBinding`。
> * 若将构建期/预检期元数据（或将三个 SoA 池句柄表示为紧凑索引/句柄）合理排布，可将 `Submit` 必读字段 100% 锁定在 **Cache Line 0（前 64 B）** 内，整条 `MeshDrawCommand` 控制在 **`64 B`～`80 B`**（相比 `472 B` 缩小 **6～7.4 倍**），且满足 `std::is_trivially_copyable_v<MeshDrawCommand>` 与 `std::is_trivially_destructible_v<MeshDrawCommand>`。

在池容器侧，按职责拆分为独立的 SoA 池：

```cpp
struct CachedPassMeshDrawList {
    SparseSet<MeshDrawCommand> Commands;
    SparseSet<MeshVertexBufferBindings> VertexBuffersPool;
    SparseSet<MeshParameterSetBindings> ParameterSetsPool;
    SparseSet<MeshPushConstantCommandBinding> PushConstantsPool;
};
```

（对于每帧线性追加、整批清空的 `DynamicMeshPassDrawListContext`，同样持有对应的 `Commands`、`VertexBuffersPool`、`ParameterSetsPool`、`PushConstantsPool`，并在 `Clear()` 时一并清空。）

#### （2）批次级 SoA 句柄共享 vs 逐命令独立句柄

在 `CachedPassMeshDrawList` 中管理 SoA 池条目生命周期时，支持两种粒度（可在实现时择一或演进）：

* **方式 A（逐命令 1:1 拥有 SoA 句柄，最简生命周期）**：
  * 每条 `MeshDrawCommand` 在 `AddCommand` 时向 `VertexBuffersPool`（非空时）、`ParameterSetsPool`（非空时）、`PushConstantsPool`（存在 PushConstants 时）分配各自句柄；
  * 当 `Scene::InvalidateBatchCachedCommands` 销毁某条命令 `CommandHandle` 时，连带销毁该命令持有的有效 `VertexBuffersHandle`、`ParameterSetsHandle` 与 `PushConstantsHandle`。
  * **特点**：命令与 SoA 条目所有权严格 1:1 绑定，支持调用方在 `AddCommand` 后单独修改某条命令的绑定；当 `ParameterSets` 或 `PushConstants` 为空时直接用空句柄 `{}` 跳过池分配。
* **方式 B（同一 `MeshBatch` 内跨 Element 共享 SoA 句柄，极致内存与缓存压缩）**：
  * 在 `MeshPassProcessor::BuildMeshDrawCommands` 中，同一个 `MeshBatch` 的所有 Submesh `Elements` 共享同一份 `VertexBuffers`、`ParameterSets` 和 `PushConstants`。
  * 若在批次构建时为整个 Batch 仅分配 **1 份** `VertexBuffersHandle` 与 **1 份** `ParameterSetsHandle`（记录在 `MeshBatch` 缓存信息中或使用轻量引用计数，在 `InvalidateBatchCachedCommands` 时统一释放），则：
    * `14,510` 条缓存命令只需在 `VertexBuffersPool` 和 `ParameterSetsPool` 中存储 **`~2,700` 份** 条目（池体积直接再缩减 **81%**！）；
    * 同一 Batch 的多个可见 Submesh 在排序后天然相邻，它们在 `SubmitMeshDrawCommands` 中持有**完全相同的 `VertexBuffersHandle` 和 `ParameterSetsHandle`**，可直接触发句柄级 $O(1)$ 状态去重短路！

---

### 3. 读写路径设计

#### （1）写入路径（`MeshPassProcessor` / `AddCommand` / `FinalizeCommand`）
1. **构建命令描述**：
   * 提供清晰的命令构建接口（保持 `BuildMeshDrawCommands` 对上层 `SolidPassProcessor` 等零侵入或最小改动）：处理器收集好共享的 `Pipeline`、`VertexBuffers`、`ParameterSets`、`PushConstants` 后，向 `MeshPassDrawListContext` 写入。
2. **SoA 分池落盘**：
   * `VertexBuffers` 写入 `VertexBuffersPool`，返回 `VertexBuffersHandle`；
   * `ParameterSets`（若非空）写入 `ParameterSetsPool`，返回 `ParameterSetsHandle`（若为空则句柄保持无效 `{}`）；
   * `PushConstants`（若存在）写入 `PushConstantsPool`，返回 `PushConstantsHandle`（若不存在则句柄保持无效 `{}`）；
   * 每个 Submesh Element 的 `IndexBuffer`、`IndexCount`、`FirstIndex`、`VertexOffset` 等标量直接写入紧凑的 `MeshDrawCommand`。

#### （2）读取与提交路径（`SubmitMeshDrawCommands`）
在 [`SubmitMeshDrawCommands`](../../modules/runtime/src/mesh_draw_command.cpp) 中，利用 SoA 布局与句柄实现快速读取和短路去重：

1. **单缓存行读取核心命令**：
   * 通过 `visible.CachedCommand` 解引用 `MeshDrawCommand`（仅 `64–80 B`），**1 次 Cache Line 读取**即可拿到 `Pipeline`、`IndexBuffer`、`IndexCount`、`FirstIndex`、`VertexOffset`、`StencilReference` 以及三个 SoA 池句柄（`VertexBuffersHandle`、`ParameterSetsHandle`、`PushConstantsHandle`）。
2. **`PushConstants` 零冷尾触碰**：
   * 直接在 Cache Line 0 内检查 `if (command.PushConstantsHandle.IsValid())`。对于不使用 PushConstants 的常规绘制（如 100% 的地形 Draw），**彻底消除原先到结构体偏移 `456`（第 7 条缓存行）读取 `std::optional::_Has_value` 的访存开销**。
3. **句柄级状态缓存短路（Handle-Level State Cache Short-Circuit）**：
   * 在 `MeshDrawCommandStateCache` 中记录当前已绑定的 `LastVertexBuffersHandle` 与 `LastParameterSetsHandle`（在 `OnPipelineChanged` 时重置）：
     * **顶点流绑定**：若 `command.VertexBuffersHandle == stateCache.LastVertexBuffersHandle`（且 `submitArgs.InstanceStream` 已在 Pass 级/PSO 切换时处理完毕），**无需去 `VertexBuffersPool` 解引用读取 `InlineVector`，也无需构建和排序 `dirtyVertexBuffers`，直接 $O(1)$ 跳过顶点缓冲绑定**！仅当句柄变化时，才从 `VertexBuffersPool.Get(command.VertexBuffersHandle)` 读取并执行槽位去重。
     * **参数集绑定**：若 `!command.ParameterSetsHandle.IsValid()` 直接跳过；若 `command.ParameterSetsHandle == stateCache.LastParameterSetsHandle`，**无需去 `ParameterSetsPool` 解引用读取 `MeshShaderParameterBinding` 和 `DynamicOffsets`，直接 $O(1)$ 跳过参数集绑定**！仅当句柄变化时，才访问 `ParameterSetsPool` 并执行 `UpdateParameterSet`。
4. **Pass 级 `submitArgs.InstanceStream` 提至外层处理**：
   * `submitArgs.InstanceStream` 在整个 `SubmitMeshDrawCommands` 调用期内恒定不变，在 PSO 绑定或首次绑定时直接写入对应的实例流槽位，不再在 `2,752` 次逐 Draw 循环内重复合并与 `std::sort`。

---

## 三、为什么该 SoA 方案能精准解决当前瓶颈（收益推演）

以 `steady_2500_3000`（常驻 `14,510` 条缓存命令、约 `2,700` 个 Resident Batches、每帧 `2,752` 次可见 Draw）定量对比：

| 维度 | 当前方案（胖 AoS `MeshDrawCommand`） | 计划方案（瘦 `MeshDrawCommand` + SoA 绑定池） | 改善机理 |
|---|---|---|---|
| `sizeof(MeshDrawCommand)` | **`472 Bytes`**（7.375 条缓存行，非平凡类型） | **`64–80 Bytes`**（1～1.25 条缓存行，100% 平凡类型） | 剥离 `VertexBuffers` (`152B`)、`ParameterSets` (`152B`)、`PushConstants` (`96B`) |
| `Commands` 主池总内存 | `14,510 × 472 B =` **`6.85 MB`**（~1,712 页） | `14,510 × 64~80 B =` **`0.93–1.16 MB`**（~230–290 页） | 主命令池缩小 **6～7.4 倍**，完整常驻 L3 Cache，相邻命令跨度从 `2.5 KB` 降至 `~350 B`，硬件预取与 TLB 命中率大幅提升 |
| `PushConstants` 访存开销 | 每次 Draw 必读偏移 `456`（第 7 条缓存行） | 无 PushConstants 时池占用 **`0 Bytes`**，每次 Draw **`0` 次额外缓存行读取** | 句柄有效性在主命令首条缓存行内直接判别 |
| 同一 Batch 多 Submesh 连续绘制时的绑定开销 | 每个 Submesh 都要跨缓存行读取各自的 `VertexBuffers` (CL 1) 与 `ParameterSets` (CL 3 + CL 4) 并逐元素比对 | 若同 Batch 共享句柄，后续 Submesh 在首条缓存行比对 Handle 相等即可 **$O(1)$ 短路跳过**，连 SoA 池都不必访问 | 2,752 次 Draw 仅来自约 913 个可见 Section（平均每 Section 3 个可见 Submesh），约 **2/3 的 Draw 可直接短路跳过 SoA 池读取** |
| 每条可见 Draw 平均触碰 Cache Line 数 | **5 条 / Draw**（共 `13,760` 条冷缓存行） | 命中句柄短路时 **1 条 / Draw**；首次访问 Batch 时 **3 条 / Draw**（平均约 **1.7 条 / Draw**） | 整帧 `Submit` 缓存行拉取总数减少 **约 65%–70%** |

---

## 四、实施步骤与验证计划

### 1. 分步实施清单
1. **第一步：定义 SoA 绑定池与紧凑 `MeshDrawCommand` 结构（`mesh_draw_command.h`）**
   - 将 `VertexBuffers`、`ParameterSets`、`PushConstants` 从 `MeshDrawCommand` 移入 SoA 存储结构（如 `MeshDrawCommandStorage` 或 `CachedPassMeshDrawList` / `DynamicMeshPassDrawListContext` 的独立池成员）；
   - 添加 `static_assert(std::is_trivially_copyable_v<MeshDrawCommand>)` 与 `static_assert(std::is_trivially_destructible_v<MeshDrawCommand>)`，并校验 `sizeof(MeshDrawCommand)`。
2. **第二步：更新命令构建、缓存失效与动态命令上下文（`mesh_pass_processor.h/.cpp`、`scene.cpp`）**
   - 在 `BuildMeshDrawCommands` / `AddCommand` / `FinalizeCommand` 中将绑定数据写入 `VertexBuffersPool`、`ParameterSetsPool`、`PushConstantsPool`，支持同 Batch 内跨 Element 复用相同的 `VertexBuffersHandle` 与 `ParameterSetsHandle`；
   - 在 `Scene::InvalidateBatchCachedCommands` 与 `Clear()` 中同步回收对应 SoA 池条目。
3. **第三步：更新 `ValidateMeshDrawCommand` 与 `SubmitMeshDrawCommands`（`mesh_draw_command.cpp`）**
   - `SubmitMeshDrawCommands` 结合命令所属的 SoA 池解析句柄；
   - 在 `MeshDrawCommandStateCache` 中加入 `VertexBuffersHandle` 与 `ParameterSetsHandle` 短路判别，并将不变的 `submitArgs.InstanceStream` 移出内层重排。
4. **第四步：同步单元测试与文档**
   - 更新 `modules/runtime/tests/` 下 `test_mesh_draw_command.cpp`、`test_mesh_pass_processor.cpp`、`test_scene.cpp` 等测试用例；
   - 运行 `radray_runtime_tests` 与 `python tools/check_docs.py` 验证通过；
   - 更新 [`docs/architecture/runtime.md`](../architecture/runtime.md) 的事实描述。

### 2. 性能回归验证标准
- 在 `Raynium` 中接入更新后的 `RadRay::runtime`，在相同 Release + Tracy 配置下采集 `steady_2500_3000` 稳态帧：
  - 验证 `RadRay.MeshDraw.Submit` 是否从 `0.261 ms` 显著回落至 `0.10–0.14 ms` 区间；
  - 验证 `Raynium.Frame.GatherVisible`（`~0.11 ms`）与 `RadRay.FrameSlot.ResetFrameData`（`~0.002 ms`）保持无快照的高速水准，推动整帧 native `render_ms` 进一步下降。
