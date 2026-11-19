> - 适用: 理解与实施 RadRay Runtime 针对几何池空页回收、MDI 批次构建代表命令签名缓存评估与瞬态参数集有序输入零拷贝快轨的修订计划
> - 权威: 经用户要求保存的设计与实施计划，不作为当前契约；当前已实现行为见 [Rendering runtime](../architecture/runtime.md) 与 [性能测量](../guide/performance.md)
> - 锚点: `docs/architecture/runtime.md`, `docs/guide/cpp-conventions.md`, `modules/runtime/include/radray/runtime/geometry_buffer_pool.h`, `modules/runtime/src/geometry_buffer_pool.cpp`, `modules/runtime/src/gpu_resource.cpp`, `modules/runtime/src/rdg.cpp`, `modules/runtime/src/render_graph_pipeline.cpp`, `modules/runtime/include/radray/runtime/mesh_batch.h`, `modules/runtime/include/radray/runtime/mesh_draw_command.h`, `modules/runtime/src/mesh_draw_command.cpp`, `modules/runtime/include/radray/runtime/transient_parameter_pool.h`, `modules/runtime/src/transient_parameter_pool.cpp`, `benchmarks/bench_rdg_upload/bench_rdg_upload.cpp`, `benchmarks/bench_transient_parameter_pool/bench_transient_parameter_pool.cpp`

# Runtime 几何池空页回收、MDI 批次构建缓存评估与瞬态参数集快轨计划

## 状态、基线与范围

- 记录日期：`2026-11-22`（`2026-11-23` 对照 `af121f2` 完成四轮审查收敛：固定 RDG 构图前后建页/重提交/`Trim` 时序契约、稀疏页槽与移动/赋空句柄语义、`GetVertexInput` 与 `GpuBuffer` 寿命边界、`Nullable<unique_ptr<GeometryPage>>` 工厂签名、析构全归还断言、`R3a` 首次入池与全链路 `activeBindings` 读写及 `R3b` 对照组跨帧重置与双向门禁）
- 基线：`develop` 分支 `af121f2`（已完成 `C1`～`C5` Runtime CPU 热路径优化，承接 [Runtime 场景提交、几何池与 RDG 资源声明 CPU 热路径优化计划](2026-11-22-runtime-cpu-hotpath-and-rdg-access-optimization-plan.md)）
- 状态：已完成基于 `af121f2` 源码的审查收敛定稿，可直接进入按 TDD 与 Google Benchmark 配对测量的分步实施，并在实施后同步 `docs/architecture/runtime.md`。
- 范围：
  1. **`R1`（`VertexBufferPool` / `IndexBufferPool` 稀疏页槽号直读与基于 `unique_ptr<GeometryPage>` 的空页撤销提交）**：
     - 在 `VertexBufferSubAllocation` 与 `IndexBufferSubAllocation` 上公开 `uint32_t GetPageIndex() const noexcept`（按 `_pool ? _page : 0u` 实现，覆盖默认构造、移动赋入空句柄触发私有 `Reset()` 释放后及被移动后的源句柄）；在两池上公开 `GetCapacityBytes()`、`GetAllocatedBytes()` 与 `TrimEmptyPages()`，明确 `GetPageCount()` 返回当前已提交页面数且不可用作 `GetPageIndex()` 上界；
     - **完整对象不变量、引用寿命边界与失败安全**：不存在半销毁 `GeometryPage` 对象；撤销提交空页直接 `Pages[index].reset()`（立即销毁该页 `GeometryPage` 及其内嵌 `VertexInput`；外部 `GetGpuBuffer()` 仅延长底层 `GpuBuffer` 寿命，不保活 `GetVertexInput()` 引用）并弹出尾部连续空槽；统一建页工厂按仓库约定返回 `Nullable<unique_ptr<GeometryPage>>`，保留 `Allocate(0)` 入口早退与 `GpuBuffer::Create` 判空早退，析构时继续独立断言每张已提交页 `GetFreeCount() == GetCapacity()`（全部子分配已归还）；
     - **保持 `AddPassReadAccess` 已提交页覆盖与严格构图时序契约**：`AddPassReadAccess` 跳过 `nullptr` 空槽，按当时的底层 `GpuBuffer` 身份导入全部非空已提交页（含保留空页）；`ExtractMeshPass` 等构图后晚分配**仅限从 `AddPassReadAccess` 调用时已导入的同一个 `GpuBuffer` 页面切分子区间**——任何影响本帧 Pass 读取的新建页面、空槽重提交（即使槽号相同但底层已换为新 `GpuBuffer`）以及 `TrimEmptyPages()` 调用，均必须在 `AddPassReadAccess` 之前（如 `ExtractFrameData`）完成。
  2. **`R2`（评估项：`BuildMeshPassIndirectDraws` 活跃批次合并签名局部标量缓存；保留既有提交阶段双槽缓存与跨池无槽位合批语义）**：
     - 保持 `MeshDrawCommandStateCache` 现有的 `(LastVertexStorage, LastVertexBuffersIndex)` 与 `(LastPayloadStorage, LastPayloadIndex)` 独立缓存不动；
     - 仅在 `BuildMeshPassIndirectDraws` 内用局部标量缓存当前末尾批次签名，逐字保留原 `sameVertices` / `samePayload` 在双侧槽号均为 `kInvalidBindingIndex` 时忽略 `BindingStorage` 差异的跨池合批谓词，并以高合并率有稳定正收益且高频断批无退化作为合入门禁。
  3. **`R3`（`TransientParameterSetPool` 有序输入零拷贝 + 单次 `_groups` 查表快轨 `R3a`，以及带正负双向门禁的 1-entry MRU 评估项 `R3b`）**：
     - **`R3a`（直接推进）**：入口原地单趟校验 `bindings` 严格递增与无重，有序输入直接以 `activeBindings` 零拷贝借用而不触碰 `_scratch`；**哈希计算、普通桶比较、`Set` 写入、`FilledEntry.Count` 与 `KeyBindings.insert` 全部统一读取 `activeBindings`**（杜绝乱序输入残留的 `_scratch` 串键污染）；新建与复用集合统一先确保 `group.Sets` 持有 owner 再经 `group.Sets[group.Used++].get()` 取出，写失败时严格先 `set->ResetBindingsForReuse()` 再 `--group.Used`；
     - **`R3b`（基准门禁评估）**：将 `kEmptyBucket` 提升为 `TransientParameterSetPool` 私有 `static constexpr` 常量（头文件自包含 `#include <limits>`），`Group` 增加 `LastEntryIndex{kEmptyBucket}`；先定位 `group` 查 MRU，未命中再算哈希，桶命中与插入成功均写回 `LastEntryIndex`；以“连续同键有稳定正收益，且 A/B 末位差异交替与持续新键冷插入均无明显退化”作为合入判据。

---

## 一、问题诊断与架构边界厘清

结合 `af121f2` 源码审计与下游实机采样，当前 `modules/runtime` 的真实改进点与必须严守的契约边界如下：

### 1.1 `GeometryBufferPool` 空页滞留、稀疏页槽契约及引用寿命边界（[`geometry_buffer_pool.cpp`](../../modules/runtime/src/geometry_buffer_pool.cpp)）

1. **流式高峰后的空页滞留与页槽号暴露缺失**：
   - 当前 `GeometryPoolStorage::Pages` 一旦在流式加载高峰期分配了多个分页，即使后续旧几何体释放使整页完全空闲（`GetFreeCount() == GetCapacity()`），池侧也无法释放该页的 `IntrusivePtr<GpuBuffer>` 引用。
   - 同时，`VertexBufferSubAllocation` 与 `IndexBufferSubAllocation` 内部虽已持有 `uint32_t _page`，但未提供只读访问器，导致下游不得不额外维护 `unordered_map<gpu::Buffer*, uint32_t>` 来查询所属页号。
2. **`TrimEmptyPages()` 的职责边界、稀疏页号与 `VertexInput` 寿命边界**：
   - **能解决与不能单独解决的问题**：`TrimEmptyPages()` 仅撤销整张空页的提交并释放池持有的 `IntrusivePtr<GpuBuffer>` 引用，不搬移在用子分配；若 `Page 0` 与 `Page 1` 各自仍有存活几何体且 `DepthFirst` 绘制跨页交替，`TrimEmptyPages()` 返回 `0`，批次数不变（消除活跃期跨页断批依赖下游池隔离与页容量规划）。
   - **稀疏页槽号 vs 已提交页数**：中间空页（如 `0, 1, 2` 三页中释放 `1`）撤销提交后保留 `Pages[1] == nullptr` 空槽以维持页 `2` 存活句柄的 `_page == 2` 不变。此时 `GetPageCount()` 返回 `2`，而页 `2` 句柄的 `GetPageIndex()` 仍为 `2`，因此 **`GetPageCount()` 绝不能用作 `GetPageIndex()` 的上界**。同时，子分配移动构造/移动赋值仅将源对象的 `_pool` 置空而未清零 `other._page`（[`geometry_buffer_pool.cpp`](../../modules/runtime/src/geometry_buffer_pool.cpp) 第 168、199 行），故 `GetPageIndex()` 必须通过 `_pool ? _page : 0u` 判空以确保被移动后的空源句柄返回 `0`。
   - **`GetVertexInput()` 引用与 `GetGpuBuffer()` 强引用的寿命区分**：[`geometry_buffer_pool.cpp`](../../modules/runtime/src/geometry_buffer_pool.cpp) 第 187 行的 `GetVertexInput()` 返回 `GeometryPage::Input` 的引用，而 [`mesh_batch.h`](../../modules/runtime/include/radray/runtime/mesh_batch.h) 第 39 行的 `MeshBatch::VertexInput` 可借用该指针。只要页上仍有至少一个活跃 `VertexBufferSubAllocation`，该页就不会被 `TrimEmptyPages()` 回收，`GetVertexInput()` 地址恒定有效；一旦该页全部子分配释放并调用 `TrimEmptyPages()`，`page.reset()` 会**立即销毁 `GeometryPage` 及其 `VertexInput`**（即便外部仍通过 `GetGpuBuffer()` 保活底层缓冲，或后续原槽号被重新提交构造了新 `GeometryPage`，旧 `const VertexInput*` 均已失效，重提交后必须从新句柄重新调用 `GetVertexInput()`）。调用方在释放页上最后一个子分配并执行 `TrimEmptyPages()` 前，须确保借用该页 `VertexInput` 的 `MeshBatch` 已移除或更新。
3. **数据模型、析构全归还断言与 RDG 时序约束**：
   - **禁止半销毁 `GeometryPage` 对象与保留析构归还检查**：未提交槽位统一以 `Pages[index] == nullptr` 表示；`~GeometryPoolStorage()` 除核对计数一致外，必须保留对每张非空已提交页 `RADRAY_ASSERT(page->Allocator.GetFreeCount() == page->Allocator.GetCapacity())`（以及 `TotalFreeElements == TotalCapacityElements`）的独立断言，用以捕获子分配晚于所属池销毁的生命周期违约。
   - **保持 `AddPassReadAccess` 导入全部已提交页并严守构图前后资源身份时序**：在 [`render_graph_pipeline.cpp`](../../modules/runtime/src/render_graph_pipeline.cpp) 第 132～140 行，`RenderGraphFramePipeline::Extract` 按 `ExtractFrameData` $\rightarrow$ `SetupGraph` $\rightarrow$ `Compile` $\rightarrow$ `ExtractMeshPass` 执行，且 [`rdg.cpp`](../../modules/runtime/src/rdg.cpp) 第 1289～1307 行的 `ImportPersistentOwned` 按具体 `GpuBuffer` 实例（而非池槽号）登记依赖与屏障。因此：
     - `AddPassReadAccess` 继续导入调用时刻全部非空已提交页（含保留空页），使 `ExtractMeshPass` 阶段**从构图时已导入的同一个 `GpuBuffer` 页面切分子区间**时仍受读依赖与状态转换保护；
     - 若在 `AddPassReadAccess` 之后调用 `TrimEmptyPages()` 再向原槽位重提交新 `GpuBuffer`（或追加新页），即使 `GetPageIndex()` 相同，新 `GpuBuffer` 也未被本帧 Pass 声明读取。故**凡影响本帧 Pass 读取的建页、空槽重提交与 `TrimEmptyPages()` 均必须在 `AddPassReadAccess` 之前（如 `ExtractFrameData`）完成**。

### 1.2 `BuildMeshPassIndirectDraws` 代表命令回查与无槽位跨池合批谓词（[`mesh_draw_command.cpp`](../../modules/runtime/src/mesh_draw_command.cpp)）

1. **构建阶段局部标量缓存与跨池谓词保持**：在 [`mesh_draw_command.cpp`](../../modules/runtime/src/mesh_draw_command.cpp) 第 276 行与第 979～988 行中，两条仅持有 `ExtraElementsIndex`（而 `VertexBuffersIndex` 与 `PayloadIndex` 均为 `kInvalidBindingIndex`）的命令可指向不同 `BindingStorage` 且允许合批。局部标量比较须逐项保留该逻辑；提交阶段双槽缓存（`MeshDrawCommandStateCache`）已完备，保持不改。

### 1.3 `TransientParameterSetPool::AcquireParameterSet` 首次入池、全链路 `activeBindings` 与基准门禁（[`transient_parameter_pool.cpp`](../../modules/runtime/src/transient_parameter_pool.cpp)）

1. **首次创建入池保活与失败回滚**：容量不足（`group.Used == group.Sets.size()`）时，必须先将新建的 `unique_ptr<ShaderParameterSet>` `push_back` 进 `group.Sets`，随后新建路径与复用路径**统一执行 `group.Sets[group.Used++].get()`**；这样既保证返回指针始终由 `group.Sets` 保活，又保证后续 `Set` / `FlushWrites` 失败时执行 `set->ResetBindingsForReuse(); --group.Used;` 绝不会在首次创建时发生 `0` 下溢。
2. **全链路统一使用 `activeBindings` 防止 `_scratch` 残留串键**：当乱序调用 A 写过 `_scratch` 后，紧接着有序调用 B 零拷贝走 `activeBindings`（不改写 `_scratch`），若桶内 `std::equal`、`FilledEntry.Count` 或 `group.KeyBindings.insert` 仍误读 `_scratch`，就会把旧请求 A 的键绑到 B 的参数集条目上。因此从哈希计算到桶内比对、集合写入及持久键入表必须全部以 `activeBindings` 为唯一数据源。
3. **头文件自包含与双向门禁**：`Group` 定义在 [`transient_parameter_pool.h`](../../modules/runtime/include/radray/runtime/transient_parameter_pool.h)，若引入 `LastEntryIndex`，须将 `kEmptyBucket` 移入 `TransientParameterSetPool` 私有类作用域并包含 `<limits>`；去留判据须同时要求连续同键正向加速与两类未命中（A/B 交替、持续新键）无退化。

---

## 二、具体实施方案（`R1`～`R3`）

### R1：`GeometryBufferPool` 稀疏页槽号直读与空页撤销提交

在 [`modules/runtime/include/radray/runtime/geometry_buffer_pool.h`](../../modules/runtime/include/radray/runtime/geometry_buffer_pool.h) 与 [`modules/runtime/src/geometry_buffer_pool.cpp`](../../modules/runtime/src/geometry_buffer_pool.cpp)（实施后同步 [`docs/architecture/runtime.md`](../architecture/runtime.md)）中：

1. **新增公开查询与空页撤销提交接口（非内联实现均置于 `.cpp`）**：
   - `VertexBufferSubAllocation` 与 `IndexBufferSubAllocation`：
     ```cpp
     // 返回池内稀疏页槽号：句柄非空（_pool != nullptr）时返回 _page，默认构造、被移动后或移动赋入空句柄释放后的空句柄返回 0。
     // 低序号空页经 TrimEmptyPages() 撤销提交后，返回值可能大于或等于所属池的 GetPageCount()，
     // 调用方不得以 GetPageCount() 作为页槽号上界，按页分桶时须按 pageIndex + 1 动态扩容。
     uint32_t GetPageIndex() const noexcept;
     ```
     在 `.cpp` 中定义为 `return _pool ? _page : 0u;`（私有 `Reset()` 同时清理 `_page = 0; _allocation = {};`）。
   - 更新 `VertexBufferSubAllocation::GetVertexInput()`、两池 `TrimEmptyPages()` 与 `AddPassReadAccess()` 契约说明：
     ```cpp
     // 返回当前已提交（非空）页面数 CommittedPageCount；不含已撤销提交的中间空槽，不可用作 GetPageIndex() 上界。
     uint32_t GetPageCount() const noexcept;
     uint64_t GetCapacityBytes() const noexcept;   // 返回当前已提交页面总容量字节数
     uint64_t GetFreeBytes() const noexcept;       // 返回当前已提交页面内可重用空闲字节数
     uint64_t GetAllocatedBytes() const noexcept;  // 返回当前活跃子分配总字节数（恒等于 GetCapacityBytes() - GetFreeBytes()）
     // 撤销池中全部无活跃子分配的已提交页面（销毁对应 GeometryPage 并释放池持有的 GpuBuffer 强引用），返回本次撤销提交的页数。
     // 只要页上仍有活跃子分配，页不会被撤销，GetVertexInput() 地址保持稳定；
     // 一旦整页空闲并被 Trim，该页的 VertexInput 立即销毁（即使外部通过 GetGpuBuffer() 保活底层 Buffer，或原槽号后续重新提交，
     // 旧 GetVertexInput() 引用均已失效，重提交后须从新子分配重新获取 GetVertexInput()）。
     // 若本帧 Pass 调用 AddPassReadAccess，任何影响本帧读取的 TrimEmptyPages() 及会新建/重提交页面的 Allocate() 须在其之前完成。
     uint32_t TrimEmptyPages() noexcept;
     // 以 Common → Common 边界导入调用时刻全部已提交页面（含保留的已提交空页面）。
     // 构图后（如 ExtractMeshPass）仅允许从此处已导入的同一个 GpuBuffer 页面继续切分子区间；新建或重提交的新页缓冲不在已声明集合内。
     void AddPassReadAccess(RDGBuilder& graph, RDGRasterPassBuilder& pass, gpu::BufferStates state = ...) const;
     ```

2. **`GeometryPoolStorage` 状态不变量、失败保护与统一建页工厂**：
   - 在 `GeometryPoolStorage` 中维护：
     ```cpp
     vector<unique_ptr<GeometryPage>> Pages;  // nullptr 表示已撤销提交的空槽（为保持高序号在用页 _page 稳定而暂留）
     uint32_t CommittedPageCount{0};
     uint64_t TotalCapacityElements{0};
     uint64_t TotalFreeElements{0};
     ```
   - 按 [C++ 约定](../guide/cpp-conventions.md#接口风格) 抽取单一工厂函数 `CreateCommittedPage(GeometryPoolStorage& storage, uint32_t slotIndex, uint32_t capacity, Nullable<const VertexInput*> input) -> Nullable<unique_ptr<GeometryPage>>`：
     ```cpp
     const uint64_t bytes = uint64_t{capacity} * storage.Stride;
     if (input && bytes > std::numeric_limits<uint32_t>::max()) return nullptr;
     auto buffer = GpuBuffer::Create(storage.Device, {.Size = bytes, .Usage = storage.Usage});
     if (!buffer) return nullptr;
     buffer->GetBuffer()->SetDebugName(radray::format("{}[{}]", storage.Name, slotIndex));
     auto page = make_unique<GeometryPage>(buffer.Release(), capacity);
     if (input) {
         page->Input = *input;
         const auto vertex = std::find_if(input->Layouts.begin(), input->Layouts.end(),
                                          [](const auto& layout) { return layout.StepMode == gpu::VertexStepMode::Vertex; });
         page->Input.Streams = {{page->Buffer->GetBuffer(), 0, bytes, vertex->Binding}};
         if (!ValidateVertexInput(page->Input, true)) return nullptr;
     }
     return page;
     ```
   - `AllocateRange` 完整流程：
     1. **零长度拦截**：`if (!count) return std::nullopt;`；
     2. **已提交页优先分配**：按槽号 `0 .. Pages.size() - 1` 扫描，跳过 `Pages[index] == nullptr`；若 `Pages[index]->Allocator.Allocate(count)` 成功，断言并扣减 `TotalFreeElements -= count`，返回 `{index, *allocation}`；
     3. **定位目标槽号**：计算 `capacity = std::max(storage.PageCapacity, count)`；查找首个 `Pages[index] == nullptr` 的空槽作为 `slotIndex`；若不存在空槽，检查 `Pages.size() < std::numeric_limits<uint32_t>::max()`（不满足则返回 `std::nullopt`），以 `static_cast<uint32_t>(Pages.size())` 作为 `slotIndex`；
     4. **建页与无副作用提交**：调用 `auto createdPage = CreateCommittedPage(storage, slotIndex, capacity, input);`，若 `!createdPage` 直接返回 `std::nullopt`（池内全部槽位与计数保持原值）；取出 `auto page = createdPage.Release();`，执行 `const auto allocation = page->Allocator.Allocate(count); RADRAY_ASSERT(allocation.has_value());`，将 `std::move(page)` 写入 `Pages[slotIndex]`（或 `Pages.push_back`），更新 `++CommittedPageCount`、`TotalCapacityElements += capacity`、`TotalFreeElements += uint64_t{capacity - count}` 并返回 `{slotIndex, *allocation}`。

3. **`TrimEmptyPages()`、析构/容量断言与 `AddPassReadAccess`**：
   - `TrimEmptyPages()`：遍历 `Pages` 中所有非空页 `page`；当 `page->Allocator.GetFreeCount() == page->Allocator.GetCapacity()` 时，同步扣减 `TotalCapacityElements -= cap`、`TotalFreeElements -= cap`、`--CommittedPageCount`，执行 `page.reset()` 并递增 `trimmed`；循环结束后执行 `while (!storage.Pages.empty() && !storage.Pages.back()) storage.Pages.pop_back();`。
   - **析构与 Debug 容量校验**：
     - `~GeometryPoolStorage()`：
       ```cpp
       uint32_t committedPages = 0;
       uint64_t totalCapacity = 0;
       for (const auto& page : Pages) {
           if (!page) continue;
           // 独立断言所有子分配均已先于池销毁归还。
           RADRAY_ASSERT(page->Allocator.GetFreeCount() == page->Allocator.GetCapacity());
           ++committedPages;
           totalCapacity += page->Allocator.GetCapacity();
       }
       RADRAY_ASSERT(committedPages == CommittedPageCount);
       RADRAY_ASSERT(totalCapacity == TotalCapacityElements);
       RADRAY_ASSERT(TotalFreeElements == totalCapacity);
       ```
     - `#if RADRAY_IS_DEBUG` 下的 `FreeBytes` 同样遍历非空页校验逐页空闲元素和、容量和及页数与三个缓存计数一致。
   - **`AddPassReadAccess`**：仅跳过 `if (!page) continue;`，将其余全部已提交页通过 `ImportPersistentOwned` 导入并声明读取。

---

### R2：评估 `BuildMeshPassIndirectDraws` 活跃批次签名缓存（精确保留原合并谓词）

在 [`modules/runtime/src/mesh_draw_command.cpp`](../../modules/runtime/src/mesh_draw_command.cpp) 中：

1. **提交阶段双槽缓存（`MeshDrawCommandStateCache` / `SubmitMeshPassIndirectDraws`）保持原样不改**。
2. **`BuildMeshPassIndirectDraws` 局部标量缓存与精确合并谓词**：
   ```cpp
   bool hasActiveBatch = false;
   bool activeIndexed = false;
   Nullable<gpu::GraphicsPipelineState*> activePipeline{nullptr};
   uint8_t activeStencil = 0;
   Nullable<const MeshDrawCommandBindingStorage*> activeStorage{nullptr};
   uint32_t activeVbIndex = kInvalidBindingIndex;
   uint32_t activePayloadIndex = kInvalidBindingIndex;
   gpu::IndexBufferView activeIndexBuffer{};
   ```
   每次展开元素时，严格按原谓词分别判定顶点槽与 Payload 槽（双侧槽号均为 `kInvalidBindingIndex` 时忽略 `BindingStorage`）：
   ```cpp
   bool canMerge = false;
   if (hasActiveBatch) {
       const bool sameVertices =
           (command.VertexBuffersIndex == kInvalidBindingIndex && activeVbIndex == kInvalidBindingIndex) ||
           (command.VertexBuffersIndex != kInvalidBindingIndex &&
            command.BindingStorage.Get() == activeStorage.Get() &&
            command.VertexBuffersIndex == activeVbIndex);
       const bool samePayload =
           (command.PayloadIndex == kInvalidBindingIndex && activePayloadIndex == kInvalidBindingIndex) ||
           (command.PayloadIndex != kInvalidBindingIndex &&
            command.BindingStorage.Get() == activeStorage.Get() &&
            command.PayloadIndex == activePayloadIndex);
       const bool sameIndex =
           (indexed == activeIndexed) && (!indexed || SameIndexBufferView(element.IndexBuffer, activeIndexBuffer));
       canMerge = command.Pipeline.Get() == activePipeline.Get() &&
                  command.StencilReference == activeStencil &&
                  sameVertices && samePayload && sameIndex;
   }
   ```
   当 `!canMerge` 新建 `MeshPassIndirectBatch` 时更新上述局部标量。
   - **去留判据**：在 `bench_rdg_upload` 中，单页高合并率负载须有可复现的正向加速，且跨页高频断批负载相比 `af121f2` 无明显退化时合入。

---

### R3：`TransientParameterSetPool` 有序零拷贝快轨（`R3a`）与 MRU 门禁评估（`R3b`）

在 [`modules/runtime/include/radray/runtime/transient_parameter_pool.h`](../../modules/runtime/include/radray/runtime/transient_parameter_pool.h) 与 [`modules/runtime/src/transient_parameter_pool.cpp`](../../modules/runtime/src/transient_parameter_pool.cpp) 中：

1. **`R3a`（直接实施）：有序输入零拷贝 + 全链路 `activeBindings` + 统一入池获取与失败回滚**：
   - **第一步（零拷贝有序检验与乱序回退）**：
     - 校验 `if (!layout) return nullptr;`；
     - 单趟扫描 `bindings` `span` 检查严格递增与无重（遇重复 `(BindingId, ArrayElement)` 直接返回 `nullptr`，不触碰 `_groups`）；
     - 若已严格递增则 `std::span<const TransientParameterBinding> activeBindings = bindings;`（零 `_scratch` 写入）；否则 `_scratch.assign(bindings.begin(), bindings.end())`、排序、验重后令 `activeBindings = _scratch;`。
   - **第二步（基于 `activeBindings` 计算哈希、单次定位 `group` 与桶内比对）**：
     - 遍历 `activeBindings` 计算 `hash`；
     - 单次定位 `auto& group = _groups[layout.get()]; if (!group.Layout) group.Layout = layout;`；
     - 若 `!group.Buckets.empty()`，探测桶表并用 **`activeBindings`（绝不读 `_scratch`）**比对：
       ```cpp
       const auto& entry = group.Entries[entryIdx];
       if (entry.Hash == hash && entry.Count == activeBindings.size() &&
           (entry.Count == 0 ||
            std::equal(activeBindings.begin(), activeBindings.end(), group.KeyBindings.data() + entry.Offset))) {
           return entry.Set;
       }
       ```
   - **第三步（统一入池推进 `Used`、写入校验回滚与 `activeBindings` 持久入表）**：
     - 与 `AcquireUninitializedParameterSet` 共用组内获取逻辑（首次创建先入池 `group.Sets`，再统一递增 `group.Used`）：
       ```cpp
       if (group.Used == group.Sets.size()) {
           auto created = _device.CreateShaderParameterSet({layout.get()});
           if (!created) return nullptr;
           group.Sets.push_back(created.Release());
       }
       gpu::ShaderParameterSet* set = group.Sets[group.Used++].get();
       ```
     - 用 `activeBindings` 写入并刷新绑定，失败时严格先清理再回退游标：
       ```cpp
       const bool written = std::all_of(activeBindings.begin(), activeBindings.end(),
                                        [&](const auto& b) { return set->Set(b.BindingId, b.ArrayElement, b.Value); });
       if (!written || !set->FlushWrites()) {
           set->ResetBindingsForReuse();
           --group.Used;
           return nullptr;
       }
       ```
     - 扩容检查后，**从 `activeBindings` 将持久键复制到 `group.KeyBindings`** 并登记 `Entries` 与 `Buckets`：
       ```cpp
       const uint32_t offset = static_cast<uint32_t>(group.KeyBindings.size());
       const uint32_t count = static_cast<uint32_t>(activeBindings.size());
       group.KeyBindings.insert(group.KeyBindings.end(), activeBindings.begin(), activeBindings.end());
       // 探测空桶 slot 后写入 group.Entries 与 group.Buckets[slot] = entryIdx
       ```

2. **`R3b`（配对基准验证后决定去留）：每布局 `LastEntryIndex` 1-entry MRU**：
   - **头文件自包含哨兵常量**：在 `transient_parameter_pool.h` 直接 `#include <limits>`，将原 `.cpp` 匿名命名空间的 `kEmptyBucket` 提升为 `TransientParameterSetPool` 私有常量 `static constexpr uint32_t kEmptyBucket = std::numeric_limits<uint32_t>::max();`，使 `Group` 内的 `uint32_t LastEntryIndex{kEmptyBucket};`、桶探测及 `BeginFrame()`（重置 `group.LastEntryIndex = kEmptyBucket;`）共用同一定义。
   - **精确查找与写回顺序**：
     1. 完成第一步得到 `activeBindings` 后，先单次定位 `group`；
     2. 在算 `HashCode` 前检查 `group.LastEntryIndex != kEmptyBucket`，用 `activeBindings` 与 `group.Entries[group.LastEntryIndex]` 比较，命中直接返回 `last.Set`；
     3. MRU 未命中后再对 `activeBindings` 计算 `hash` 并探测桶表：**普通桶命中 `entryIdx` 时更新 `group.LastEntryIndex = entryIdx;` 后返回**；
     4. 桶未命中则执行第三步创建/复用并写入集合（写失败回滚时不改 `LastEntryIndex`），成功插入 `Entries` 得到新 `entryIdx` 后**更新 `group.LastEntryIndex = entryIdx;`**。
   - **正负双向合入判据**：在 `bench_transient_parameter_pool` 中相对 `R3a` 同时满足以下全部条件时才合入 `R3b`，否则仅交付 `R3a`：
     1. **正向收益要求**：“连续同键重复获取”负载相比 `R3a` 取得稳定、可复现的吞吐提升；
     2. **未命中无退化要求**：“同布局 A/B 两键交替且仅末位 binding 不同（100% MRU 未命中）”与“持续新键获取（0% 表命中冷插入）”两类负载相比 `R3a` 均无明显退化。

---

## 三、测试与基准验证计划

1. **GTest 单元测试（`modules/runtime/tests/`）**：
   - **`GeometryBufferPool` 失败路径、移动/赋空句柄、稀疏页槽重提交与时序测试（`test_geometry_buffer_pool.cpp` / `test_rdg.cpp`）**：
     - **移动构造、移动赋值与赋空释放后的页号覆盖**：从 `Page 2` 分配句柄后分别执行移动构造与移动赋值，在被移动后的源对象仍存活时同时断言**目标句柄 `GetPageIndex() == 2`、源空句柄 `GetPageIndex() == 0`**；再对存活句柄移动赋入默认构造的空句柄（`*sub = VertexBufferSubAllocation{};` / `*sub = IndexBufferSubAllocation{};`，触发私有 `Reset()` 释放区间而不销毁句柄对象本身），断言赋空后 `sub->GetPageIndex() == 0`，以及默认构造空句柄 `GetPageIndex() == 0`；
     - **零长度与建页失败无副作用覆盖**：验证 `Allocate(0)` 返回 `std::nullopt` 且 `GetPageCount() == 0`；注入设备 `CreateBuffer` 失败，分别覆盖首次建页失败与中间空槽 `Page 1` 重提交失败，断言失败后 `GetPageCount()`、`GetCapacityBytes()`、`GetFreeBytes()`、`GetAllocatedBytes()` 及既有存活句柄均保持原值不变；
     - **中间空槽具体字节数变化、稀疏页号上界与直接查询/析构全归还断言覆盖**：配置每页容量 `8` 个元素、`Stride = 4`（每页 `32` 字节），使 `Page 0` 用满 `8` 个、`Page 1` 分配后释放变全空、`Page 2` 用 `4` 个：
       - `TrimEmptyPages()` **前**：断言 `GetPageCount() == 3`、**`GetCapacityBytes() == 96`、`GetFreeBytes() == 48`、`GetAllocatedBytes() == 48`**；
       - 调用 `TrimEmptyPages()`：断言返回值 `== 1`、**`GetPageCount() == 2`、`Page 2` 存活句柄 `GetPageIndex() == 2`（`>= GetPageCount()`），且 `GetCapacityBytes() == 64`、`GetFreeBytes() == 16`、`GetAllocatedBytes() == 48`**（同时覆盖 `Stride` 乘法、已提交容量扣减与活跃分配守恒）；
       - 保留中间空槽时查询 `GetFreeBytes()`，最后释放 `Page 0`/`Page 2` 销毁池，确认析构逐页全归还断言与容量断言零触发；
     - **顶点页中间空槽重提交与新 `VertexInput` 覆盖**：保持 `Page 0` 与 `Page 2` 存活，释放并 `Trim` `Page 1` 后再次申请落入槽位 `1` 的子分配，断言 `GetPageIndex() == 1`、`ValidateVertexInput(sub.GetVertexInput(), true) == true`、`GetVertexInput().Streams` 绑定新 `Buffer`；
     - **超大请求（`count > PageCapacity`）中间空槽重提交覆盖**：保持 `Page 0` 和 `Page 2` 占满，释放 `Page 1` 并调用 `TrimEmptyPages()`，再申请 `count > PageCapacity` 超大块，直接断言返回 `GetPageIndex() == 1`、`GetPageCount() == 3` 且 `Page 2` 句柄与缓冲有效；释放该超大块再次 `TrimEmptyPages()` 后确认 `GetPageCount() == 2` 且槽位数不增长；
     - **外部 `GetGpuBuffer()` 强引用保活与 `RenderGraphFramePipeline` 同缓冲晚分配时序覆盖**：验证外部持有 `GetGpuBuffer()` 时 `TrimEmptyPages()` 返回撤销页数且底层 `Buffer` 存活；验证 `SetupGraph` 调用 `AddPassReadAccess` 时已提交保留页为空、随后在 `ExtractMeshPass` 中从**该同一个已导入页面缓冲**分配子区间时读依赖与屏障完整生效。
   - **MDI 跨池无槽位合批与参数池串键/首次入池/回滚测试（`test_mesh_draw_command.cpp` / `test_transient_parameter_pool.cpp`）**：
     - **无顶点/Payload 槽位的跨 `BindingStorage` 合批正例**：构造两条 `VertexBuffersIndex` 与 `PayloadIndex` 均为 `kInvalidBindingIndex`、但因各自携带 `ExtraElements` 而指向两个不同 `MeshDrawCommandBindingStorage` 的同管线/同索引缓冲命令，断言 `BuildMeshPassIndirectDraws` 合并为 1 个 `MeshPassIndirectBatch`；
     - **乱序与有序交替防 `_scratch` 串键覆盖**：依次执行“乱序输入 A（写入 `_scratch`）$\rightarrow$ 有序输入 B（走 `activeBindings` 零拷贝新建集合）$\rightarrow$ 再次查询 A 与查询 B”，断言两次创建返回不同集合，且后续查询 A 精确命中 A 的参数集、查询 B 精确命中 B 的参数集；
     - **首次创建写失败与后续写失败完整回滚覆盖**：分别在**新布局首次创建第 1 个集合时**（验证先入池 `Sets` 再推进 `Used`、回滚时 `Used` 从 `1` 退回 `0` 不下溢且集合由池保活复用）以及已有集合后注入 `Set` / `FlushWrites` 失败，断言每次失败恰好触发 1 次 `ResetBindingsForReuse()`、未污染 `Entries`/`Buckets`/`LastEntryIndex`，且重试立即可复用该集合。

2. **Google Benchmark 配对测量与对照组隔离**：
   - **`benchmarks/bench_rdg_upload/bench_rdg_upload.cpp`**：配对测量 `BuildMeshPassIndirectDraws` 在 20,000 条命令高合并率（单页）与高频断批（跨页交替）下的耗时；
   - **`benchmarks/bench_transient_parameter_pool/bench_transient_parameter_pool.cpp`**：
     - **保持既有清桶基准单一变量（含跨帧重置同步）**：将 `FullTableClearParameterPool::AcquireParameterSet` 与生产池 `TransientParameterSetPool` 同步保持相同的获取逻辑；若 `R3b` 合入生产池，`FullTableClearParameterPool` 除同步 `Group::LastEntryIndex{kEmptyBucket}` 与 `AcquireParameterSet` 外，**还必须在其独立的 `BeginFrame()`（清空 `Entries` 处）同步重置 `group.LastEntryIndex = kEmptyBucket;`**（仅保留逐项清桶 vs `std::fill` 全表清桶的单一差异，防止跨帧悬垂下标访问空 `Entries`）；
     - **三方独立获取路径基准**：在同一基准二进制内并列对比 `Baseline (af121f2)`、`R3a`、`R3a + MRU (R3b)`，在绑定数 `1 / 4 / 8` ×（有序输入 / 乱序输入）下测量“连续同键”、“同布局 A/B 末位差异交替（100% MRU 未命中）”与“持续新键冷插入”三组负载，按上述正负双向门禁决定 `R3b` 去留。
