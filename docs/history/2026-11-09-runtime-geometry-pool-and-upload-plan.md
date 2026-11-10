> - 适用: 理解 RadRay 对齐 UE5（`Core` / `RenderCore` / `RDG`）职责边界，根治 `FrameScheduler` 缺少准备前槽回收、`RDG` 不支持只读组合状态、上传器单缓冲重分配受限等底层设施缺陷，并在 `RadRay::core` 与 `RadRay::runtime` 中下沉 `SpanAllocator`、`VertexBufferPool` / `IndexBufferPool`、分页式 `BatchedBufferUpload` 的演进计划
> - 权威: 经用户确认保存的架构演进计划；不作为已实现契约，当前已实现行为见 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `modules/core/include/radray/sparse_set.h`, `modules/core/include/radray/object_arena.h`, `modules/runtime/include/radray/runtime/vertex_input.h`, `modules/runtime/include/radray/runtime/mesh_batch.h`, `modules/runtime/include/radray/runtime/mesh_draw_command.h`, `modules/runtime/src/mesh_draw_command.cpp`, `modules/runtime/include/radray/runtime/frame_slot.h`, `modules/runtime/include/radray/runtime/frame_scheduler.h`, `modules/runtime/src/frame_scheduler.cpp`, `modules/runtime/include/radray/runtime/rdg.h`, `modules/runtime/src/rdg.cpp`

# Runtime 帧槽准备阶段、RDG 只读组合状态、几何子分配池与分页批量上传演进计划

## 状态、基线与范围

- 记录日期：2026-11-09
- 源码基线：2026-11-09 当前工作树（已完成 [Runtime 架构边界收紧、状态驻留与数据导向重构计划](2026-11-08-runtime-dod-and-state-interning-plan.md) 的 `RadRay::core` 与 `RadRay::runtime` 实现）
- 状态：经架构评审，识别出 `RadRay::runtime` 现有底层设施的四处结构性缺陷，决定**扩大修改范围从根源升级基础设施，而非在业务层或新工具类上打局部补丁**，待实施
- 核心原则：**遇基础设施缺陷则直接修正基础设施契约**。本次演进涵盖：
  1. **修正 `FrameScheduler` 单函数黑盒缺陷**：新增幂等的 `FrameScheduler::BeginFrame() -> FrameSlot&`，将「槽等待 + `CommandAllocator` 重置 + 退役资源清理 + 帧数据重置」前移至场景更新与上传准备之前，从根源消除准备期扩容 Buffer 时 `CommandAllocator` 尚未重置的 RHI 引用冲突；
  2. **修正 `RDG` 只读状态窄化缺陷**：将 `RDG` Buffer 状态从单枚举 `render::BufferState` 升级为 `render::BufferStates` 位掩码，原生支持 `Vertex | ShaderRead` 等只读组合状态及同 Pass 内多次 `ReadBuffer` 状态自动按位合并；
  3. **下沉 `SpanAllocator` 与对称的 `VertexBufferPool` / `IndexBufferPool`**：在 `RadRay::core` 提供通用 1D 连续区间分配器 `SpanAllocator`，在 `RadRay::runtime` 提供对称的顶点缓冲池与索引缓冲池，统一支持池级 `AddPassReadAccess` 与 `Common -> ... -> Common` 跨帧边界状态交接，由池级接口统一声明几何读取依赖；
  4. **下沉 `MappedUploadBuffer` 与分页式 `BatchedBufferUpload`**：底层 `MappedUploadBuffer` 支持自定义 `BufferUses`（覆盖实例顶点流与常量流）；上层 `BatchedBufferUpload` 采用**多页（Paged）持久映射架构**，空间不足时追加新页而不 `Unmap`/不销毁当帧已写页，从根源保证返回的写指针全帧永不失效、零 Write-Combined 内存回读、且无需调用方预先遍历计算总字节数。

---

## 一、背景：为什么局部补丁会暴露出底层基础设施缺陷？

本计划聚焦 RadRay 的四类接口与生命周期问题：帧准备前的槽回收、RDG Buffer 只读组合状态、上传写指针稳定性，以及顶点与索引缓冲池的对称依赖声明。

深入剖析可见，这些问题并非新工具类的细节疏漏，而是 **`RadRay::runtime` 现有基础设施存在四处根源性缺陷**：

### 基础设施缺陷一：`FrameScheduler` 将「槽回收」与「录制执行」锁死在 `RunFrame` 内部，缺失「帧准备（BeginFrame）」阶段
- **现状病灶**：
  - [`FrameScheduler::RunFrame`](../../modules/runtime/src/frame_scheduler.cpp) 将 `WaitSlot(slot)`、`slot.ResetCommandStorage()`、`slot.ResetRetiredResources()`、`slot.ResetFrameData()` 与 `SyncCachedMeshDrawCommands`、`Extract`、`Record` 捆绑在同一个不可分割的函数中，且要求 `RunFrame` 期间不得从外部修改 `Scene`。
  - 但真实引擎的每帧流水线必须在**当前帧槽已完成 GPU 等待且 `CommandAllocator` 已重置之后、`SyncCachedMeshDrawCommands` 之前**，执行当帧的 `Scene` 增删、旧 GPU 资源退役（`slot.RetireResource`）以及当帧动态缓冲（如实例流 `MappedUploadBuffer`）的扩容与写入。
- **准备期约束**：
  - GPU Fence 已完成不等于旧录制引用已经解除；替换或销毁本槽上轮录制使用的 Buffer 前，仍须先重置 CommandAllocator。
  - 场景更新和上传准备需要能够访问已完成回收的当前帧槽，直接登记旧资源的退役 owner。

### 基础设施缺陷二：`RDG` 将 Buffer 状态窄化为单枚举 `BufferState`，与 RHI `BufferStates` 位标志脱节
- **现状病灶**：
  - `RadRay::render`（RHI 层）将 `BufferState` 定义为按位左移标志，`BarrierBufferDescriptor` 原生接受 `render::BufferStates`（`EnumFlags<BufferState>`），D3D12 与 Vulkan 后端均支持 `Vertex | ShaderRead` 等只读组合状态。
  - 然而 [`rdg.h`](../../modules/runtime/include/radray/runtime/rdg.h) 与 [`rdg.cpp`](../../modules/runtime/src/rdg.cpp) 将接口窄化为单枚举 `render::BufferState`：`ValidBufferState` 用 `switch (state)` 拒绝任何组合位，`AddAccess` 在同一 Pass 对同一 Buffer 声明两种只读状态（如同时作为 `Vertex` 和 `ShaderRead`）时直接报错 `"conflicting whole-resource states in a pass"`。

### 基础设施缺陷三：单缓冲重分配上传模型违背流式写入的指针稳定性与 WC 内存特性
- **现状病灶**：
  - 若批量上传器（`BatchedBufferUpload`）内部仅持有一个单块 `MappedUploadBuffer` 并在容量不足时重分配替换：
    1. 调用方若分多次获取写指针，后续扩容会导致先前返回的写指针立即悬垂；
    2. 若在扩容时从旧映射区 `memcpy` 前缀数据到新映射区，会触发极慢的 **Write-Combined (WC) 内存 CPU 回读**；
    3. 迫使调用方在每次上传前必须先跑一遍预遍历循环累加对齐后的 `requiredBytes` 并显式调用 `Reserve`。

### 基础设施缺陷四：仅有顶点池而无对称的索引缓冲池，导致 RDG 几何依赖声明不完整
- **现状病灶**：
  - [`MeshBatchElement`](../../modules/runtime/include/radray/runtime/mesh_batch.h) 已原生设计了 `IndexBufferOffset` 与 `FirstIndex` 字段以支持索引缓冲子分配，且 [`SubmitMeshDrawCommands`](../../modules/runtime/src/mesh_draw_command.cpp) 支持同 IndexBufferView 状态短路。
  - 若只下沉 `VertexBufferPool` 而不提供对称的 `IndexBufferPool`，索引 Buffer 仍需单独管理生命周期并在 `SetupGraph` 中单独编写索引 Buffer 的 RDG 导入与 `BufferState::Index` 依赖循环。

---

## 二、根源架构演进方案（四层正交体系）

针对上述四处基础设施缺陷，不再采取“在 `MappedUploadBuffer` 里塞 `_retiredBuffers`”、“限制 `AddPassReadAccess` 只传单态”、“要求调用方手写索引 Buffer 遍历和预计算总上传字节数”的补丁做法，而是直接升级 `RadRay::core` 与 `RadRay::runtime`：

| 模块与层级 | 涉及文件 | 核心改动与新增类型 | 根治的问题 |
|---|---|---|---|
| **1. `FrameScheduler` 两阶段帧驱动升级** | `modules/runtime/include/radray/runtime/frame_scheduler.h`<br>`modules/runtime/src/frame_scheduler.cpp`<br>`modules/runtime/include/radray/runtime/frame_slot.h` | `FrameScheduler::BeginFrame() -> FrameSlot&`<br>`FrameScheduler::GetCurrentSlot()` | 在场景更新与上传准备前完成 `WaitSlot` + `ResetCommandStorage` + `ResetRetiredResources` + `ResetFrameData`，使准备期 `CommandAllocator` 已处于 Reset 状态且可直接调用 `slot.RetireResource` |
| **2. `RDG` 只读组合 `BufferStates` 升级** | `modules/runtime/include/radray/runtime/rdg.h`<br>`modules/runtime/src/rdg.cpp` | `ImportBuffer` / `ReadBuffer` 升级为 `render::BufferStates`<br>`ValidBufferState` / `AddAccess` 支持只读位组合与合并 | 允许 `BufferState::Vertex \| BufferState::ShaderRead` 等任意合法只读状态组合，以及同 Pass 内多次 `ReadBuffer` 自动按位或（`\|=`）合并 |
| **3. `SpanAllocator` 与对称的 `VertexBufferPool` / `IndexBufferPool`** | `modules/core/include/radray/span_allocator.h/.cpp`<br>`modules/runtime/include/radray/runtime/geometry_buffer_pool.h/.cpp` | `SpanAllocator`、`SpanAllocation`<br>`VertexBufferPool`、`VertexBufferSubAllocation`<br>`IndexBufferPool`、`IndexBufferSubAllocation` | 提供纯 CPU 区间合并分配器，并在 Runtime 层同时提供顶点池与索引池，统一以 `Common -> ... -> Common` 边界导入 RDG 并声明 `AddPassReadAccess` |
| **4. `MappedUploadBuffer` 与分页式 `BatchedBufferUpload`** | `modules/runtime/include/radray/runtime/upload_buffer.h`<br>`modules/runtime/src/upload_buffer.cpp` | `MappedUploadBuffer`（支持自定义 `BufferUses`）<br>`BatchedBufferUpload`（多页 Paged 暂存架构） | `MappedUploadBuffer` 在已 `BeginFrame` 的槽上可安全原地扩容；`BatchedBufferUpload` 按页追加而不释放当帧已写页，写指针全帧永不失效、零 WC 内存回读、免预计算总大小 |

---

## 三、各模块详细设计契约

### 模块一：`FrameScheduler::BeginFrame()` 与帧槽生命周期前移

1. **两阶段帧循环契约（兼容单步 `RunFrame`）**：
   - 新增 `FrameSlot& FrameScheduler::BeginFrame() noexcept`：
     - 若当前槽尚未开启（`!_slotBegun`）：
       1. 检查 `_nextFrameId != UINT64_MAX`；
       2. 执行 `WaitSlot(slot)`（若 `slot._inFlight` 则调用 `_pipeline->Wait(slot)` 并清除 `_inFlight`）；
       3. 执行 `slot.ResetCommandStorage()`（**立即重置该槽的 `CommandAllocator`，解除上一轮录制对所有 GPU 资源的 epoch 引用**）；
       4. 执行 `slot.ResetRetiredResources()` 与 `slot.ResetFrameData()`；
       5. 设置 `slot._frameId = _nextFrameId`，置 `_slotBegun = true`（Debug 下同时开启 `slot._inFrameCallback = true`）。
     - 若当前槽已开启（`_slotBegun == true`，例如上一帧在准备阶段因外部输入错误提前中止而未调用 `RunFrame`）：
       - 再次调用 `BeginFrame()` **不执行任何重置，直接返回当前已开启的 `FrameSlot&`**：既保证已登记在 `slot.GetArena()` 中的对象与 `_inFrameCallback` 状态继续有效，又保证本轮准备期已通过 `slot.RetireResource` 登记的退役资源绝不被提前释放（退役队列严格等待完整槽位轮转回到本槽或调用 `WaitIdle()` 后才回收）。
   - `FrameScheduler::RunFrame(Scene&)` / `RunFrame(const Scene&)`：
     - 若 `!_slotBegun`，内部自动先调用 `BeginFrame()`（**100% 兼容现有直接调 `RunFrame` 的代码与测试**）；
     - 随后执行 `SyncCachedMeshDrawCommands` $\to$ `_pipeline->Extract(source, slot)` $\to$ `_pipeline->Record(source, slot)`；
     - 结束时置 `_slotBegun = false`（Debug 下 `slot._inFrameCallback = false`）、`slot._inFlight = true`，递增 `_nextFrameId` 并推进 `_currentSlotIndex = (_currentSlotIndex + 1) % GetSlotCount()`。
   - `FrameScheduler::WaitIdle()`：
     - 若 `_slotBegun == true`，在等待与三阶段全槽清理后同步将 `_slotBegun` 与 `slot._inFrameCallback` 复位为 `false`。
2. **帧准备窗口**：
   - 调用方可在 BeginFrame 返回后更新 Scene、准备上传并登记退役资源，再调用 RunFrame。
   - 对当前槽独占的 MappedUploadBuffer，BeginFrame 已完成等待和 allocator Reset，可在该窗口扩容并释放旧 Buffer。

---

### 模块二：`RDG` 原生支持只读组合状态 `render::BufferStates`

1. **接口与内部存储升级（[`rdg.h`](../../modules/runtime/include/radray/runtime/rdg.h) / [`rdg.cpp`](../../modules/runtime/src/rdg.cpp)）**：
   - 将以下接口的 `render::BufferState` 参数升级为 `render::BufferStates`（由于 `EnumFlags<BufferState>` 支持从单枚举 `BufferState` 隐式构造，现有传单枚举的调用代码无需修改即可编译）：
     - `RDGBuilder::ImportBuffer(render::Buffer* buffer, render::BufferStates initialState, std::optional<render::BufferStates> finalState = std::nullopt, RDGExternalOwnership ownership = {})`
     - `RDGRasterPassBuilder::ReadBuffer(RDGBufferHandle buffer, render::BufferStates state)`
     - `RDGComputePassBuilder::ReadBuffer(RDGBufferHandle buffer, render::BufferStates state = render::BufferState::ShaderRead)`
     - `RDGTransientBufferAllocation::LastState` 与 `RDGTransientResourcePool::CommitBufferState`
2. **只读组合状态合法性判定（`ValidBufferState` 与 `ValidAccess`）**：
   - 定义只读状态全集掩码：
     ```cpp
     constexpr render::BufferStates kReadOnlyBufferStates =
         render::BufferState::CopySource | render::BufferState::Vertex | render::BufferState::Index |
         render::BufferState::CBuffer | render::BufferState::ShaderRead | render::BufferState::Indirect;
     ```
   - 在 `ValidBufferState(const render::BufferDescriptor& desc, render::BufferStates state)` 中：
     - 单态 `Undefined`、`Common`、`CopyDestination`、`UnorderedAccess`、`HostRead`、`HostWrite` 保持原有互斥单态校验；
     - 若 `state != render::BufferState::UNKNOWN && (state & ~kReadOnlyBufferStates) == render::BufferState::UNKNOWN`（即非空纯只读状态组合），则逐位检查：凡置位的标志，其对应的 `BufferUse`（`CopySource`、`Vertex`、`Index`、`CBuffer`、`Resource`、`Indirect`）必须在 `desc.Usage` 中具备，全部满足即返回 `true`。
   - 在 `ValidAccess` 中：
     - `Raster` Pass 的 Buffer 访问：保留 `BufferState::UnorderedAccess` 单态下的只读（`ReadBuffer`）、写入（`WriteBuffer`）与读写（`ReadWriteBuffer`）访问；纯只读组合状态允许 `Vertex | Index | CBuffer | ShaderRead | Indirect` 的任意非空子集（拒绝 `UnorderedAccess | Vertex` 等跨类混合）；
     - `Compute` Pass 的 Buffer 访问：同样保留 `BufferState::UnorderedAccess` 单态下的只读、写入与读写访问；纯只读组合状态允许 `CBuffer | ShaderRead | Indirect` 的任意非空子集（拒绝与 `UnorderedAccess` 混合）；
     - `Copy` Pass 的只读 Buffer 访问允许 `CopySource`。
3. **同 Pass 只读状态自动合并（`AddAccess`）**：
   - 当同一 Pass 对同一 Buffer 多次调用 `ReadBuffer`（例如先声明 `BufferState::Vertex`，后声明 `BufferState::ShaderRead`）时，若 `previous` 与新 `access` 均为纯只读状态（`!previous.Write && !access.Write` 且两者均属于 `kReadOnlyBufferStates` 子集，或同为 `UnorderedAccess` 只读），则直接执行 **`previous.State |= access.State`** 并经过 `ValidAccess` 校验，而不再误报 `"conflicting whole-resource states in a pass"`。

---

### 模块三：`radray::SpanAllocator` 与对称的 `VertexBufferPool` / `IndexBufferPool`

#### 1. `radray::SpanAllocator`（`RadRay::core`，`<radray/span_allocator.h>`）
- 对齐 UE5 `FSpanAllocator`：管理固定容量 `[0, capacity)` 内的 1D 连续整数区间分配与释放；内部通过 `set<pair<uint32_t, uint32_t>> _freeByCount`（Best-Fit 查找）与 `map<uint32_t, uint32_t> _freeByOffset`（释放时前后相邻空闲区间自动合并）消除流式碎片。

#### 2. `radray::VertexBufferPool` 与 `IndexBufferPool`（`RadRay::runtime`，`<radray/runtime/geometry_buffer_pool.h>`）
- **`VertexBufferPool`（单交错顶点流 + 可选延迟实例流）**：
  - `VertexBufferPoolDescriptor`：
    ```cpp
    struct VertexBufferPoolDescriptor {
        uint32_t VertexStride{0};
        uint32_t VertexStreamBinding{0};
        uint32_t PageVertexCapacity{150'000};
        render::BufferUses AdditionalUsage{};
        std::span<const render::VertexBufferLayout> Layouts{};
        std::span<const VertexInputAttribute> Attributes{};
        std::string_view DebugNamePrefix{"VertexBufferPool"};
    };
    ```
  - 对齐 [`ValidateVertexInput`](../../modules/runtime/src/mesh_draw_command.cpp)：`Create` 校验 `Layouts` 中必须**恰好包含 1 个** `StepMode == VertexStepMode::Vertex` 的流（`Binding == VertexStreamBinding` 且 `ArrayStride == VertexStride`），以及 **0 个或 1 个** `StepMode == VertexStepMode::Instance` 的流；每页生成的稳定 `VertexInput` 在 `Streams` 中仅绑定该页顶点 Buffer 到 `VertexStreamBinding`，保留实例流槽位在 `Submit` 期通过 `MeshPassSubmitArgs::InstanceStream` 延迟注入。
  - `AddPassReadAccess(RDGBuilder& graph, RDGRasterPassBuilder& pass, render::BufferStates state = render::BufferState::Vertex) const`：
    遍历池内所有页，以 **`graph.ImportBuffer(page.Buffer.get(), BufferState::Common, BufferState::Common)`** 导入并声明 `pass.ReadBuffer(handle, state)`（得益于模块二，`state` 既可传默认的 `BufferState::Vertex`，也可传 `BufferState::Vertex | BufferState::ShaderRead`）。
- **`IndexBufferPool`（对称的分页索引缓冲子分配池）**：
  - `IndexBufferPoolDescriptor`：
    ```cpp
    struct IndexBufferPoolDescriptor {
        uint32_t IndexStride{4}; // 2 或 4 字节
        uint32_t PageIndexCapacity{300'000};
        render::BufferUses AdditionalUsage{};
        std::string_view DebugNamePrefix{"IndexBufferPool"};
    };
    ```
  - 每页持有一个 `MemoryType::Device`、`Usage = BufferUse::Index | BufferUse::CopyDestination | desc.AdditionalUsage` 的 `render::Buffer` 与一个 `SpanAllocator`。
  - `IndexBufferSubAllocation` 为仅可移动 RAII 句柄，提供 `GetBuffer()`、`GetIndexStride()`、`GetFirstIndex()`、`GetIndexCount()`、`GetByteOffset()`、`GetByteSize()` 以及直接构造 `MeshBatchElement` 所需的视图信息。
  - `AddPassReadAccess(RDGBuilder& graph, RDGRasterPassBuilder& pass, render::BufferStates state = render::BufferState::Index) const`：
    同样以 `(BufferState::Common, BufferState::Common)` 边界将池内所有索引页导入 RDG 并声明 `pass.ReadBuffer(handle, state)`。
  - **池级依赖声明**：在 `SetupGraph` 中只需调用 `vertexPool.AddPassReadAccess(graph, pass)` 与 `indexPool.AddPassReadAccess(graph, pass)` 两行代码，即可声明两池保留页的顶点与索引读取依赖。

---

### 模块四：`radray::MappedUploadBuffer` 与分页式 `BatchedBufferUpload`（`<radray/runtime/upload_buffer.h>`）

1. **`MappedUploadBuffer`（单块持久映射缓冲，用于动态实例流/常量流及上传页基座）**：
   - `Create(render::Device& device, uint64_t capacity, render::BufferUses usage = render::BufferUse::CopySource)`：
     创建 `MemoryType::Upload`、`Usage = usage | BufferUse::MapWrite`、`Hints = ResourceHint::PersistentMap` 的 `render::Buffer` 并持久 `Map(0, capacity)`。
   - `Reserve(render::Device& device, uint64_t minCapacity)`：
     在已通过 `FrameScheduler::BeginFrame()` 完成 `WaitSlot` + `ResetCommandStorage()` 的槽上，若 `minCapacity > _capacity`，按 2 的幂分配新 Buffer 并直接安全 `Unmap` + 析构旧 Buffer（因 `CommandAllocator` 已在 `BeginFrame` 中 Reset，无任何悬挂录制引用）。

2. **`BatchedBufferUpload`（分页式帧级批量拷贝上传器，对齐 UE5 `FDynamicBufferAllocator` + `FUploadBuffer`）**：
   - **分页式（Paged）内部架构**：
     - 内部维护 `vector<Page> _pages`（每个 `Page` 持有一个 `unique_ptr<MappedUploadBuffer>` 与当前页内游标 `uint64_t Cursor{0}`）以及 `vector<BufferUploadRegion> _regions`（记录 `{uint32_t PageIndex, uint64_t SrcOffset, render::Buffer* DstBuffer, uint64_t DstOffset, uint64_t ByteSize}`）。
   - **写入期指针绝对稳定契约（零预遍历、零指针失效、零 WC 回读）**：
     - `Acquire(render::Buffer* dstBuffer, uint64_t dstOffset, uint64_t byteSize, uint64_t alignment = 16) -> Nullable<byte*>`：
       1. 在当前活动页（或已有空闲页）中尝试按 `alignment` 对齐切分 `byteSize` 字节；
       2. 若所有现有页剩余空间均不足，则直接新建一个容量为 `max(_defaultPageSize, NextPow2(byteSize))` 的新 `MappedUploadBuffer` 页追加到 `_pages` 尾部，并在新页上分配；
       3. **之前已写入的页在整个帧周期内绝不 `Unmap`、绝不重分配、绝不回读拷贝**——因此任何一次 `Acquire` 返回的 `byte*` 写指针在当帧 `CommitToGraph` / `Reset` 之前**恒定有效**，调用方也无需预先遍历一遍数据计算总字节数！
     - `Stage(render::Buffer* dstBuffer, uint64_t dstOffset, const void* data, uint64_t byteSize, uint64_t alignment = 16) -> bool`：调用 `Acquire` 并完成 `memcpy`。
   - **重置与容量收敛（`Reset()`）**：
     - 在下一轮帧槽复用（`BeginFrame` 后）调用 `Reset()` 时，将各页 `Cursor` 归零並清空 `_regions`。
   - **RDG 提交与跨帧边界状态交接（`CommitToGraph`）**：
     - `CommitToGraph(RDGBuilder& graph, std::string_view passName = "BatchedBufferUpload")`：
       1. 若 `_regions` 为空则直接返回；
       2. 对所有 `Cursor > 0` 的活动页执行 `Flush(0, page.Cursor)`，并以 `graph.ImportBuffer(page.Buffer->Get(), BufferState::HostWrite, BufferState::HostWrite)` 导入 RDG；
       3. 对所有去重后的目标 `DstBuffer` 统一以 **`graph.ImportBuffer(dstBuffer, BufferState::Common, BufferState::Common)`** 导入并调用 `graph.MarkOutput(dstHandle)`（确保首帧仅上传、无 RasterPass 读取时，帧末自动从 `CopyDestination` 还原回 `BufferState::Common`，且与 `VertexBufferPool` / `IndexBufferPool` 的 `AddPassReadAccess` 导入状态严格一致）；
       4. 注册单个合并的 `graph.AddCopyPass(passName, ...)`，在 Setup 中声明 `ReadBuffer(pageHandle)` 与 `WriteBuffer(dstHandle)`，在 Execute 中按序执行全部 `CopyBufferToBuffer`。

---

## 四、分阶段实施与验证计划

| 阶段 | 涉及文件 | 关键验证指标 |
|---|---|---|
| **Stage 1：`FrameScheduler::BeginFrame` 与 `RDG` 只读组合 `BufferStates` 升级** | `frame_scheduler.h/.cpp`、`test_frame_scheduler.cpp`<br>`rdg.h/.cpp`、`test_rdg.cpp` | 1. 验证 `FrameScheduler::BeginFrame()` 在 `RunFrame` 前完成 `WaitSlot` + `ResetCommandStorage` + `ResetRetiredResources` + `ResetFrameData`；验证连续调用 `BeginFrame()` 时不执行任何重置（`slot.GetArena()` 已分配对象继续存活、`slot.RetireResource` 继续可调用，且另一槽仍引用的退役资源不提前释放，直至完整轮转或 `WaitIdle` 才释放）；验证准备期替换销毁上一轮本槽录制引用过的 Buffer 不触发 RHI 引用错误；<br>2. 验证 `RDG` 支持 `BufferState::Vertex \| BufferState::ShaderRead` 纯只读状态组合与同 Pass 多次 `ReadBuffer` 自动按位合并，同时保持 `ReadBuffer(handle, BufferState::UnorderedAccess)` 单态只读访问兼容且拒绝 `UnorderedAccess \| Vertex` 混合。 |
| **Stage 2：`RadRay::core` 新增 `SpanAllocator` + `RadRay::runtime` 新增 `VertexBufferPool` / `IndexBufferPool`** | `span_allocator.h/.cpp`、`test_span_allocator.cpp`<br>`geometry_buffer_pool.h/.cpp`、`test_geometry_buffer_pool.cpp` | 1. 验证 `SpanAllocator` Best-Fit 分配与前驱/后继/双向空闲区间自动合并；<br>2. 验证 `VertexBufferPool::Create`（恰好 1 个 `Vertex` 流 + 0 或 1 个 `Instance` 流）与 `IndexBufferPool::Create`（步长 2 或 4 字节）参数校验、跨页扩容时 `VertexInput*` 地址稳定性及同页共享去重；<br>3. 验证 `VertexBufferPool::AddPassReadAccess` 与 `IndexBufferPool::AddPassReadAccess` 在单态及 `Vertex \| ShaderRead` 组合态下的 RDG 导入与屏障正确性。 |
| **Stage 3：`RadRay::runtime` 新增 `MappedUploadBuffer` 与分页式 `BatchedBufferUpload`** | `upload_buffer.h/.cpp`、`test_upload_buffer.cpp` | 1. 验证 `MappedUploadBuffer` 支持 `CopySource` / `Vertex` 等用途创建，以及在 `BeginFrame()` 重置槽后调用 `Reserve` 扩容的安全性；<br>2. 验证分页式 `BatchedBufferUpload`：**先 `Acquire` 第一个指针暂不写入，随后第二次 `Acquire` 触发跨页新建 Page，再向第一个指针写入数据并 `CommitToGraph`**，验证两段数据均被正确拷入目标 Buffer（证明写指针跨扩容永不失效且无 WC 内存回读）；<br>3. 验证**“首帧仅上传（无 RasterPass 读取）、次帧 RasterPass 绘制”**跨帧 `Common -> CopyDestination -> Common` 状态交接，以及同帧同时上传顶点池与索引池并绘制的端到端正确性。 |
| **Stage 4：文档同步与完整验证** | `docs/architecture/runtime.md`<br>`docs/history/README.md` | 运行 core 与 runtime 全部测试；同步当前接口与生命周期文档，运行 `python tools/check_docs.py` 与 `git diff --check`。 |
