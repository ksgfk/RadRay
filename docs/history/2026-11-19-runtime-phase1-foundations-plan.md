> - 适用: 理解与实施 RadRay 第一阶段通用渲染底座补齐计划（全能力 GpuTexture 与 RHI/RDG 块布局扩展上传、D3D12 描述符堆与内聚于 FrameSlot 的瞬态参数池、原生管线缓存与联合退役 PsoCache、RDG 所有权分流视图/单 mip 整资源 Clear/黑板/两阶段异步读回/内建 GPU 时间戳）
> - 权威: 经用户确认保存的设计与实施计划；不作为当前已实现契约，当前已实现行为见 [RHI 与后端](../architecture/render-rhi.md) 与 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `modules/gpu/include/radray/gpu/types.h`, `modules/gpu_impl/src/validation.cpp`, `modules/gpu_impl/src/rhi_validation.cpp`, `modules/gpu_impl/include/radray/gpu_impl/backend/d3d12_impl.h`, `modules/gpu_impl/src/d3d12/d3d12_impl.cpp`, `modules/gpu_impl/include/radray/gpu_impl/backend/vulkan_impl.h`, `modules/gpu_impl/src/vk/vulkan_impl.cpp`, `modules/runtime/include/radray/runtime/frame_slot.h`, `modules/runtime/include/radray/runtime/gpu_resource.h`, `modules/runtime/include/radray/runtime/rdg.h`, `modules/runtime/include/radray/runtime/render_graph_pipeline.h`, `modules/runtime/include/radray/runtime/asset_manager.h`, `modules/runtime/CMakeLists.txt`

# Runtime 与 RHI 第一阶段通用渲染底座补齐计划

## 状态、基线与范围

- 记录日期：2026-11-19
- 源码基线：`4a8bf33`
- 状态：细节与跨层契约经多轮评审收敛及用户确认，待按 TDD 分步实施；本文为探索性多阶段演进的第一阶段实施计划，执行过程中若遇驱动行为差异或实测瓶颈，允许在保持安全不变量的前提下调整局部接口与策略
- 影响范围：
  - `RadRay::gpu` / `RadRay::gpu_impl`：补齐 `BC1`～`BC7` 块压缩格式与全链路块布局计算（容量校验、整子资源拷贝/读回、区域拷贝及 typed buffer 拒绝规则）；扩展 `Cube` / `Dim3D`（含非紧密 `RowsPerImage` 多切片）区域拷贝描述符与双后端录制；D3D12 Shader-Visible 资源描述符堆扩容与 Sampler Table 引用计数去重；原生管线缓存（Pipeline Cache / Library，D3D12 自持输入字节副本）二进制导入导出接口及校验包装；
  - `RadRay::runtime`：升级 `frame_slot.h/.cpp`、`gpu_resource.h/.cpp`、`rdg.h/.cpp`、`render_graph_pipeline.h/.cpp`，新增一一对应的同名文件对 `texture_asset.h/.cpp`、`transient_parameter_pool.h/.cpp`、`pso_cache.h/.cpp`、`readback_queue.h/.cpp` 及配套 GTest 测试。
- 明确不在本阶段的范围：
  - 不移动或包装已存在于 `gpu::Device::GetOrCreateSampler` 的后端采样器缓存；
  - 不封装通用 `SwapchainPresenter`（窗口交换链的获取前准备、获取图像及 `RunFrame()` fail-fast 提交与呈现时序由宿主直接控制）；
  - 不在本阶段引入多 mip / 子资源粒度依赖追踪 RDG、无界 Bindless 描述符表、`MeshDrawCommand` 多重间接绘制（MDI）合批、跨队列异步传输或硬件光线追踪接口（留归后续阶段）。

---

## 一、现状缺口与设计动机

在基线 `4a8bf33` 上，`RadRay::runtime` 已具备 `AssetManager` 原子事务热替换、`ShaderAsset`、`PipelineLayoutRegistry`、`Scene` 增量命令缓存、多槽 `FrameScheduler`、`UploadArena` 与基础 `RDG`，但在承载完整实时渲染管线时仍存在四处基础设施缺口：

1. **RHI 块压缩与三维/立方体拷贝链路未贯通，`GpuTexture` 用途受限且缺少子视图缓存与纹理资产**
   - RHI 侧 [`types.h`](../../modules/gpu/include/radray/gpu/types.h) 尚未声明 `BC1`～`BC7` 枚举与块尺寸信息，`TextureRegion2D` 缺少 `Z` 偏移与 `Depth`；[`validation.cpp`](../../modules/gpu_impl/src/validation.cpp)、[`rhi_validation.cpp`](../../modules/gpu_impl/src/rhi_validation.cpp) 与 [`vulkan_impl.cpp`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp) 在纹理容量校验、typed buffer 校验、区域拷贝及整子资源拷贝/读回中均直接按 `GetTextureFormatBytesPerPixel` 计算或取模。
   - Runtime 侧 [`gpu_resource.cpp`](../../modules/runtime/src/gpu_resource.cpp) 仅允许非压缩颜色 `Dim2D` / `Dim2DArray` 纹理并硬编码 `CopyDestination | Resource` 用途（无法表达纯渲染目标或深度附件）；仅持有单张全范围默认视图，不缓存子资源视图；`AssetManager` 中尚无与 `ShaderAsset` 对称的 `TextureAsset`。

2. **高频瞬态 `ShaderParameterSet` 缺少与 `FrameSlot` 回收顺序严格对齐的池化，且 D3D12 存在采样器堆溢出风险**
   - RHI 校验层要求参数集在被引用的视图或缓冲销毁前必须先完成 `CommandAllocator` 重置并执行 `ResetBindingsForReuse()`。在 [`frame_scheduler.cpp`](../../modules/runtime/src/frame_scheduler.cpp) 中，`BeginFrame()` 执行 `WaitSlot -> ResetCommandStorage() -> ResetFrameData()`，`WaitIdle()` 更是直接重置全部槽命令存储并清理帧对象而不再进入 `Prepare()`；参数集池必须直接内聚在 `FrameSlot` 并在 `ResetCommandStorage()` 内完成解绑。
   - 同时在 [`d3d12_impl.cpp`](../../modules/gpu_impl/src/d3d12/d3d12_impl.cpp) 中，每个 `ShaderParameterSetD3D12` 创建时都会从 `_gpuResHeap`（当前仅 `65,536`）与 `_gpuSamplerHeap`（硬件硬上限 `2,048`）独占切走持久区间，多帧槽池化带采样器的参数集会迅速耗尽 `2,048` 采样器堆槽位。

3. **缺少 RHI 原生管线缓存序列化与联合保活布局的 Runtime `PsoCache`**
   - 两个后端尚未向公共接口暴露二进制管线缓存导入/导出（且 D3D12 `ID3D12Device1::CreatePipelineLibrary` 要求输入字节缓冲在 library 存活期内持续有效）；
   - `PipelineLayoutRegistry` 仅保存 `GpuPipelineLayout` 弱引用，而 [`rhi_validation.cpp`](../../modules/gpu_impl/src/rhi_validation.cpp) 中 PSO 强依赖底层 `PipelineLayout`；热重载退役旧 PSO 时必须同步联合持有并按序释放 `shared_ptr<GpuPipelineLayout>`。

4. **RDG 缺少按所有权分流的子资源视图、单 mip 整资源清屏、图黑板、异步读回队列与内建 GPU 计时**
   - [`rdg.cpp`](../../modules/runtime/src/rdg.cpp) 当前仅支持整资源默认视图；扩展子资源视图时必须区分瞬态池、持久 `GpuTexture` 与裸导入/交换链纹理，防止残留视图锁死 `SwapChain::Recreate()`；
   - 单 mip 渲染附件的独立整面清屏仍需手写空回调 `AddRasterPass`；多模块协作构图缺少零全局状态的类型化黑板；纹理异步读回缺少构图前缓冲预留、提交 Fence 状态关联与非破坏性尺寸轮询语义；整帧与 Pass 级 GPU 时间戳采集（含读回屏障、映射缓存失效与外部帧序号关联）仍需手工拼装额外命令缓冲。

---

## 二、分步设计契约（T1 ～ T4）

所有新增与修改遵循仓库硬约束：头文件与同名 `.cpp` 一一对应，非内联函数定义放入 `.cpp`（纯头文件库 `modules/gpu` 例外），不使用二段式初始化，不引入全局变量或函数内静态可变状态，不使用异常控制流。

### T1：RHI / RDG 全链路块布局与扩展纹理上传、全能力 `GpuTexture`、子视图缓存与 `TextureAsset`

1. **纵向打通 RHI 与双后端块压缩及三维/立方体拷贝（[`types.h`](../../modules/gpu/include/radray/gpu/types.h)、[`validation.cpp`](../../modules/gpu_impl/src/validation.cpp)、[`rhi_validation.cpp`](../../modules/gpu_impl/src/rhi_validation.cpp)、[`d3d12_impl.cpp`](../../modules/gpu_impl/src/d3d12/d3d12_impl.cpp)、[`vulkan_impl.cpp`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp)）**：
   - **格式与全链路块布局**：在不重命名任何已有枚举成员的前提下，向 `gpu::TextureFormat` 增补 `BC1`～`BC7` 格式，新增 `IsBlockCompressedFormat` 与 `GetTextureFormatBlockInfo`（返回 `{BlockWidth, BlockHeight, BytesPerBlock}`；非压缩格式为 `{1, 1, bpp}`，BC1/BC4 为 `{4, 4, 8}`，其余 BCn 为 `{4, 4, 16}`，`GetTextureFormatBytesPerPixel` 对 BCn 返回 `0`）：
     - 在 `ValidateTextureDescriptor`（`validation.cpp`）中统一按块网格 `ceil(mipWidth / BlockWidth) * ceil(mipHeight / BlockHeight) * BytesPerBlock` 计算每级 mip 容量，防止 BCn 纹理绕过 `MaxResourceSize` 与溢出检查；
     - 在 `rhi_validation.cpp` 的 typed/texel buffer 绑定校验中显式拒绝 `IsBlockCompressedFormat(format)`，消除 `% 0` 除零风险；
     - 在 `rhi_validation.cpp` 的 `ValidateTextureBufferCopy` 与 `vulkan_impl.cpp` 的整子资源 `CopyBufferToTexture` / `CopyTextureToBuffer` 中统一改为块布局计算行字节数、`bufferRowLength = (alignedRowPitch / BytesPerBlock) * BlockWidth` 与块行数，确保开启 RHI 校验时整子资源拷贝与 BCn 尾级读回完整可用（D3D12 整子资源拷贝已走 `GetCopyableFootprints`）。
   - **区域拷贝描述符与非紧密切片语义**：
     - 在 `gpu::TextureRegion2D` 尾部追加 `uint32_t Z{0}` 与 `uint32_t Depth{1}`；在 `gpu::BufferTextureCopyRegion` 增加 `uint32_t RowsPerImage{0}`，明确单位为**块行数（block rows）**（非压缩格式下 `BlockHeight == 1` 即纹素行数；BCn 下为 `4x4` 块行数；`0` 表示紧密排列即 `targetBlockRows = ceil(target.Height / BlockHeight)`，非零值须 `>= targetBlockRows`）；
     - 升级 `ValidateBufferTextureCopyRegion`：支持 `Dim1D` / `Dim1DArray` / `Dim2D` / `Dim2DArray` / `Cube` / `CubeArray`（`Z == 0 && Depth == 1 && ArrayLayer < DepthOrArraySize`）与 `Dim3D`（`ArrayLayer == 0 && Z + Depth <= mipDepth`），按 `((Depth - 1) * effectiveBlockRows + (targetBlockRows - 1)) * RowPitch + rowBytes` 校验源缓冲总字节数；
     - 升级 Vulkan `CopyBufferToTextureRegion`：`bufferRowLength = (RowPitch / BytesPerBlock) * BlockWidth`，`bufferImageHeight = effectiveBlockRows * BlockHeight`，`imageOffset.z = target.Z`，`imageExtent.depth = target.Depth`；
     - 升级 D3D12 `CopyBufferToTextureRegion`：将源 `Footprint.Width` 设为块对齐宽度（BCn `< 4x4` 尾级 mip 取 `4`），`Footprint.Height` 设为 **`effectiveBlockRows * BlockHeight`**，`Footprint.Depth = target.Depth`；当 `effectiveBlockRows > targetBlockRows` 时传入显式 `D3D12_BOX{0, 0, 0, alignedWidth, alignedHeight, target.Depth}`，单次 `CopyTextureRegion` 即精确兑现 `Depth > 1` 的非紧密切片步长。

2. **升级 [`gpu_resource.h`](../../modules/runtime/include/radray/runtime/gpu_resource.h) / `gpu_resource.cpp` 与 RDG 上传**：
   - 扩展 `GpuTextureDescriptor`：支持全部维度（`Dim1D`、`Dim1DArray`、`Dim2D`、`Dim2DArray`、`Dim3D`、`Cube`、`CubeArray`）、非压缩/BCn/深度模板格式及可选 `gpu::TextureUses Usage{gpu::TextureUse::UNKNOWN}`（默认 `UNKNOWN` 时对非深度格式解析为 `CopyDestination | Resource`，对深度模板格式解析为 `DepthStencilRead | DepthStencilWrite`；显式传入非零 `Usage` 时原样采用，可精确表达无 `Resource` 用途的纯颜色或纯深度附件，`GetDesc().Usage` 保存解析后的实际标志）。
   - 默认视图与子资源视图缓存：`GetDefaultView() const noexcept` 返回类型调整为 `Nullable<gpu::TextureView*>`（当且仅当纹理具备 `TextureUse::Resource` 时非空，否则返回 `nullptr`）；新增 `GpuTextureViewKey` 与 `GetOrCreateView(gpu::Device&, const GpuTextureViewKey&)`，全范围采样命中直接复用 `_defaultView`，析构顺序严格保证：**先清空 `_viewCache`，再销毁 `_defaultView`，最后销毁 `_texture`**。
   - 持久图导入终态规则：`RDGBuilder::ImportPersistentBorrowed` / `ImportPersistentOwned` 在纹理具备 `TextureUse::Resource` 时默认收尾状态为 `ShaderRead`，无 `Resource` 用途的纯附件纹理默认保持图内最后合法状态并由 `CommitSubmittedStates()` 发布。
   - 扩展 `RDGBuilder::UploadTexture`：对齐块布局与 `Z` / `Depth` / `RowsPerImage`，原生上传 `Cube` / `CubeArray`、`Dim3D` 切片与 BCn mip。

3. **新增 `modules/runtime/include/radray/runtime/texture_asset.h` / `texture_asset.cpp`**：
   - `TextureAsset::Create` 要求非空 `IntrusivePtr<GpuTexture>` 且具备 `TextureUse::Resource`（提供非空 `gpu::TextureView* GetDefaultView()`），`ClassifyReload` 在底层 `GpuTexture` 实例指针改变时返回 `AssetReloadDecision::Breaking`。

### T2：D3D12 描述符堆改造与内聚于 `FrameSlot` 的 `TransientParameterSetPool`

1. **D3D12 后端改造（[`d3d12_impl.h`](../../modules/gpu_impl/include/radray/gpu_impl/backend/d3d12_impl.h) / [`d3d12_impl.cpp`](../../modules/gpu_impl/src/d3d12/d3d12_impl.cpp)）**：
   - **扩容 `_gpuResHeap`**：将 `D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV` 的 Shader-Visible 描述符堆容量由 `1 << 16` 提升至 `D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1`（`1,000,000`）。
   - **Sampler Table 引用计数去重**：每个 `SamplerD3D12` 分配唯一的 64 位单调递增 `SamplerId`；`ShaderParameterSetD3D12` 在 `FlushWrites()` 时以表内完整 `SamplerId` 序列为键向 `DeviceD3D12` 的 `SamplerTableCache` 获取共享 `_gpuSamplerHeap` 区间（命中递增引用计数，未命中分配并拷贝；若多槽同时在途的唯一采样器表总槽位数超过硬件 `2,048` 上限则安全返回 `false`），在重刷、`ResetBindingsForReuse()` 或 `Destroy()` 时递减引用计数并在归零时释放区间。

2. **新增 `modules/runtime/include/radray/runtime/transient_parameter_pool.h` / `transient_parameter_pool.cpp` 并内聚进 [`frame_slot.h`](../../modules/runtime/include/radray/runtime/frame_slot.h)**：
   - **简洁正交接口、同帧内容去重与峰值回缩**：
     - `AcquireParameterSet(const shared_ptr<gpu::BindingGroupLayout>& layout, std::span<const TransientParameterBinding> bindings) -> Nullable<gpu::ShaderParameterSet*>`：对本槽当帧相同布局与相同绑定内容的请求直接复用同一已完成 `Set` + `FlushWrites()` 的参数集；在 `BeginFrame()` 之后至提交前（含宿主准备阶段与 `RDGPrepareContext`）均可调用；
     - `AcquireUninitializedParameterSet(const shared_ptr<gpu::BindingGroupLayout>& layout) -> Nullable<gpu::ShaderParameterSet*>`：分配或复用未预填的参数集供自定义写入；
     - 统计滑动窗口峰值，连续多帧空闲数远超窗口峰值时自动裁剪多余空闲集归还描述符区间。
   - **严格闭合的帧槽回收顺序**：`TransientParameterSetPool` 直接作为 `FrameSlot` 成员，在 `FrameSlot::ResetCommandStorage()` 中**紧接 `_allocator->Reset()` 之后立即执行 `_parameterSets.BeginFrame()`**（解绑上一轮或未提交中止帧的全部活跃集、清空当帧去重表并回缩水位），严格早于 `FrameSlot::ResetFrameData()` 析构 `_objects`，统一覆盖 `BeginFrame()`、`WaitIdle()`（先解绑全部槽，再从旧到新清理帧对象）与 `~FrameScheduler()`（先清理 `_slots`，后销毁 `_pipeline` 的瞬态池与上传区）；同模块提供 `AllocateDynamicUniform(UploadArena&, ...)` 辅助。

### T3：RHI 原生管线缓存（D3D12 自持副本）与固定资产源 `PsoCache`

1. **RHI 层（[`types.h`](../../modules/gpu/include/radray/gpu/types.h)、`vulkan_impl`、`d3d12_impl`、`rhi_validation`）**：
   - 新增 `gpu::PipelineCache` 与 `Device::CreatePipelineCache(std::span<const byte> initialData = {})`、`PipelineCache::GetData() -> vector<byte>`，图形与计算 PSO 描述符增加可选 `Nullable<PipelineCache*> Cache{nullptr}`；
   - Vulkan 对接 `VkPipelineCache`；D3D12 对接 `ID3D12PipelineLibrary1`（不可用或头部不匹配时安全回退空缓存），且 **`PipelineCacheD3D12` 内部先以 `vector<byte>` 完整拷贝持有 `initialData`，析构或销毁时严格先释放 `ID3D12PipelineLibrary1` 再释放字节容器**，满足 D3D12 要求输入缓冲在 library 存活期持续有效的契约。

2. **新增 `modules/runtime/include/radray/runtime/pso_cache.h` / `pso_cache.cpp`**：
   - **构造期固定绑定唯一 `const AssetManager&`**（连同 `gpu::Device&`、`PipelineLayoutRegistry&` 及可选 `gpu::PipelineCache*`），消除跨 `AssetManager` 实例的 `AssetHandle` 代次混用歧义；
   - `GraphicsPsoKey` 与 `ComputePsoKey` 支持可选 `shared_ptr<GpuPipelineLayout> ExplicitLayout{nullptr}`（与 `PipelineLayoutRegistry::Acquire` 返回类型一致；为空时自动获取该 `ShaderAsset` 变体的反射布局，非空时采用调用方显式传入的共享布局）；
   - **联合持有与安全退役**：每个缓存项同时强持有 `{shared_ptr<GpuPipelineLayout>, unique_ptr<PipelineState>}` 与 `ShaderAsset` `Revision`；`RetireInvalidated(FrameSlot&)` 将失效项打包移入 `slot.Emplace(...)`，退役包裹析构时**严格先销毁 `PipelineState`、后释放 `shared_ptr<GpuPipelineLayout>`**。

### T4：RDG 所有权分流子视图、单 mip 整资源 Clear、黑板、异步读回队列与内建 GPU 计时

1. **扩展 [`rdg.h`](../../modules/runtime/include/radray/runtime/rdg.h) / `rdg.cpp`**：
   - **三路所有权分流的子资源 `ResolveView`**：新增 `RDGTextureViewDescriptor` 与 `ResolveView` 重载——RDG 瞬态纹理走 `RDGTransientResourcePool` 跨帧缓存；持久 `GpuTexture` 导入走其自身 `GetOrCreateView` 缓存；裸导入 `gpu::Texture*`（含交换链后备缓冲）走当帧 `CompiledRDG` 的 `execution.ViewOwners`，在 `ResetFrameData()` / `WaitIdle()` 时立即释放。
   - **单 mip 整资源原生 Clear 节点**：`RDGBuilder` 增加 `AddClearColor` 与 `AddClearDepthStencil`：
     - **显式校验并限定 `MipLevels == 1`**（对 `MipLevels > 1` 的纹理调用直接标记图构建失败 `Fail`）；
     - 对同时含 Depth 与 Stencil aspect 的纹理，当 `AddClearDepthStencil` 仅清除其中一个 aspect（`clearDepth != clearStencil`）时，未清除的 aspect 采用 `LoadAction::Load` + `StoreAction::Store` 并在 `RDGAccess` 标记 `Read = true, Preserve = true`，保留前序对该 aspect 的写入依赖。
   - **类型化帧黑板（`RDGBlackboard`）**：基于 `FrameSlot::GetMemoryResource()` 提供 `Emplace<T>` 与 `Get<T>`，禁止重复注册同一类型。

2. **新增 `modules/runtime/include/radray/runtime/readback_queue.h` / `readback_queue.cpp`**：
   - 支持构造期配置 `maxActiveRequests`（默认 `4096`），拒绝重复活跃 `requestId`，暴露 `HasRequest(uint64_t)` 与 `GetActiveRequestCount()`；
   - **明确单待提交图约束与四态生命周期（`Reserved -> Enqueued -> Submitted(fenceValue) -> Consumed`）**：
     - 同一 `AsyncReadbackQueue` 实例在同一时刻至多服务一个尚未 `MarkSubmitted` 的构图周期；
     - `ReserveTexture(uint64_t requestId, const ReadbackTexturePlan& plan) -> bool`：可在构图前调用，校验格式/上限并从空闲池获取或创建 `MemoryType::ReadBack` 缓冲，进入 `Reserved` 态；
     - `EnqueueReserved(RDGBuilder& graph, uint64_t requestId, RDGTextureHandle source, uint32_t mipLevel = 0, uint32_t arrayLayer = 0) -> bool`（并保留一步式便捷重载 `EnqueueTexture`）：向 `graph` 挂载带 `MarkOutput` 的拷贝节点与 `HostRead` 转换，将该请求转为 `Enqueued` 态；
     - `CancelUnsubmitted() noexcept`：**同时取消并回收全部处于 `Reserved` 与 `Enqueued`（即尚未 `MarkSubmitted`）态的请求及其缓冲**；若当前构图在 `Compile()` / `Prepare()` 阶段放弃而未提交，调用 `CancelUnsubmitted()` 即可确保废弃图的 `Enqueued` 请求绝不会被后续另一张图的 `MarkSubmitted` 误推进；
     - `MarkSubmitted(uint64_t frameFenceValue) noexcept`：仅将当前处于 `Enqueued` 态的请求推进为 `Submitted(frameFenceValue)` 态（关联本次队列提交发出的 Fence 目标值）；
   - **精确区分空指针查尺寸与零容量报错的 `Poll` 签名**：
     - `Poll(uint64_t requestId, uint64_t completedFenceValue, ReadbackResultInfo& outInfo, Nullable<byte*> destination = nullptr, size_t capacity = 0) -> bool`；
     - 仅当条目处于 `Submitted(fenceValue)` 态且 `completedFenceValue >= fenceValue` 时 `Ready = true`；处于 `Reserved`、`Enqueued`（未提交）或 `completedFenceValue < fenceValue` 时一律 `Ready = false`，绝不映射或回收缓冲；
     - 当 `destination != nullptr && capacity < PackedByteSize`（含非空指针但 `capacity == 0`）时，立即返回 `false` 且**不消费**条目；
     - 当 `destination == nullptr`（尺寸与就绪状态查询）或 `Ready == false` 时，填写 `outInfo` 并返回 `true`，**不映射、不消费**条目；
     - **仅当** `Ready == true` 且 `destination != nullptr && capacity >= PackedByteSize` 且映射成功时：执行 `InvalidateMappedRange`、逐行剥离 `RowPitch` 填充拷贝、`Unmap`、回收缓冲至空闲池并移除 `requestId`。

3. **内建可选 GPU 时间戳采样（[`render_graph_pipeline.h`](../../modules/runtime/include/radray/runtime/render_graph_pipeline.h) / `.cpp`）**：
   - 支持设置 `RDGGpuProfilingMode`（`Off`、`FrameOnly`、`FrameAndPasses`）及每帧外部关联序号 `userSequence`；
   - 录制时在 `ResolveQueryData` 前插入读回缓冲 `Undefined/HostRead -> CopyDestination` 屏障，在 `ResolveQueryData` 后**插入 `CopyDestination -> HostRead` 屏障**；
   - 槽位 GPU 完成后映射读回缓冲、**调用 `InvalidateMappedRange` 刷新主机可见缓存**并 `Unmap`，生成包含 `{FrameId, UserSequence, TotalNanoseconds, Passes[{Name, Nanoseconds}]}` 的完成快照并发布 `RADRAY_PROFILE_PLOT` 指标。

---

## 三、测试、文档同步与实施顺序

按依赖自底向上分 4 个提交单元以 TDD 方式推进：

1. **单元 1（T1）**：先行编写双后端测试，开启 RHI 校验覆盖：非零 `Cube` 面上传、BCn 尾级小 mip（`< 4x4`）上传与整子资源 `CopyTextureToBuffer` 读回、typed buffer 绑定 BCn 格式拒绝、`Dim3D` 在 `Depth > 1` 且非紧密 `RowsPerImage` 下两张不同切片图案上传读回、无 `Resource` 用途纯附件 `GpuTexture`（`GetDefaultView() == nullptr`）跨图状态保持及 `TextureAsset` 热替换分类，随后落地实现。
2. **单元 2（T2）**：先行编写 D3D12 采样器表大量重复共享与唯一组合耗尽 `2,048` 后恢复两类测试、百万级 `_gpuResHeap` 容量校验，以及 `test_transient_parameter_pool.cpp`（覆盖同帧内容去重、峰值回缩、正常轮转与 `WaitIdle()` 下参数集先于帧对象解绑），随后落地实现。
3. **单元 3（T3）**：新增 RHI `PipelineCache` 往返测试（在 `CreatePipelineCache` 返回后立即清零/销毁调用方输入缓冲，再创建 PSO 与调用 `GetData()` 验证 D3D12 无悬空引用）与 `test_pso_cache.cpp`（覆盖构造期固定 `AssetManager`、`ExplicitLayout` 共享布局、仅缓存项持有旧布局且 PSO 在途时的热重载退役顺序），随后落地实现。
4. **单元 4（T4 + 文档同步）**：扩充 `test_rdg.cpp`、`test_rdg_upload_gpu.cpp` 并新增 `test_readback_queue.cpp`（覆盖三路视图所有权分流与 `WaitIdle()` 后无残留导入视图、`AddClear*` 对 `MipLevels > 1` 的拒绝及单面清除保留另一深度/模板 aspect 前序写入、`RDGBlackboard`、`AsyncReadbackQueue` 在“放弃图 A（`CancelUnsubmitted` 清理 `Enqueued`）后提交图 B 仅图 B 可读”、“已入图未提交 / 已提交未完成 / Fence 已完成”三态隔离与“空指针查尺寸两次 → 非空指针零/小容量失败不消费 → 正常读取消费 → 再次读取失败”协议、带 `HostRead` 屏障与 `InvalidateMappedRange` 及 `UserSequence` 的整帧/Pass 级 GPU 计时），落地后同步更新 `docs/architecture/overview.md`、`docs/architecture/render-rhi.md`、`docs/architecture/runtime.md`，并执行全套 `ctest`、`python tools/check_docs.py` 与 `git diff --check`。
