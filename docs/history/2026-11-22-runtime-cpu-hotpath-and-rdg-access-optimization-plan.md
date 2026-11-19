> - 适用: 理解与实施 RadRay Runtime 针对高密度场景与多页几何池的 CPU 热路径优化计划（基数排序奇数趟零拷贝交换与按奇偶趟容量对齐、MeshDrawCommand 附加元素位掩码预验与含索引绑定的单元素展开快轨、几何池 O(1) 空闲容量统计、RDG Pass 资源访问小表扫描加按需帧内存槽位哈希索引及 Scene 双入口可见性收集外提）
> - 权威: 经用户要求保存的设计与实施计划，不作为当前契约；当前已实现行为见 [Rendering runtime](../architecture/runtime.md) 与 [性能测量](../guide/performance.md)
> - 锚点: `modules/runtime/include/radray/runtime/frame_slot.h`, `modules/runtime/src/frame_slot.cpp`, `modules/runtime/include/radray/runtime/mesh_draw_command.h`, `modules/runtime/src/mesh_draw_command.cpp`, `modules/runtime/src/mesh_pass_processor.cpp`, `modules/runtime/src/scene.cpp`, `modules/runtime/src/geometry_buffer_pool.cpp`, `modules/runtime/src/rdg.cpp`, `benchmarks/bench_rdg_upload/`

# Runtime 场景提交、几何池与 RDG 资源声明 CPU 热路径优化计划

## 状态、基线与范围

- 记录日期：`2026-11-22`
- 基线：阶段三 MDI 场景底座（`O1`～`O5` 与 `M1`～`M3`）落地后的 `develop` 分支状态（承接 [Runtime 与 RHI 阶段二实机接入反馈优化与阶段三 MDI 场景底座实施计划](2026-11-21-runtime-phase2-optimization-and-phase3-mdi-plan.md)）
- 状态：已完成 `modules/runtime` 源码热路径审计与边界复核，待按 TDD 与 Google Benchmark 配对测量分步实施。
- 范围：
  1. **`C1`（`MeshPassFrameDrawList::SortVisibleCommands` 奇数趟基数排序零拷贝交换与按奇偶趟容量保持）**：消除 `SortVisibleCommands()` 在活跃字节趟数为奇数（1、3、5、7 趟）时末尾将 `_scratchCommands` 全量 `std::copy_n` 回 `VisibleCommands` 的整表内存复制，仅在奇数趟下预对齐 `_scratchCommands.capacity()` 后执行 $O(1)$ 向量缓冲交换；
  2. **`C2`（`MeshDrawCommand` 附加元素位掩码构建期预验与单元素展开快轨）**：在 `MeshDrawCommandElements` 入库时一次性预验并记录 `ValidatedExtraMask`（100% 保持原 `ValidateCommandElementStructure` 的校验语义），将 `ForEachExpandedMeshDrawElement` 每帧逐命令的 $O(E)$ 位序循环校验降为 $O(1)$ 位运算比较；为单子网格命令（`ExtraElementsIndex == kInvalidBindingIndex`）与全掩码可见的多子网格命令提供直通展开快轨（在 `SubmitMeshDrawCommands` 中完整复用 `IndexBuffer` 状态缓存比较与绑定逻辑），同时保持 RDG `PrepareLayout`（`CountMeshPassIndirectDraws`）与 `Write`（`BuildMeshPassIndirectDraws`）两阶段解耦契约不变；
  3. **`C3`（`VertexBufferPool` / `IndexBufferPool` $O(1)$ 增量空闲字节统计）**：在 `GeometryPoolStorage` 中维护增量空闲元素计数 `TotalFreeElements`，使公共查询接口 `GetFreeBytes()` 从 $O(P)$ 遍历全部分页分配器收敛为 $O(1)$ 标量乘法；
  4. **`C4`（`RDG` Pass 资源访问声明与纹理视图访问核验的自适应小表扫描 + 帧内存槽位哈希索引）**：针对 `detail::RDGUserPass`，当单 Pass 独立访问资源数 $\le 16$ 时保持零额外分配的紧凑线性扫描；当单 Pass 独立访问资源数 $> 16$ 时，在所属图的 `data.MemoryResource` 上按该 Pass 实际访问规模建立仅存 `uint32_t slot` 的开放寻址哈希桶（探测时回查 `Accesses[slot]` 的完整 `(Texture, Index)`，无 32 位键位冲突且严格遵守 `FrameSlot` 跨阶段生命周期契约），将重度 Pass 下 `AddAccess` 与 `ResolveTextureView` 的访问声明查找平均复杂度降为 $O(1)$；
  5. **`C5`（`Scene` 双入口可见性收集缓存外提与槽表边界保护）**：在 `Scene::GatherVisibleMeshDrawCommands` 与 `Scene::GatherVisiblePrimitives` 中共用内层批处理逻辑，将 `GetPassCache(pass)` 与 `std::span<const PassBatchCacheSlot>` 提取至外层循环之外并保留 `batchIndex < batchSlots.size()` 边界检查，消除 `GatherVisiblePrimitives` 逐 Batch 构造单元素 `span` 重入导致的重复查表与 profiler 作用域开销。

---

## 一、问题诊断与根因分析

在万级可见命令（`10,000`～`40,000` 条 `VisibleMeshDrawCommand`）、百页级 `VertexBufferPool` / `IndexBufferPool` 以及单光栅 Pass 内包含数百次特征资源声明的 CPU 密集负载下，`modules/runtime` 存在五处可消除的 CPU 吞吐开销：

1. **基数排序在 1 趟 / 3 趟键宽下触发额外整表 `memcpy`（[`frame_slot.cpp`](../../modules/runtime/src/frame_slot.cpp)）**：
   - `MeshPassFrameDrawList::SortVisibleCommands()` 按 `differing` 位掩码跳过无差异字节。当排序键活跃差异字节为奇数个（如 1 字节或 3 字节）时，基数排序循环结束时有序结果位于 `_scratchCommands` 中；
   - 第 77 行 `if (source != VisibleCommands.data()) std::copy_n(source, VisibleCommands.size(), VisibleCommands.data());` 会对 `VisibleCommands.size() * 32` 字节（例如 20,000 条命令 = 640 KiB）额外执行一整趟内存拷贝，使 1 趟基数排序的实际内存读写翻倍为 2 趟，3 趟变为 4 趟；
   - 由于 `DynamicContext.FinalizeVisibleCommands()` 已在排序入口第 39 行完成并清空了 `_pendingVisibleCommands`，且 `DynamicContext._visibleCommands` 指向的是 `VisibleCommands` 成员容器对象本身，在确保 `_scratchCommands.capacity() >= VisibleCommands.capacity()` 后直接执行 `VisibleCommands.swap(_scratchCommands)` 即可在 $O(1)$ 时间内完成结果接管且不丢失调用方预留容量。

2. **`ForEachExpandedMeshDrawElement` 每帧重复执行 $O(E)$ 结构校验与位扫描（[`mesh_draw_command.cpp`](../../modules/runtime/src/mesh_draw_command.cpp)）**：
   - `CountMeshPassIndirectDraws`（用于 RDG `PrepareLayout` 阶段确定精确字节数）与 `BuildMeshPassIndirectDraws`（用于所有 `PrepareLayout` 完成后的 `Write` 阶段或非 RDG 直接构建）均通过 `ForEachExpandedMeshDrawElement` 展开可见命令。该函数在 Release 构建下仍对每条可见命令调用 `ValidateCommandElementStructure(command, elements)`，每次都计算 `std::popcount(command.ValidElementMask)` 并循环遍历 `elements->ExtraElements` 逐项计算 `std::countr_zero(remaining)` 校验 `ElementIndex` 严格递增；
   - 实际上，`MeshDrawCommandElements` 在 `MeshDrawCommandBindingStorage::Store` 入库后即为不可变只读数据，其内部 `ExtraElements` 的位集合与 `ElementInstances` 形状完全可以在 `Store` 时一次性预验并缓存为 `ValidatedExtraMask`；
   - 此外，当 `command.ExtraElementsIndex == kInvalidBindingIndex`（单元素命令）时，展开循环仍执行通用的 `while (mask != 0)`、`countr_zero` 与 `popcount` 前缀计数；当多元素命令的 `mask == command.ValidElementMask` 时，每个置位仍重复计算 `std::popcount(command.ValidElementMask & ((1u << bit) - 1))` 来推导已经严格递增的 `ordinal`。

3. **几何池 `GetFreeBytes()` 每次遍历全部分页（[`geometry_buffer_pool.cpp`](../../modules/runtime/src/geometry_buffer_pool.cpp)）**：
   - `FreeBytes(const GeometryPoolStorage& storage)` 遍历 `storage.Pages` 中的所有 `unique_ptr<GeometryPage>` 并读取 `page->Allocator.GetFreeCount()`。当外部宿主通过公共接口 `VertexBufferPool::GetFreeBytes()` / `IndexBufferPool::GetFreeBytes()` 周期性或逐批采集几何池容量遥测指标时，百页规模下每次查询都产生 $O(P)$ 的跨页指针访问。

4. **RDG `AddAccess` 与 `ResolveTextureView` 在单 Pass 多资源下的 $O(A)$ 线性扫描（[`rdg.cpp`](../../modules/runtime/src/rdg.cpp)）**：
   - `AddAccess(RDGData& data, uint32_t passIndex, RDGAccess access)` 在每次 `pass.ReadBuffer` / `pass.ReadTexture` 调用时，都会线性遍历 `User(data, pass).Accesses` 查找同一 `(Texture, Index)` 是否已存在；
   - 当一个光栅 Pass 导入包含上百页的 `VertexBufferPool` / `IndexBufferPool`，且同一 Pass 内的多个渲染特征各自声明池读依赖、常量缓冲与纹理时，`AddAccess` 与 `ResolveTextureView`（在核对纹理是否已在该 Pass 声明访问时）会对 `Accesses` 数组执行数千次线性扫描；
   - 同时需遵守 `FrameSlot` 的两个内存契约（[帧内分配与资源释放](../architecture/runtime.md#帧内分配与资源释放)）：（a）`ScratchResource` 仅供单个串行阶段的局部作用域工作容器使用，跨 `Build` $\rightarrow$ `Compile` $\rightarrow$ `Prepare` $\rightarrow$ `Record` 存活的 `RDGUserPass` 成员必须由图的 `data.MemoryResource` 持有；（b）`MemoryResource` 是 `128 KiB` 初始缓冲的单调线性分配器，若按全局最大资源编号 `access.Index` 建立定长数组，在包含上千个稀疏 Pass（如每个 Pass 仅访问一个高编号 `Buffer[4095]`）的图中会造成显著的帧内存膨胀。因此查找结构必须仅在单 Pass 实际访问数超过小阈值（`> 16`）时在 `data.MemoryResource` 上按该 Pass 自身访问规模按需分配。

5. **`Scene::GatherVisiblePrimitives` 逐 Batch 重入 `GatherVisibleMeshDrawCommands`（[`scene.cpp`](../../modules/runtime/src/scene.cpp)）**：
   - `Scene::GatherVisibleMeshDrawCommands` 虽已在循环外获取 `GetPassCache(pass)`，但 `Scene::GatherVisiblePrimitives` 在内层循环中为每个 `Primitive` 的每个 `MeshBatchHandle` 构造长度为 1 的 `std::span{&visible, 1}` 并重新调用 `GatherVisibleMeshDrawCommands`，导致每个 Batch 都重复执行一次 `GetPassCache(pass)`、容量检查与 `RADRAY_PROFILE_SCOPE_N` / `RADRAY_PROFILE_PLOT`。

---

## 二、具体实施方案（`C1`～`C5`）

### C1：`SortVisibleCommands` 奇数趟基数排序 $O(1)$ 缓冲交换与按奇偶趟容量保持

在 [`modules/runtime/include/radray/runtime/frame_slot.h`](../../modules/runtime/include/radray/runtime/frame_slot.h) 与 [`modules/runtime/src/frame_slot.cpp`](../../modules/runtime/src/frame_slot.cpp) 的 `MeshPassFrameDrawList::SortVisibleCommands()` 中：

1. **仅在奇数个活跃差异字节时预对齐 `_scratchCommands` 容量并执行 $O(1)$ 交换**：
   - 当 `VisibleCommands.size() > 64` 且 `!sorted` 时，先由已算出的 `differing` 统计活跃字节趟数：
     ```cpp
     uint32_t activePasses = 0;
     for (uint32_t shift = 0; shift < 64; shift += 8) {
         if (((differing >> shift) & 255) != 0) ++activePasses;
     }
     if ((activePasses & 1u) != 0 && _scratchCommands.capacity() < VisibleCommands.capacity()) {
         _scratchCommands.reserve(VisibleCommands.capacity());
     }
     _scratchCommands.resize(VisibleCommands.size());
     ```
     - 若为**偶数趟**（`(activePasses & 1u) == 0`），最终有序结果直接落回 `VisibleCommands`，不会发生缓冲交换，因此 `_scratchCommands` 仅按当帧实际 `VisibleCommands.size()` 分配（避免在“大预留、小输入、偶数趟”场景下为 `_scratchCommands` 多复制一份大预留内存预算）；
     - 若为**奇数趟**（`(activePasses & 1u) != 0`），最终将交换两个 vector 的底层缓冲区，因此预先使 `_scratchCommands.capacity() >= VisibleCommands.capacity()`，确保交换后 `VisibleCommands` 不丢失调用方预留容量；
   - 基数排序循环结束后，将：
     ```cpp
     if (source != VisibleCommands.data()) std::copy_n(source, VisibleCommands.size(), VisibleCommands.data());
     ```
     替换为：
     ```cpp
     if (source != VisibleCommands.data()) VisibleCommands.swap(_scratchCommands);
     ```
2. **契约与不变量**：
   - `DynamicContext.FinalizeVisibleCommands()` 已在排序入口将所有动态命令的 `CachedCommand` 指针固定到 `DynamicContext.Commands` 元素地址上，与 `VisibleCommands` 缓冲首地址无关；
   - `DynamicContext._visibleCommands` 保存的是 `&VisibleCommands` 容器指针，交换后依然指向 `this->VisibleCommands`；
   - 调用方在调用 `SortVisibleCommands()` 后再读取 `VisibleCommands` 的 `std::span` 视图（排序前取得的底层指针/span 视图在排序后失效应重新获取）。

---

### C2：`MeshDrawCommand` 附加元素位掩码 $O(1)$ 预验与含索引绑定的单元素展开快轨

在 [`modules/runtime/include/radray/runtime/mesh_draw_command.h`](../../modules/runtime/include/radray/runtime/mesh_draw_command.h) 与 [`modules/runtime/src/mesh_draw_command.cpp`](../../modules/runtime/src/mesh_draw_command.cpp) 中（严格保持 RDG `PrepareLayout` 期 `CountMeshPassIndirectDraws` 与 `Write` 期 `BuildMeshPassIndirectDraws` 的两阶段顺序无关契约）：

1. **`MeshDrawCommandElements` 入库预验 `ValidatedExtraMask`（100% 兼容既有结构校验语义）**：
   - 现有 `ValidateCommandElementStructure` 的本质是校验：（a）`command.ValidElementMask != 0`；（b）`elements->ElementInstances` 为空或大小等于 `ExtraElements.size() + 1`；（c）`ExtraElements` 中各项的 `ElementIndex` 严格递增（`< 32`），且其构成的位集合恰好等于 `command.ValidElementMask` 去掉最低有效位后的掩码 `command.ValidElementMask & (command.ValidElementMask - 1)`（允许命令副本改变不越界的主元素最低置位，同时拒绝任何与 `ExtraElements` 位集合不匹配的掩码）；
   - 在 `MeshDrawCommandElements` 中新增预验字段：
     ```cpp
     uint32_t ValidatedExtraMask{0};
     bool StructureValid{false};
     ```
   - 在 `MeshDrawCommandBindingStorage::Store` 写入 `MeshDrawCommandElements` 时一次性完成预验（直接覆盖 `ExtraElements` 为空但 `ElementInstances` 含 1 项主元素实例的合法输入）：
     ```cpp
     uint32_t extraMask = 0;
     bool structureValid = bindings.ExtraElements.size() < 32 &&
                           (bindings.ElementInstances.empty() ||
                            bindings.ElementInstances.size() == bindings.ExtraElements.size() + 1);
     uint32_t prevIndex = 0;
     for (size_t i = 0; i < bindings.ExtraElements.size() && structureValid; ++i) {
         const uint32_t idx = bindings.ExtraElements[i].ElementIndex;
         if (idx == 0 || idx >= 32 || (i > 0 && idx <= prevIndex)) {
             structureValid = false;
             break;
         }
         prevIndex = idx;
         extraMask |= (uint32_t{1} << idx);
     }
     ```
   - 将 `ValidateCommandElementStructure(const MeshDrawCommand& command, Nullable<const MeshDrawCommandElements*> elements) noexcept` 收敛为 $O(1)$ 位运算比较：
     ```cpp
     bool ValidateCommandElementStructure(const MeshDrawCommand& command, Nullable<const MeshDrawCommandElements*> elements) noexcept {
         if (command.ValidElementMask == 0) return false;
         const uint32_t extraMask = command.ValidElementMask & (command.ValidElementMask - 1);
         if (command.ExtraElementsIndex == kInvalidBindingIndex) {
             return !elements && extraMask == 0;
         }
         return elements && elements->StructureValid && extraMask == elements->ValidatedExtraMask;
     }
     ```
   - 该 $O(1)$ 检查（仅 1 次 `v & (v - 1)` 与 1 次 `==` 比较）与原 $O(E)$ 循环在所有输入下（包括 `ExtraElements` 为空但 `ElementInstances` 含 1 项、修改命令副本的主元素最低置位、篡改 `ValidElementMask` 或缺失 `ExtraElements`）**数学上 100% 等价**。

2. **`ForEachExpandedMeshDrawElement` 与 `SubmitMeshDrawCommands` 单元素快轨与线性 `ordinal`**：
   - **单元素快轨（`!elements`）**：当 `ValidateCommandElementStructure` 通过且 `!elements`（即 `ExtraElementsIndex == kInvalidBindingIndex`，既无 `ExtraElements` 亦无 `ElementInstances`）时，`command.ValidElementMask` 必为单比特，且 `mask = visible.ElementMask & command.ValidElementMask` 非零时只有 `ordinal == 0` 的主元素：
     - 在 `ForEachExpandedMeshDrawElement` 中：若 `visible.InstanceCount == 0` 则跳过；否则直接以 `MeshDrawElement{command.IndexBuffer, command.IndexCount, command.FirstIndex, command.VertexOffset, static_cast<uint32_t>(std::countr_zero(mask))}` 调用一次 `emit(...)`，跳过内层 `while (mask != 0)` 循环、`popcount` 与 `ContiguousMergeMask` 分支；
     - 在 `SubmitMeshDrawCommands` 中：因为 `BindMeshDrawCommandState` 仅绑定 PSO、VertexBuffers、StencilReference 与参数集/PushConstants，**不绑定 `IndexBuffer`**，单元素快轨在调用 `DrawIndexed` 前**必须完整复用 `stateCache.IndexBuffer` 比较与绑定逻辑**：
       ```cpp
       if (!elements) {
           if (visible.InstanceCount == 0) continue;
           if (command.IndexBuffer.Target != nullptr) {
               if (!stateCache.IndexBuffer || !SameIndexBufferView(*stateCache.IndexBuffer, command.IndexBuffer)) {
                   encoder.BindIndexBuffer(command.IndexBuffer);
                   stateCache.IndexBuffer = command.IndexBuffer;
               }
               encoder.DrawIndexed(command.IndexCount, visible.InstanceCount, command.FirstIndex, command.VertexOffset, visible.FirstInstance);
           } else {
               encoder.Draw(command.IndexCount, visible.InstanceCount, command.FirstIndex, visible.FirstInstance);
           }
           continue;
       }
       ```
   - **多元素全掩码可见线性 `ordinal`**：当 `elements != nullptr` 且 `mask == command.ValidElementMask` 时，令 `uint32_t currentOrdinal = 0;`，每次取最低置位后 `const auto ordinal = currentOrdinal++;`（在内层 `ContiguousMergeMask` 合并连续元素时同步 `++currentOrdinal`），省去每次迭代的 `std::popcount(command.ValidElementMask & ((uint32_t{1} << bit) - 1))`。

---

### C3：`VertexBufferPool` / `IndexBufferPool` $O(1)$ 增量空闲容量统计

在 [`modules/runtime/src/geometry_buffer_pool.cpp`](../../modules/runtime/src/geometry_buffer_pool.cpp) 中：

1. 在 `GeometryPoolStorage` 中新增成员变量：
   ```cpp
   uint64_t TotalFreeElements{0};
   ```
2. 在分页分配与子分配释放路径维护 `TotalFreeElements`：
   - `AllocateRange` 在已有分页命中 `storage.Pages[index]->Allocator.Allocate(count)` 时：`storage.TotalFreeElements -= count;`；
   - `AllocateRange` 新建分页并完成首笔 `page->Allocator.Allocate(count)`、`storage.Pages.push_back(std::move(page))` 时：`storage.TotalFreeElements += (capacity - count);`；
   - `VertexBufferSubAllocation::Reset()` 与 `IndexBufferSubAllocation::Reset()` 在 `_pool` 非空时：
     ```cpp
     _pool->_impl->Pages[_page]->Allocator.Destroy(_allocation);
     _pool->_impl->TotalFreeElements += _allocation.Count;
     _pool = nullptr;
     ```
3. `FreeBytes(const GeometryPoolStorage& storage) noexcept` 直接返回 `storage.TotalFreeElements * storage.Stride`；在 `#if RADRAY_IS_DEBUG` 下保留遍历累加断言 `RADRAY_ASSERT(bytes == storage.TotalFreeElements * storage.Stride)`。

---

### C4：`RDG` Pass 资源访问查重与纹理视图访问核验的自适应小表扫描 + 帧内存槽位哈希索引

在 [`modules/runtime/src/rdg.cpp`](../../modules/runtime/src/rdg.cpp) 中，兼顾“大量稀疏小 Pass（如 1,024 个单资源 Copy Pass）”与“单个重度 Pass（绑定上百页几何池与数百次绘制访问）”两类极端负载，且严格遵守 `FrameSlot` 跨阶段容器生命周期契约：

1. **阈值分流与纯 `slot` 哈希桶设计（`kLinearAccessThreshold = 16`）**：
   - 在 `detail::RDGUserPass` 中新增绑定 `&data.MemoryResource` 的开放寻址桶数组（与 `Accesses` 生命周期一致，跨 `Build` $\rightarrow$ `Compile` $\rightarrow$ `Prepare` $\rightarrow$ `Record` 有效，不借用阶段局部的 `ScratchResource`）：
     ```cpp
     static constexpr uint32_t kInvalidAccessSlot = std::numeric_limits<uint32_t>::max();
     std::pmr::vector<uint32_t> AccessBuckets;
     ```
   - 当 `User(data, pass).Accesses.size() <= 16` 时：**`AccessBuckets` 保持为空（`capacity() == 0`，0 字节堆/arena 分配）**，直接对 `Accesses`（当前 x64 布局下 `sizeof(RDGAccess) == 16`，16 项占 `256 B` = 4 条缓存行）执行线性扫描。所有小型 Pass 以及仅访问高编号资源（如 `Buffer[4095]`）的稀疏 Pass 均不分配任何桶数组；
   - 当某个 Pass 的独立访问资源数增长超过 `16` 时，在 `data.MemoryResource` 上按该 Pass 自身 `Accesses.size()` 规模初始化 2 的幂长度的 `AccessBuckets`（初始 64 桶 × 4 B = `256 B`，装载率 $\le 1/2$）；
   - **纯 `slot` 桶（避免 32 位键位冲突）**：每个桶仅存储 `uint32_t slot`（指向 `Accesses[slot]` 的下标，空桶为 `kInvalidAccessSlot`）。哈希函数由 `(bool texture, uint32_t index)` 计算桶起点，**探测命中非空桶时直接回查 `Accesses[slot].Texture == texture && Accesses[slot].Index == index` 比较完整的 `(bool, uint32_t)` 对**，无需将 `(Texture, Index)` 压缩编码进 32 位整数，从根本上杜绝 `Buffer[0x80000000]` 与 `Texture[0]` 的最高位碰撞，同时将单桶体积减半至 4 字节。
2. **`AddAccess` 与 `ResolveTextureView` 执行路径**：
   - **`AddAccess`（查找与状态合并解耦，100% 复用既有合并逻辑）**：
     - 线性扫描（`AccessBuckets.empty()`）与哈希桶探测（`!AccessBuckets.empty()`）**仅负责查找目标 `(access.Texture, access.Index)` 在 `Accesses` 中的既有槽位 `Nullable<RDGAccess*> existing`**；
     - **一旦命中（`existing != nullptr`），两条路径完全共用现有的状态与标志合并代码（[`rdg.cpp:346-361`](../../modules/runtime/src/rdg.cpp)）**：
       - 若 `existing->State != access.State`：尝试只读 Buffer 状态位或合并（`access.State |= existing->State` 并校验 `ValidAccess`），不兼容则 `Fail(data, "conflicting whole-resource states in a pass")`；
       - 若 `existing->State == access.State`：**原样执行 `existing->Read |= access.Read; existing->Write |= access.Write; existing->Preserve |= access.Preserve; return;`**（确保同一 Pass 先读后写同一 UAV 等同状态读写合并后，`Write` 与 `Preserve` 标志及后续写者依赖追踪 100% 完整保留）；
     - **若未命中（`existing == nullptr`）**：执行 `Accesses.push_back(access)`；若此前 `AccessBuckets.empty()` 且新 `Accesses.size() > 16`，则分配 64 桶的 `AccessBuckets` 并将全部 17 项散列填入；若此前 `!AccessBuckets.empty()`，则将新槽位写入探测到的空桶（当 `Accesses.size() * 2 > AccessBuckets.size()` 时将 `AccessBuckets` 翻倍并按 `Accesses` 重新散列）；
   - **`ResolveTextureView`**：
     - 当 `execution.Pass` 非空且 `!IsUpload(*execution.Pass)` 时，核验纹理 `handle` 是否已在当前 Pass 声明兼容访问：若 `!userPass.AccessBuckets.empty()`，直接以 `(true, handle.Index)` 探测 `AccessBuckets`（平均 $O(1)$）定位 `Accesses[slot]` 校验 `state` 与 `usage`；若 `AccessBuckets.empty()`（$\le 16$ 项）则扫描短 `Accesses` 数组（通过访问合法性校验后，`execution.Views` 的帧内视图复用查找仍按原有逻辑执行）。

---

### C5：`Scene` 双入口可见性收集（`GatherVisibleMeshDrawCommands` 与 `GatherVisiblePrimitives`）缓存外提、空缓存兼容与槽表边界保护

在 [`modules/runtime/src/scene.cpp`](../../modules/runtime/src/scene.cpp) 中：

1. **提取单 Batch 内层转换辅助，严格保留“先验句柄与脏标记、再判空缓存/槽表边界”的校验顺序**：
   - 未注册 Pass 的 `GetPassCache(pass)` 返回 `nullptr`；已注册 Pass 中被处理器过滤的最高编号存活 Batch 其 `batchIndex` 可以 `>= cache->_batchSlots.size()`；
   - 外提缓存时，当 `cache == nullptr` 时生成空 `std::span`：
     ```cpp
     const auto cache = GetPassCache(pass);
     const auto sortMode = cache ? cache->GetSortMode() : MeshPassSortMode::StateFirst;
     const std::span<const PassBatchCacheSlot> batchSlots =
         cache ? std::span<const PassBatchCacheSlot>{cache->_batchSlots} : std::span<const PassBatchCacheSlot>{};
     const std::span<const MeshDrawCommand> commands =
         cache ? std::span<const MeshDrawCommand>{cache->_commands} : std::span<const MeshDrawCommand>{};
     ```
   - 在内层单 Batch 转换辅助函数中，**严格先检查句柄存活与脏标记（失败时将 `output.resize(initialSize)` 回滚并返回 `false`），再检查 `batchIndex >= batchSlots.size()`（统一覆盖 `cache == nullptr` 未注册 Pass 与被处理器过滤的尾部存活 Batch，正常跳过并返回 `true`）**：
     ```cpp
     const uint32_t batchIndex = visible.Batch.GetIndex();
     if (!_meshes.IsAlive(visible.Batch._handle) || _batchDirty[batchIndex]) {
         output.resize(initialSize);
         return false;
     }
     if (batchIndex >= batchSlots.size()) return true; // 未注册 Pass (size==0) 或该 Batch 未在此 Pass 生成命令
     const auto& slot = batchSlots[batchIndex];
     if (slot.CommandDenseIndex == kInvalidBindingIndex) return true;
     const auto& command = commands[slot.CommandDenseIndex];
     const auto mask = visible.ElementMask & command.ValidElementMask;
     if (!mask) return true;
     output.push_back({.SortKey = ComposeMeshDrawSortKey(sortMode, slot.StateSortKey, visible.VisibilitySortKey),
                       .CachedCommand = &command,
                       .FirstInstance = visible.FirstInstanceOverride.value_or(slot.FirstInstance),
                       .InstanceCount = visible.InstanceCountOverride.value_or(slot.InstanceCount),
                       .ElementMask = mask,
                       .UseElementFirstInstance = !visible.FirstInstanceOverride.has_value(),
                       .UseElementInstanceCount = !visible.InstanceCountOverride.has_value()});
     return true;
     ```
   - `Scene::GatherVisibleMeshDrawCommands` 与 `Scene::GatherVisiblePrimitives` 均在最外层循环前仅调用一次 `GetPassCache(pass)`，内层直接调用该辅助函数，既消除 `GatherVisiblePrimitives` 逐 Batch 构造单元素 `std::span` 重入的开销，又 100% 保持未注册 Pass、脏 Batch 拒绝回滚与尾部过滤 Batch 的既有语义。
2. **状态排序与合批边界**：
   - 自定义确定性状态键（`MeshDrawCommandSharedState::CustomStateSortKey`）仅在 `MeshPassSortMode::StateFirst` 下充当 64 位 `SortKey` 的高 32 位以消除 8 位 `geometryBucket` 哈希碰撞并聚拢同页命令（且需 PSO、StencilReference、绑定存储与实际 IB 视图兼容方可由 `BuildMeshPassIndirectDraws` 合并）；在 `MeshPassSortMode::DepthFirst` 下高 32 位为 `VisibilitySortKey`，严格服从调用方给定的深度顺序。

---

## 三、测试、性能基准与验收标准

### 3.1 正确性单元测试（`modules/runtime/tests/`）

1. **`test_frame_slot.cpp`**：
   - 验证 `SortVisibleCommands` 在奇数趟（1、3、5 字节差异）与偶数趟（2、4 字节差异）下排序结果的稳定性与动态命令指针有效性；
   - 验证调用方预先 `VisibleCommands.reserve(40000)`、首帧以 `10,000` 项触发奇数趟 `swap` 后，`VisibleCommands.capacity()` 仍保持 `>= 40000`，次帧追加至 `20,000` 项零重分配；
   - 验证调用方预先 `VisibleCommands.reserve(40000)`、实际仅输入 `65` 项且触发**偶数趟**（2 个活跃字节）排序时，`_scratchCommands.capacity()` 仅按 `65` 项分配而不复制 `40,000` 项的预留容量预算。
2. **`test_mesh_draw_command.cpp` & `test_mdi_gpu.cpp`**：
   - 验证 `SubmitMeshDrawCommands` 单元素快轨的索引缓冲绑定：连续提交三条单元素有索引命令分别访问 `IB_A`、`IB_B`、`IB_B`，断言 `encoder` 精确记录两次 `BindIndexBuffer`（顺序为 `IB_A`、`IB_B`）与三次 `DrawIndexed`；
   - 验证 `ValidatedExtraMask` 在单元素、`ValidElementMask = 1` 且 `ExtraElements` 为空但 `ElementInstances` 含 1 项（验证 `Count` 与 `Build` 生成的参数确实采用了 `ElementInstances[0]` 的实例范围）、多元素全掩码、多元素子集掩码、命令副本修改主元素最低置位（如 `0b0101 -> 0b0110` 且附加元素仍在位 2）时均通过，而在篡改 `ValidElementMask` 或缺失 `ExtraElements` 时精确返回 `false`；
   - 保持 `test_mdi_gpu.cpp` 中 `CompactedInstancePrepareLayoutAndWriteMatchesDirectUnderBothRegistrationOrders`（正反两种生成器注册顺序下 `PrepareLayout` 计数 + 实例紧凑重映射 + `Write` 构建 MDI 参数的像素级一致性）100% 通过。
3. **`test_geometry_upload_gpu.cpp`**：
   - 验证多页分配、跨页释放与重分配过程中 `GetFreeBytes()` $O(1)$ 计数与逐页累加值严格相等。
4. **`test_rdg.cpp`**：
   - 在槽位查找辅助单测中直接构造含 `(false, 0x80000000u)` 与 `(true, 0u)` 的合成 `RDGAccess` 记录，验证纯 `slot` 桶回查下最高位无任何别名混淆；
   - 在 RDG 集成测试中：
     - 使用真实创建的 Buffer 与 Texture 越过 `16 -> 17` 项建桶阈值及 `32 -> 33` 项首次扩桶重哈希边界（`64 -> 128` 桶），重新查询并解析扩桶前登记的资源；
     - 验证已启用 `AccessBuckets`（`> 16` 项访问）的 Compute Pass 对同一已初始化 UAV Buffer **先声明 `ReadBuffer(uav, BufferState::UnorderedAccess)` 再声明 `WriteBuffer(uav, BufferState::UnorderedAccess)`**，断言同状态读写标志合并后该 Pass 被正确登记为最后写者、不被死代码裁剪且后继消费者 Pass 保持正确依赖顺序；
     - 验证 1,024 个稀疏 `AddCopyPass` 用户 Pass 各读一个高编号 Buffer 时保持 `AccessBuckets.empty()` 零桶分配。
5. **`test_scene.cpp`**：
   - 同时验证 `GatherVisibleMeshDrawCommands`（批量入口）与 `GatherVisiblePrimitives`（逐 Primitive 多 Batch 入口）：覆盖（a）未注册 Pass（`cache == nullptr`）下干净有效 Batch 返回 `true` 且输出 0 条，但混入失效句柄或尚未同步的新脏 Batch 时仍返回 `false` 并回滚 `output`；（b）已注册 Pass 下最高编号的最后一个存活 Batch 被处理器过滤（`batchIndex >= _batchSlots.size()`）时安全跳过并完整保留前面已收集的命令。

### 3.2 Release 性能配对基准（遵循 [性能测量](../guide/performance.md)）

在 Release 配置（`RADRAY_BUILD_BENCHMARKS=ON`，`RADRAY_ENABLE_PROFILER=OFF`，固定进程 affinity，改前/改后交替运行）下，扩展并运行 `bench_rdg_upload` 执行配对测量，报告中位数、CV 与 `--rdg_memory_probe` 内存指标：

1. **`C1` 排序基准**：测量 `20,000` 条 `VisibleMeshDrawCommand` 在 0 趟（已预排序）、1 趟（单字节页键）、3 趟（状态+页+深度桶）与 4 趟差异键下的 `SortVisibleCommands()` 耗时（对非 0 趟用例在每轮迭代计时外通过 `PauseTiming()`/`ResumeTiming()` 恢复同一份乱序输入，确保每轮均实测基数排序而非第二轮起短路返回），确认奇数趟（1 趟、3 趟）消除末尾 `std::copy_n` 后的净耗时下降；
2. **`C2` 展开与提交基准**：测量 `20,000` 条命令（分单元素命令与 7 元素命令）执行 `CountMeshPassIndirectDraws` + `BuildMeshPassIndirectDraws` 以及 `SubmitMeshDrawCommands` 的耗时；
3. **`C4` 密集单 Pass 与稀疏多用户 Pass 构图基准**：
   - 密集单 Pass：单光栅 Pass 导入 100 页 Buffer 并重复声明 200 次特征读访问 + 纹理视图解析，对比 `Build` / `Prepare` 耗时；
   - 稀疏多用户 Pass：测量创建 1,024 个 `AddCopyPass` 用户 Pass（如 `Intermediate` 模式或 1,024 个 Copy Pass 各读一个高编号 Buffer，含 `--rdg_memory_probe`），对比构图耗时、`peak_storage_bytes` 与 `upstream_allocations`；
4. **`C3` 与 `C5` 基准**：测量百页几何池下 `GetFreeBytes()` 查询耗时与分配/释放计数开销，以及 `GatherVisibleMeshDrawCommands` 与 `GatherVisiblePrimitives` 在 `10,000` 个 Batch 下的收集耗时。
5. **全量回归与文档检查**：`ctest --output-on-failure`、`python tools/check_docs.py` 与 `git diff --check` 零错误。
