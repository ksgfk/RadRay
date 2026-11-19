> - 适用: 理解与实施 RadRay 第二阶段 RHI 与 Runtime 局部写入保活、子区域/子资源清屏、显式顺序边与区间屏障、子区域颜色纹理读回及设备丢失（DeviceLost）单向终态与免等待安全析构补齐计划
> - 权威: 经用户确认保存的设计与实施计划；不作为当前已实现契约，当前已实现行为见 [RHI 与后端](../architecture/render-rhi.md) 与 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `modules/core/include/radray/viewport.h`, `modules/gpu/include/radray/gpu/types.h`, `modules/gpu_impl/include/radray/gpu_impl/validation.h`, `modules/gpu_impl/src/validation.cpp`, `modules/gpu_impl/src/rhi_validation.cpp`, `modules/gpu_impl/include/radray/gpu_impl/backend/d3d12_impl.h`, `modules/gpu_impl/src/d3d12/d3d12_impl.cpp`, `modules/gpu_impl/include/radray/gpu_impl/backend/vulkan_impl.h`, `modules/gpu_impl/src/vk/vulkan_impl.cpp`, `modules/runtime/include/radray/runtime/rdg.h`, `modules/runtime/src/rdg.cpp`, `modules/runtime/include/radray/runtime/readback_queue.h`, `modules/runtime/src/readback_queue.cpp`, `modules/runtime/include/radray/runtime/frame_pipeline.h`, `modules/runtime/src/frame_pipeline.cpp`, `modules/runtime/include/radray/runtime/frame_scheduler.h`, `modules/runtime/src/frame_scheduler.cpp`, `modules/runtime/include/radray/runtime/render_graph_pipeline.h`, `modules/runtime/src/render_graph_pipeline.cpp`, `modules/runtime/src/gpu_device.cpp`

# Runtime 与 RHI 第二阶段：RDG 局部写入保活、顺序屏障、子区域读回与设备丢失终态实施计划

## 状态、基线与范围

- 记录日期为 2026-11-20，本次修订日期为 2026-11-21。
- 基线：承接 [Runtime 与 RHI 第一阶段通用渲染底座补齐计划](2026-11-19-runtime-phase1-foundations-plan.md) 已落地后的 `develop` 分支实现状态
- 状态：设计与接口契约经多轮静态走查与用户确认后归档，待按 TDD 分步实施；归档时未修改生产代码或运行测试
- 影响范围：
  - `RadRay::gpu` / `RadRay::gpu_impl`：新增面向颜色纹理且与 `BufferToTextureCopyDescriptor` 对称的 `TextureToBufferCopyDescriptor` 及 `CommandBuffer::CopyTextureToBufferRegion`（含多切片非紧密 `RowsPerImage` 与紧凑尾片对齐拆分），放开单平面深度格式（`D16_UNORM`、`D32_FLOAT`）的整子资源 `CommandBuffer::CopyTextureToTexture`（D3D12 整子资源传空 `pSrcBox`，Vulkan 按格式选择 `DEPTH` / `COLOR` aspect），贯通参数/对齐/容量校验层与双后端拷贝录制；补齐覆盖资源创建、命令缓冲重置/闭合、队列、围栏与交换链的 `Device::IsDeviceLost()` 单向终态锁存（Vulkan 在 `MarkDeviceLost` 首次翻转且非由 `DrainAndClear` 自身等待触发时当场完成唯一一次 `vkDeviceWaitIdle` 同步，严格早于任何帧槽或交换链/设备原生对象销毁）、`CommandQueue::Submit -> bool`（保留 RHI 校验拒绝与 `DeviceLost` 返回 `false`）与免重复阻塞 `Wait()`、不伪造完成度的 `Fence`（保留丢失前真实完成值，严禁向调用方暴露 `UINT64_MAX`）、丢失态仍释放内部渲染作用域与校验引用的 `CommandAllocator::Reset()`，以及 `SwapChainD3D12` 与丢失态 `SwapChainVulkan`（跳过冗余围栏等待但完整销毁原生句柄，且允许丢失态 `DiscardAcquiredFrame`）的非 `RADRAY_ABORT` 收敛；
  - `RadRay::runtime`：扩展 `rdg.h/.cpp` 支持光栅 `SetRenderArea`、附件目标子资源范围声明、局部覆盖写版本保活（`Preserve`）、`CanMerge` 局部区域与加载动作约束、多 mip / 多层与子矩形 `AddClearColor` / `AddClearDepthStencil` 以及轻量 `RDGPassHandle` + `AddOrderDependency` / `AddOrderBarrier` 显式执行顺序控制；扩展 `readback_queue.h/.cpp` 支持源纹理子区域异步读回并保持既有不带区域重载的整 mip 尺寸校验契约；打通 `RenderGraphFramePipeline::Prepare / Record` 检出 `IsDeviceLost()` 的非 `RADRAY_ABORT` 提前返回（健康设备 OOM 维持 fail-fast），扩展 `FramePipeline::Submit`、`RenderGraphFramePipeline::Submit` 与 `FrameScheduler::RunFrame` 返回 `bool`（`FrameScheduler::RunFrame` 对 `!IsDeviceLost()` 的健康设备提交失败维持 fail-fast `RADRAY_ABORT`，仅在 `IsDeviceLost()` 时返回 `false`），并在 `FrameScheduler::WaitSlot` 检出 `IsDeviceLost()` 时跳过 `OnSlotCompleted`（防止映射读取未完成的 GPU 时间戳），保障 `WaitIdle` 与 `DeviceContext` 析构零阻塞、零 `RADRAY_ABORT` 干净回收；在 `modules/gpu_impl/tests/`、`modules/runtime/tests/` 补齐校验单测、Fake RHI 设备丢失测试与双后端真设备测试。
- 明确不在本阶段的范围：
  - 不将 `RDG` 改造为逐子资源或逐像素矩形区间的细粒度屏障状态机（物理屏障仍维持整资源粒度，通过 `Preserve` 数据依赖保活前序非完全覆盖写入）；
  - 深度/模板纹理的整子资源读回继续使用既有的带 aspect 参数 `CommandBuffer::CopyTextureToBuffer`，不在区域读回新接口中拆分深度/模板多平面；
  - 设备丢失仅提供可观测的单向终态与整棵旧设备/调度器对象树的免等待无泄漏析构，不在 `RadRay::runtime` 内部自动重建新 `DeviceContext` 或静默迁移已驻留 GPU 资源（是否销毁旧会话并创建新设备由上层宿主决定）；
  - 不在本阶段引入 `MeshDrawCommand` 多重间接绘制（MDI）自动合批、无界 Bindless 描述符表、跨队列异步传输或硬件光线追踪接口（相关后续阶段前置约束见文末第四节）。

---

## 一、现状缺口与设计动机

第一阶段落地后，`RadRay` 已具备块压缩与多切片区域上传 `CopyBufferToTextureRegion`、支持局部 `RenderArea` 的 RHI `BeginRenderPass`（D3D12 自动切换 `PRESERVE` 并录制矩形清除，Vulkan 透传 `VkRenderPassBeginInfo::renderArea`）、三路所有权 `ResolveView`、单 mip 整资源清屏、相邻同附件 Pass 合并（`CanMerge`）及四态 `AsyncReadbackQueue`。但在承载包含局部图集更新、多级 mip 维护、硬件查询区间包裹、局部像素读回以及外部硬件/驱动重置恢复的工程场景时，仍存在五处明确缺口：

1. **RDG 未透传 `RenderArea`，且局部覆盖写会被死代码裁剪误删前序写入**
   - `RDGRasterPassBuilder` 尚未暴露 `SetRenderArea`；
   - 在 [`rdg.cpp`](../../modules/runtime/src/rdg.cpp) 中，`AddTextureUpload` 与 `CopyPass` 对目标资源的写入已设置 `Preserve = true`，但 `WriteColor` 与 `SetDepthStencil` 仅在 `LoadAction::Load` 时置 `Preserve = true`。当后继光栅 Pass 使用 `LoadAction::Clear` 或 `DontCare` 仅更新单个子矩形或仅更新多 mip / 多层纹理中的某一个子资源时，`Compile` 只建立 `OrderDependencies` 而不建立 `DataDependencies` 并直接覆盖整张纹理的 `LastWriter`；若下游仅消费最终纹理句柄，`Compile.Cull` 会将前序针对其他矩形、其他 mip 或其他数组层的写入全部裁掉。

2. **清屏辅助接口硬限制 `MipLevels == 1`，且瞬态多 mip/多层附件缺少免外部视图的子资源绑定途径**
   - `RDGBuilder::AddClearColor` 与 `AddClearDepthStencil` 目前拒绝 `MipLevels > 1` 的纹理，不支持指定 `(mipLevel, baseArrayLayer, arrayLayerCount)` 或局部 `Rect`；
   - `WriteColor` / `SetDepthStencil` 若要绑定非第 0 级 mip，当前只能传入外部 `explicitView` 且 `ValidateAttachments` 仅允许导入资源携带 `explicitView`，RDG 瞬态多 mip 纹理无法直接声明单级 mip 渲染附件。

3. **缺少针对非图管资源副作用（如 `QueryPool` 时间戳区间）的点对点顺序边与区间顺序屏障**
   - `SchedulePasses` 将就绪节点分为 `readyHost`、`readyUpload`（含 `Upload` 与 `Copy`）与 `readyOther`（`Raster` 与 `Compute`），在无资源依赖边时会重排执行顺序并优先调度 `Upload` / `Copy`；
   - `NeverCull()` 仅保证节点存活，不提供先后顺序约束；且对于 `Begin -> Pass A -> Pass B -> End` 这类中间包含多个互不依赖节点的采样区间，若仅连接相邻节点 `Begin -> A` 与 `B -> End`，调度器仍可将无入度的 `B` 提前至 `Begin` 之前或将无出度的 `A` 推迟至 `End` 之后，因此需要 $O(N)$ 边数的区间顺序屏障原语。

4. **颜色纹理到缓冲拷贝缺少子区域能力，且纹理间拷贝一刀切拦截了单平面深度格式整子资源拷贝**
   - RHI 仅有整子资源 `CommandBuffer::CopyTextureToBuffer`，无法按非零起点 `(X, Y, Z)`、局部 `(Width, Height, Depth)` 或指定目标 `RowPitch` / `RowsPerImage` 将颜色纹理局部区域拷入目标缓冲；
   - `CommandBufferValidation::CopyTextureToTexture` 目前对所有 `IsDepthStencilFormat` 一律拒绝，且 `CommandBufferVulkan::CopyTextureToTexture` 硬编码 `VK_IMAGE_ASPECT_COLOR_BIT`，导致单平面深度纹理（`D16_UNORM`、`D32_FLOAT`）在双后端均合法的整子资源纹理间拷贝无法执行。

5. **RHI 与 Runtime 对硬件/驱动设备丢失（`DeviceLost`）的处理半途中断，直接 `RADRAY_ABORT` 阻断宿主收尾与对象树析构**
   - [RHI 与后端](../architecture/render-rhi.md) 已规定“`DEVICE_LOST` 要求设备恢复，不再等待无法完成的 fence”，且 [`vulkan_impl.cpp`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp) 中的 `SwapChainVulkan` 已专门跟踪 `_terminalResult == VK_ERROR_DEVICE_LOST` 以避免销毁时调用 `vkDeviceWaitIdle` / `vkWaitForFences`；
   - 但 `Device`、`CommandQueue::Submit/Wait`、`Fence::GetCompletedValue/WaitFor/Wait`、`CommandAllocator::Reset`、[`d3d12_impl.cpp`](../../modules/gpu_impl/src/d3d12/d3d12_impl.cpp) 的 `_CheckD3D12Result` / `SwapChainD3D12` 以及 [`frame_scheduler.cpp`](../../modules/runtime/src/frame_scheduler.cpp) / [`gpu_device.cpp`](../../modules/runtime/src/gpu_device.cpp) 在遇到 `VK_ERROR_DEVICE_LOST` 或 `DXGI_ERROR_DEVICE_REMOVED` / `DXGI_ERROR_DEVICE_RESET` 时仍直接调用 `RADRAY_ABORT`，将外部显卡 TDR、驱动热更新或扩展坞断开等同于调用方编程违约直接杀死宿主进程，既使 `SwapChainVulkan` 已有的丢失清理路径在真实析构链路上不可达，也使上层宿主无法保存未落盘数据、输出结构化故障报告或安全销毁旧 `DeviceContext` 重建设备。

---

## 二、分项设计方案

### T1：RDG 光栅 `RenderArea`、附件子资源范围、局部写保活与 `CanMerge` 约束

1. **光栅 `RenderArea` 声明与有符号坐标边界校验（[`rdg.h`](../../modules/runtime/include/radray/runtime/rdg.h) / [`rdg.cpp`](../../modules/runtime/src/rdg.cpp)）**
   - 在 `RDGRasterPassBuilder` 新增成员函数（声明与定义分离，使用 [`viewport.h`](../../modules/core/include/radray/viewport.h) 中的 `radray::Rect`，实现位于 `rdg.cpp`）：
     - `RDGRasterPassBuilder& SetRenderArea(Rect area);`
   - 在 `RDGUserPass` 保存 `std::optional<Rect> RenderArea`；
   - 在 `ValidateAttachments` 计算出附件目标子资源尺寸 `(Width, Height, Layers)` 后，严格对齐 [`validation.cpp`](../../modules/gpu_impl/src/validation.cpp) 的 `ValidateRenderScope` 规则：先校验有符号起点非负且宽高非零（`area.X >= 0 && area.Y >= 0 && area.Width > 0 && area.Height > 0`），再执行扩宽无溢出上界比较（`uint64_t(area.X) + area.Width <= Width && uint64_t(area.Y) + area.Height <= Height`），不满足则调用 `Fail` 拒绝构图；
   - 在 `CompiledRDG::Record` 构造 `gpu::RenderPassBeginDescriptor` 时透传 `RenderArea`。

2. **附件目标子资源范围声明与三路视图解析**
   - 在 `WriteColor` 与 `SetDepthStencil` 支持指定单级 mip 与数组层范围（新增接收 `gpu::SubresourceRange` 的重载，默认仍为 `{.BaseArrayLayer = 0, .ArrayLayerCount = gpu::SubresourceRange::All, .BaseMipLevel = 0, .MipLevelCount = 1}`；保留外部 `explicitView` 重载兼容既有调用，二者互斥）；
   - `ValidateAttachments` 统一归一化子资源范围，要求 `MipLevelCount == 1`，并按目标 `BaseMipLevel` 推导附件有效宽高；
   - `Prepare` / `Record` 阶段对未提供 `explicitView` 的附件统一走 `ResolveTextureView` 三路所有权分流获取单级 mip 附件视图，使瞬态纹理、持久 `GpuTexture` 与裸导入 `gpu::Texture*` 均可直接渲染任意合法 `(mip, layer)`。

3. **局部覆盖写的版本保活规则（`Preserve = true`）**
   - 在 `Compile` 阶段紧随 `ValidateAttachments` 之后（不依赖 `SetRenderArea` 与 `WriteColor` 的链式调用先后）统一修正当前光栅 Pass 各附件对应的 `RDGAccess`：
     - 设底层纹理描述符为 `desc`，附件归一化子资源范围为 `range`，有效渲染矩形为 `effectiveArea`；
     - 若 `effectiveArea.X != 0 || effectiveArea.Y != 0 || effectiveArea.Width != mipWidth || effectiveArea.Height != mipHeight`（局部矩形），或 `range.MipLevelCount != desc.MipLevels || range.BaseArrayLayer != 0 || range.ArrayLayerCount != expectedTotalLayers`（局部子资源），则判定为**非整纹理完全覆盖写**，无论 `LoadAction` 为 `Clear`、`DontCare` 还是 `Load`，均置 `access.Preserve = true`；
     - `access.Read` 仍仅在 `LoadAction::Load`（或存在只读深度/模板 aspect）时为 `true`。由此既在存在前序 `LastWriter` 时通过 `DataDependencies` 保活其他矩形/mip/layer 的旧写入，又不会在首次对 `TextureState::Undefined` 导入资源执行局部 `Clear` 时误报 `resource read before initialization`。

4. **`CanMerge` 合并判据收紧**
   - 相邻光栅 Pass 合并除原有的附件槽位、纹理句柄、存储动作与后继 `LoadAction::Load` 检查外，还要求两 Pass 的附件归一化子资源范围与显式视图指针完全一致，且归一化后的有效 `RenderArea` 完全相等。

---

### T2：RDG 多 Mip / 多层与子矩形清屏辅助接口

- 在 [`rdg.h`](../../modules/runtime/include/radray/runtime/rdg.h) / [`rdg.cpp`](../../modules/runtime/src/rdg.cpp) 保留无子资源参数的单 mip 整幅清屏重载（继续严格要求 `MipLevels == 1`，同步改为返回 `RDGPassHandle`），并新增显式子资源与可选子矩形重载：
  ```cpp
  RDGPassHandle AddClearColor(RDGTextureHandle texture, gpu::ColorClearValue value,
                              gpu::SubresourceRange range, std::optional<Rect> renderArea = std::nullopt);
  RDGPassHandle AddClearDepthStencil(RDGTextureHandle texture, gpu::DepthStencilClearValue value,
                                     gpu::SubresourceRange range, bool clearDepth = true, bool clearStencil = true,
                                     std::optional<Rect> renderArea = std::nullopt);
  ```
- 新重载校验 `range.MipLevelCount == 1` 且目标纹理具备对应附件用途，内部直接复用 `T1` 的子资源附件声明与 `SetRenderArea`，自动获得局部写 `Preserve = true` 保活语义。

---

### T3：RDG 显式执行顺序边与区间顺序屏障（`RDGPassHandle`、`AddOrderDependency`、`AddOrderBarrier`）

1. **与资源句柄风格一致的 `RDGPassHandle`**
   - 与现有 `RDGTextureHandle`、`RDGBufferHandle` 保持一致的单索引轻量值语义（构图期仅针对当前图 `_data.Passes` 索引范围校验有效性）：
     ```cpp
     struct RDGPassHandle {
         uint32_t Index{std::numeric_limits<uint32_t>::max()};
         constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
         friend constexpr bool operator==(RDGPassHandle, RDGPassHandle) noexcept = default;
     };
     ```
   - 在 `RDGRasterPassBuilder`、`RDGComputePassBuilder`、`RDGCopyPassBuilder` 增加 `RDGPassHandle GetHandle() const noexcept;`，并让 `UploadBuffer`、`UploadTexture`、`AddClearColor`、`AddClearDepthStencil` 返回所创建节点的 `RDGPassHandle`（**当输入校验触发 `Fail` 提前返回，或 `UploadBuffer` 命中非动态缓冲零字节上传合法空操作分支 `!source.GetMaxSize() && !resource.Dynamic` 而未调用 `AddNode` 时，统一返回默认无效句柄 `RDGPassHandle{}` 即 `IsValid() == false`，绝不误取前序无关节点下标；调用方仅对 `IsValid() == true` 的句柄追加顺序边或顺序屏障**）。

2. **点对点顺序边、$O(N)$ 区间顺序屏障与节点保活边界**
   - 在 `RDGBuilder` 增加：
     ```cpp
     void AddOrderDependency(RDGPassHandle before, RDGPassHandle after);
     void AddOrderBarrier(RDGPassHandle barrier);
     ```
   - **`AddOrderDependency(before, after)`**：校验两句柄均落在 `[0, _data.Passes.size())` 且严格满足 `before.Index < after.Index`（拒绝越界、自环及逆声明序建边，违约调用 `Fail`），向 `after` 的 `OrderDependencies` 去重注入 `before.Index`；
   - **`AddOrderBarrier(barrier)`（裁剪后在存活子图上按声明序线性建边）**：
     - 在 `RDGPassNode` 记录布尔标记 `bool OrderBarrier{false}`；
     - 调用 `AddOrderBarrier(barrier)` 时：严格要求 `barrier` 为当前最新声明的节点 `barrier.Index + 1 == _data.Passes.size()` 且 `!_data.Passes[barrier.Index].OrderBarrier`（拒绝越界句柄、重复登记同一屏障及对旧节点逆序补打屏障，从拓扑声明序上杜绝 `A -> B` 已有依赖后再补 `AddOrderBarrier(A)` 产生环路，违约调用 `Fail`），仅置 `_data.Passes[barrier.Index].OrderBarrier = true`；
     - **顺序控制与保活正交解耦，并在 `Compile.Cull` 之后统一生成屏障顺序边**：`AddOrderDependency` 与 `AddOrderBarrier` **只产生 `OrderDependencies`，绝不隐式设置 `NeverCull()`**。在 `RDGBuilder::Compile` 完成死代码裁剪（各节点 `Alive` 已确定）之后、`SchedulePasses` 之前，按声明序单趟扫描全部 **`Alive` 存活节点**（严格 $O(N)$ 复杂度，自动跳过已被裁剪的普通节点与已被裁剪的中间屏障节点）：
       1. 维护 `std::optional<uint32_t> lastAliveBarrier` 与当前开放区间的存活非屏障节点列表 `openAliveInterval`；
       2. 若存活节点 `i` 带有 `OrderBarrier == true`：当 `openAliveInterval` 非空时对其中每个节点 `p` 注入顺序边 `p -> i` 并清空 `openAliveInterval`，当 `openAliveInterval` 为空且 `lastAliveBarrier` 有效时注入直接顺序边 `*lastAliveBarrier -> i`，随后更新 `lastAliveBarrier = i`；
       3. 若存活节点 `i` 为普通节点（`OrderBarrier == false`）：若 `lastAliveBarrier` 有效则注入顺序边 `*lastAliveBarrier -> i`，并将 `i` 加入 `openAliveInterval`；
       由此无论“两个存活屏障之间的普通节点全部被裁掉”还是“`A`（存活屏障）→ `B`（被裁剪屏障）→ `C`（存活独立拷贝）”场景，`C` 都会直接收到前一个存活屏障 `A -> C` 的顺序约束，绝不会被 `SchedulePasses` 越过 `A` 提前调度。
   - **外部 `QueryPool` 在 `RenderGraphFramePipeline` 下的标准录制模式**：
     - 光栅/计算 Pass 回调内可通过 `context.GetEncoder().GetCommandBuffer()->WriteTimestamp(...)` 直接打点，调用方须在声明该 Pass 时**同时标记 `.NeverCull()` 并调用 `AddOrderBarrier`**（既保证被后继整幅清屏覆盖时查询槽仍在 GPU 上写出有效结果，也防止后继无资源依赖的独立 `Copy`/`Upload` 节点被调度器提至该打点 Pass 之前）；
     - 鉴于 `RenderGraphFramePipeline::Record` 为 `final`（仅维护管线内建剖析查询池），外部 `QueryPool` 的集中 `ResetQueryPool` 与 `ResolveQueryData`（含读回缓冲 `CopyDestination -> HostRead` 屏障）通过构图首尾各挂载一个标记 `.NeverCull()` 并调用 `AddOrderBarrier` 的 `CopyPass`（经 `RDGCopyPassContext::GetCommandBuffer()`）完成。

---

### T4：RHI 颜色纹理 `CopyTextureToBufferRegion`、单平面深度整子资源 `CopyTextureToTexture` 与 `AsyncReadbackQueue` 子区域读回

1. **RHI 描述符与命令接口（[`types.h`](../../modules/gpu/include/radray/gpu/types.h)）**
   - 新增面向颜色格式且与 `BufferToTextureCopyDescriptor` 对称的描述符：
     ```cpp
     struct TextureBufferCopyRegion {
         TextureRegion2D Source{};
         uint64_t DestinationOffset{0};
         uint32_t RowPitch{0};
         uint32_t RowsPerImage{0};  // 切片间块行数；0 表示按拷贝块行数紧密排列。
     };

     struct TextureToBufferCopyDescriptor {
         Texture* Source{nullptr};
         Buffer* Destination{nullptr};
         TextureBufferCopyRegion Region{};
     };
     ```
   - 在 `gpu::CommandBuffer` 新增纯虚函数：
     `virtual bool CopyTextureToBufferRegion(const TextureToBufferCopyDescriptor& desc) noexcept = 0;`

2. **共享校验与 RHI 校验包装（[`validation.h`](../../modules/gpu_impl/include/radray/gpu_impl/validation.h) / [`validation.cpp`](../../modules/gpu_impl/src/validation.cpp) / [`rhi_validation.cpp`](../../modules/gpu_impl/src/rhi_validation.cpp)）**
   - 新增 `ValidateTextureReadbackRegion`：
     - 复用 [`validation.cpp`](../../modules/gpu_impl/src/validation.cpp) 中 `ValidateTextureUploadRegion` 的前置守卫顺序：先取 `block = GetTextureFormatBlockInfo(source.Format)`，在执行任何块字节取模或除法之前立即校验 `source.Dim != TextureDimension::UNKNOWN`、`source.SampleCount == 1`、**`block.BytesPerBlock != 0`（前置拦截 `TextureFormat::UNKNOWN` 防止后续 `% block.BytesPerBlock` 除零）**、`!IsDepthStencilFormat(source.Format)`（明确仅接收颜色格式，深度/模板整子资源读回继续统一走既有带 aspect 的 `CommandBuffer::CopyTextureToBuffer`，避免 D3D12 深度/模板要求 `pSrcBox == nullptr` 与多平面拆分限制）、`source.Usage.HasFlag(TextureUse::CopySource)` 以及 `destination.Usage.HasFlag(BufferUse::CopyDestination)`；
     - 校验源子区域 `MipLevel`、`ArrayLayer`、`(X, Y, Z)` 与 `(Width, Height, Depth)` 非空不越界，BCn 压缩格式满足块对齐与 mip 右/下尾块边界规则；
     - 镜像复用 [`validation.cpp`](../../modules/gpu_impl/src/validation.cpp) 中 `ValidateBufferTextureCopyRegion` 的块字节对齐与步长可表示性检查：
       - `DestinationOffset % std::max(uint64_t{1}, detail.TextureDataPlacementAlignment) == 0 && DestinationOffset % block.BytesPerBlock == 0`；
       - `RowPitch >= rowBytes && RowPitch % block.BytesPerBlock == 0 && RowPitch % std::max(1u, detail.TextureDataPitchAlignment) == 0`；
       - `imageRows >= rows`（其中 `imageRows = RowsPerImage ? RowsPerImage : rows`），且纹素换算步长不溢出 `uint32_t`（`imageRows <= std::numeric_limits<uint32_t>::max() / block.BlockHeight` 且 `RowPitch / block.BytesPerBlock <= std::numeric_limits<uint32_t>::max() / block.BlockWidth`）；
       - 目标缓冲自 `DestinationOffset` 起的剩余容量覆盖最后一切片最后一行有效字节（`DestinationOffset <= destination.Size && rowBytes <= destination.Size - DestinationOffset && lastRow <= (destination.Size - DestinationOffset - rowBytes) / RowPitch`）；
   - **Vulkan 队列族 `minImageTransferGranularity` 与纯传输队列 4 字节缓冲偏移下限校验**：在 `QueueVulkan`（[`vulkan_impl.h`](../../modules/gpu_impl/include/radray/gpu_impl/backend/vulkan_impl.h) / [`vulkan_impl.cpp`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp)）记录所属队列族的 `VkExtent3D _minImageTransferGranularity`；`CommandBufferValidation` 在 Vulkan 后端录制纹理↔缓冲传输前结合 `_cmdPool->_queue` 的实际队列族能力补齐两项双向校验（同时覆盖 `CopyTextureToBufferRegion`、`CopyBufferToTextureRegion` 以及整子资源 `ValidateTextureBufferCopy`）：
     1. **纯传输队列 `bufferOffset` 4 字节下限（VUID 07737 / 07746）**：当命令池所属队列族同时缺少 `VK_QUEUE_GRAPHICS_BIT` 与 `VK_QUEUE_COMPUTE_BIT`（且未启用 `maintenance11`）时，无论纹理格式块大小或设备 `optimalBufferCopyOffsetAlignment` 是否小于 `4`，统一要求上传 `SourceOffset % 4 == 0` 且读回 `DestinationOffset % 4 == 0`；
     2. **队列族图像传输粒度（含 BCn 压缩块乘积换算）**：对照 `_cmdPool->_queue->_minImageTransferGranularity` 校验区域 `(X, Y, Z)` 与 `(Width, Height, Depth)`——当粒度为 `(0, 0, 0)`（仅允许整幅 mip 传输的专用拷贝队列）时，要求起点为 `(0, 0, 0)` 且宽高深等于对应 mip 完整尺寸；当粒度非零时，鉴于 Vulkan 对块压缩格式以**压缩块个数**作为 `minImageTransferGranularity` 的单位，各有效轴的纹素步长严格按乘法计算为 `step = uint64_t{granularityAxis} * blockDim`（未压缩格式 `blockDim = 1` 得 `granularityAxis`，BCn 压缩格式如 `BlockWidth = 4` 且 `granularityX = 2` 得 `step = 8` 纹素），要求起点为 `step` 整数倍且尺寸为 `step` 整数倍或贴合 mip 末端边界（`offset + extent == mipExtent`），违约调用 `Context->Reject` 并返回 `false`；
   - 在 `CommandBufferValidation` 包装句柄归属、非 Pass 录制状态及上述区域与队列能力校验。

3. **D3D12 与 Vulkan 后端实现（[`d3d12_impl.cpp`](../../modules/gpu_impl/src/d3d12/d3d12_impl.cpp) / [`vulkan_impl.cpp`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp)）**
   - **D3D12（颜色纹理子区域与多切片 `RowsPerImage > rows` 对称处理）**：
     - 构造源颜色子资源 `D3D12_TEXTURE_COPY_LOCATION`（`SUBRESOURCE_INDEX`）与目标缓冲 `D3D12_PLACED_SUBRESOURCE_FOOTPRINT`（`Offset = DestinationOffset`、`Width = block.Columns(Source.Width) * block.BlockWidth`、`RowPitch`）；
     - 当 `Source.Depth == 1 || imageRows == rows` 时：`Footprint.Height = rows * block.BlockHeight`、`Footprint.Depth = Source.Depth`，配合源 `D3D12_BOX{X, Y, Z, X + Width, Y + Height, Z + Depth}` 单次调用 `CopyTextureRegion(&to, 0, 0, 0, &from, &srcBox)`；
     - 当 `Source.Depth > 1 && imageRows > rows` 时：镜像对照 `CmdListD3D12::CopyBufferToTextureRegion` 的边界算法——检查 `uint64_t{imageRows} * Source.Depth - 1 <= (available - rowBytes) / RowPitch`（`fullFootprintFits`）：
       - 若装得进：设 `Footprint.Height = imageRows * block.BlockHeight`、`Footprint.Depth = Source.Depth`，配合高度为 `Source.Height`、深度为 `Source.Depth` 的 `srcBox` 单次调用 `CopyTextureRegion`；
       - 若尾片之后无多余填充空间（`!fullFootprintFits`）：先以 `Footprint.Height = imageRows * block.BlockHeight`、`Footprint.Depth = Source.Depth - 1` 拷贝前 `Source.Depth - 1` 片；再对尾片计算 `lastOffset = DestinationOffset + uint64_t{Source.Depth - 1} * imageRows * RowPitch` 与对齐修正行 `prefixRows = lastOffset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT ? 1 : 0`，令 `PlacedFootprint.Offset = lastOffset - prefixRows * RowPitch`、`Footprint.Height = (prefixRows + rows) * block.BlockHeight`、`Footprint.Depth = 1`，以 `DstY = prefixRows * block.BlockHeight` 单独拷贝第 `Z + Source.Depth - 1` 片；
   - **Vulkan**：按块尺寸换算纹素单位的 `bufferRowLength` 与 `bufferImageHeight = imageRows * block.BlockHeight`，填充 `VkBufferImageCopy` 的 `bufferOffset = DestinationOffset`、`imageOffset = {X, Y, Z}`、`imageExtent = {Width, Height, Depth}`，调用 `vkCmdCopyImageToBuffer`；
   - 同步更新 `modules/runtime/tests/test_fakes.h` 假 `CommandBuffer` 覆写声明。

4. **单平面深度格式整子资源 `CopyTextureToTexture` 补齐（[`rhi_validation.cpp`](../../modules/gpu_impl/src/rhi_validation.cpp) / [`d3d12_impl.cpp`](../../modules/gpu_impl/src/d3d12/d3d12_impl.cpp) / [`vulkan_impl.cpp`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp)）**
   - 顺应 D3D12 硬件对深度资源 `CopyTextureRegion` 要求 `pSrcBox == nullptr` 且含模板格式分多平面存储的约束，在 `CommandBufferValidation::CopyTextureToTexture` 放开无模板平面的单平面深度格式（`TextureFormat::D16_UNORM`、`TextureFormat::D32_FLOAT`）**整子资源**拷贝：当源与目标同为单平面深度格式时，要求起点 `(SourceX, SourceY, SourceZ)` 与 `(DestinationX, DestinationY, DestinationZ)` 全为 `0`，且 `(Width, Height, Depth)` 同时等于源与目标对应 mip 的完整尺寸（局部子区域深度拷贝及带模板的双平面 `D24_UNORM_S8_UINT` / `D32_FLOAT_S8_UINT` 继续拒绝）；
   - Vulkan 校验包装从命令池所属队列族读取 `queueFlags`。当 `VK_QUEUE_GRAPHICS_BIT` 未置位且拷贝深度 aspect 时，源和目标的 `SampleCount` 均须为 `1`。即使两端采样数相同且整幅拷贝，`4× MSAA` 也须在录制 `vkCmdCopyImage` 前由 `Context->Reject` 拒绝。具备图形能力的队列继续按格式、采样数相等与整子资源条件校验。[Vulkan 规范 VUID 12449](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImage.html#VUID-vkCmdCopyImage-commandBuffer-12449)
   - `CmdListD3D12::CopyTextureToTexture`：当源为多采样或满足整子资源拷贝（含单平面深度格式）时，向 `ID3D12GraphicsCommandList::CopyTextureRegion` 传入 `nullptr` 作为 `pSrcBox`；
   - `CommandBufferVulkan::CopyTextureToTexture`：将源与目标 `subresource.aspectMask` 从硬编码 `VK_IMAGE_ASPECT_COLOR_BIT` 改为按纹理格式选择 `VK_IMAGE_ASPECT_DEPTH_BIT` 或 `VK_IMAGE_ASPECT_COLOR_BIT`。

5. **`AsyncReadbackQueue` 子区域读回扩展与既有整 mip 重载契约保持（[`readback_queue.h`](../../modules/runtime/include/radray/runtime/readback_queue.h) / [`readback_queue.cpp`](../../modules/runtime/src/readback_queue.cpp)）**
   - 新增显式接收源 `gpu::TextureRegion2D` 的 `EnqueueReserved` 与 `EnqueueTexture` 重载：校验源为颜色格式且与已预留 `plan.Format` 匹配、`sourceRegion` 的 `(Width, Height, Depth)` 等于 `plan` 且完整落在源纹理指定 mip 边界内，内部改用 `CopyTextureToBufferRegion` 录制并在 `Poll` 中完成 `InvalidateMappedRange` 与紧凑去填充拷贝；
   - **严格保留既有不带区域的 `EnqueueReserved(graph, id, source, mipLevel, arrayLayer)` 整子资源尺寸校验语义**：先由 `SourcePlan(graph, source, mipLevel, arrayLayer)` 推导源 mip 的完整尺寸，**若 `!sourcePlan || *sourcePlan != request.Info.Texture`（例如源为 `7 × 3` 而仅预留 `6 × 3`）直接返回 `false`**，校验通过后再以起点 `(0, 0, 0)` 与 `sourcePlan` 完整宽高深委托至区域重载，确保既有整 mip 回归用例不退化，仅显式传入 `TextureRegion2D` 的新重载接受局部子矩形请求。

---

### T5：RHI 与 Runtime 设备丢失（`DeviceLost`）单向终态、防伪造完成度与免等待安全析构

核心边界：**严格区分“调用方契约违约 / 内部不变量破坏”（继续 `RADRAY_ABORT`）与“外部硬件/驱动设备丢失”（单向锁存至 `Device` 终态，不伪造 GPU 完成进度、不阻塞死等无法完成的 Fence、保障全套子对象无泄漏析构）。**

1. **`gpu::Device::IsDeviceLost()` 与双后端单向终态锁存（[`types.h`](../../modules/gpu/include/radray/gpu/types.h) / [`rhi_validation.cpp`](../../modules/gpu_impl/src/rhi_validation.cpp) / [`d3d12_impl.h`](../../modules/gpu_impl/include/radray/gpu_impl/backend/d3d12_impl.h) / [`d3d12_impl.cpp`](../../modules/gpu_impl/src/d3d12/d3d12_impl.cpp) / [`vulkan_impl.h`](../../modules/gpu_impl/include/radray/gpu_impl/backend/vulkan_impl.h) / [`vulkan_impl.cpp`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp)）**
   - 在 `gpu::Device` 声明纯虚成员函数 `virtual bool IsDeviceLost() const noexcept = 0;`（非内联覆写实现均落入对应 `.cpp`，`DeviceValidation` 直接转发 `Real()->IsDeviceLost()`）；
   - `DeviceD3D12` 与 `DeviceVulkan` 各自持有 `std::atomic<bool> _deviceLost{false}` 与内部锁存函数 `MarkDeviceLost`（覆盖队列/围栏/交换链、资源与对象创建 `CreateBuffer` / `CreateTexture` / `CreateCommandBuffer` / `CreateCommandAllocator` / `CreateFence` / 管线创建以及命令缓冲 `Begin` / `End` 重置与闭合点）：
     - 仅识别真实设备丢失错误码——Vulkan 为 `VK_ERROR_DEVICE_LOST`；D3D12 为 `DXGI_ERROR_DEVICE_REMOVED`、`DXGI_ERROR_DEVICE_RESET`、`DXGI_ERROR_DEVICE_HUNG`、`DXGI_ERROR_DRIVER_INTERNAL_ERROR` 或失败时 `ID3D12Device::GetDeviceRemovedReason() != S_OK`（`Create*` 检出设备丢失时锁存状态并返回 `nullptr`；`CmdListD3D12::Begin/End` 与 `CommandBufferVulkan::Begin/End` 检出设备丢失时锁存状态并直接返回而不触发 `RADRAY_ABORT`，非设备丢失错误继续维持既有行为）；
     - 首次由 `false` 翻转为 `true` 时，D3D12 立即刷出 DRED / InfoQueue 诊断并输出单条含操作名、原 `HRESULT` 及 `GetDeviceRemovedReason()` 的 `RADRAY_ERR_LOG`；**Vulkan `MarkDeviceLost(..., bool deviceIdleAlreadyCalled = false)` 输出单条含操作名与 `VkResult` 的 `RADRAY_ERR_LOG`，且当 `!deviceIdleAlreadyCalled` 时在 `MarkDeviceLost` 内当场执行唯一一次 `_ftb.vkDeviceWaitIdle(_device)`（接受 `VK_SUCCESS` 或 `VK_ERROR_DEVICE_LOST` 返回值而不 `RADRAY_ABORT`）**，后续重复检出不再刷屏且绝不调用 `RADRAY_ABORT`。将 Vulkan 宿主收尾同步直接绑定在 `MarkDeviceLost` 首次翻转处，既严格保证无交换链场景下 `FrameScheduler::BeginFrame() -> WaitSlot()` 提前返回时原生 `vkDeviceWaitIdle` 已早于 `ResetCommandStorage()` / `ResetFrameData()` 首次析构槽内 GPU 对象完成，又避免 `SwapChainVulkan::DrainAndClear` 自身 `vkDeviceWaitIdle` 首次检出丢失时重复调用第二次 `vkDeviceWaitIdle`。

2. **`gpu::CommandQueue::Submit -> bool`（保留 RHI 校验返回值，维持健康设备 OOM Fail-Fast）与丢失态免重复等待 `Wait()`**
   - 将 `gpu::CommandQueue::Submit` 签名调整为 `virtual bool Submit(const CommandQueueSubmitDescriptor& desc) noexcept = 0;`（同步更新 `QueueValidation`、`CmdQueueD3D12`、`QueueVulkan`、示例与测试假对象）：
     - `QueueVulkan::Submit`：入口若 `_device->IsDeviceLost()` 直接返回 `false`；修正原实现先推进 `fenceObj->_fenceValue` 再调 `vkQueueSubmit` 的顺序问题——**仅在 `vkQueueSubmit == VK_SUCCESS` 后才更新 `fenceObj->_fenceValue`**；若 `vkQueueSubmit` 返回 `VK_ERROR_DEVICE_LOST` 则调用 `_device->MarkDeviceLost`（内部已完成单次 `vkDeviceWaitIdle`）并返回 `false`；**非设备丢失的 `vkQueueSubmit` 内存耗尽错误（与 `RenderGraphFramePipeline::Prepare / Record` 瞬态分配失败一致）维持既有 fail-fast `RADRAY_ABORT` 契约**；
     - `CmdQueueD3D12::Submit`：入口及 `Wait` / `ExecuteCommandLists` / `Signal` 检出设备移除时锁存 `_device->MarkDeviceLost` 并返回 `false`（其余非设备移除错误维持 `_CheckD3D12Result` fail-fast）；
     - `QueueValidation::Submit`：保留 RHI 校验不通过返回 `false` 的可测试契约；仅当 `Real()->Submit(converted)` 返回 `true` 时才将各命令缓冲状态置为 `RecordingState::Submitted`（底层因设备丢失返回 `false` 时保持 `RecordingState::Executable`，供后续清理阶段经 `CommandAllocator::Reset()` 释放校验引用）；
   - `CommandQueue::Wait()` 与设备析构句柄清理：
     - `QueueVulkan::Wait()`：入口若 `_device->IsDeviceLost()`（`MarkDeviceLost` 已完成过原生 `vkDeviceWaitIdle`）直接返回；若 `vkQueueWaitIdle == VK_ERROR_DEVICE_LOST` 则调用 `_device->MarkDeviceLost` 后返回；`DeviceVulkan::DestroyImpl()` 在 `_device->IsDeviceLost()` 时同样无需二次等待，**继续按既定顺序完整释放 `_renderPassCache`、`_descriptorSetAllocator`、`_samplerCache`、`_vma`、队列与 `vkDestroyDevice`**；
     - `CmdQueueD3D12::Wait()`：在 `_device->IsDeviceLost()` 或 `Signal` / `Wait` 检出设备移除时锁存 `MarkDeviceLost` 并直接返回。

3. **`gpu::Fence` 防伪造完成度与有界等待 / 停机排空分流**
   - **`GetCompletedValue()` 严禁透出 `UINT64_MAX` 假完成值**：
     - `FenceD3D12` 与 `TimelineSemaphoreVulkan` 维护 `std::atomic<uint64_t> _lastCompletedValue{0}`；每次原生查询成功且未丢失（D3D12 返回值 `< std::numeric_limits<uint64_t>::max()`，Vulkan `vkGetSemaphoreCounterValue == VK_SUCCESS`）时单调更新缓存并返回；
     - 一旦 D3D12 返回 `UINT64_MAX` 设备移除哨兵或 Vulkan 返回 `VK_ERROR_DEVICE_LOST`，调用 `_device->MarkDeviceLost` 并返回丢失前缓存的 `_lastCompletedValue`——绝不让 `AsyncReadbackQueue::Poll` 或上层轮询误判 `completed >= target` 而映射读取尚未完成的暂存缓冲；
   - **`WaitFor(value, timeoutMs)` 与 `Wait(value)` / `Wait()`**：
     - `WaitFor`：入口若 `_device->IsDeviceLost()` 立即返回 `false`；等待期间若 `vkWaitSemaphores == VK_ERROR_DEVICE_LOST` 或 D3D12 `SetEventOnCompletion` / `WaitForSingleObject` / `GetCompletedValue` 检出设备移除，锁存 `MarkDeviceLost` 并立即返回 `false`（调用方可通过 `device.IsDeviceLost()` 区分常规超时与设备丢失）；
     - `Wait`（供内部停机排空与槽回收使用）：内部调用 `WaitFor(value, 30000)`；若返回 `false` 且 `_device->IsDeviceLost()`，直接正常返回而不触发 `RADRAY_ABORT`（仅在设备未丢失且真实卡死超 30 秒时维持 `RADRAY_ABORT`）。

4. **`CommandBuffer` / `CommandAllocator` 录制中途丢失态逻辑闭合与 `SwapChain` 丢失态闭合**
   - **校验包装录制状态在 `DeviceLost` 下的闭合（防止留下 `Open == true` 或 `Recording` 子对象导致 `Reset` 拒绝与析构 `Fatal`）**：
     - `CommandBufferValidation::Begin()`：在调用 `Real()->Begin()` 前后均检查 `Context->NativeDevice->IsDeviceLost()`；一旦设备已丢失，**直接返回而不置位 `Allocator->RecordingCommand`、`State = RecordingState::Recording` 与 `Open = true`**；
     - `CommandBufferValidation::End()`：当 `Context->NativeDevice->IsDeviceLost()` 为真时，豁免未配平 `DebugDepth` 检查并清零 `DebugDepth`，复位 `Allocator->RecordingCommand = nullptr`、`State = RecordingState::Executable` 与 `Open = false`；
     - `CommandAllocatorValidation::Reset()` 与 `CommandBufferValidation::ResetLogical()`：当 `Context->NativeDevice->IsDeviceLost()` 为真时，**允许重置因录制中途丢失而停留在 `RecordingState::Recording` 的命令缓冲**（前提无未退出的活跃 Encoder 栈对象），清空 `RecordingCommand = nullptr`，并在 `ResetLogical()` 中一并复位 `Open = false`、`Active = nullptr`、`DebugDepth = 0`、`State = RecordingState::Initial` 及 `EpochReferences`，保证 `WaitIdle() -> ResetCommandStorage()` 与后续 `CommandBuffer` / `Device` 析构零 `Reject`、零 `Fatal`；
   - `CommandAllocatorVulkan::Reset()`：仅在 `_used && !_device->IsDeviceLost()` 时调用 `vkResetCommandPool`（若返回 `VK_ERROR_DEVICE_LOST` 锁存 `MarkDeviceLost` 而不 abort）；**无论设备是否丢失，始终清除 `_used` 并遍历 `_children` 执行 `ReleaseRenderScopeReferences()`**，确保设备销毁时 `VulkanRenderPassCache::Clear()` 的 `_viewCommandUses` 与活跃计数严格归零，不会误触残留引用 `RADRAY_ABORT`；`CommandAllocatorD3D12::Reset()` 在 `!_device->IsDeviceLost()` 时调用 `_native->Reset()`，检出设备移除时锁存状态而不 abort；
   - `SwapChainD3D12::Present` / `Recreate`：检出设备移除时锁存 `MarkDeviceLost`，分别返回 `SwapChainStatus::Error`（`NativeStatusCode` 保留原 `HRESULT`）与 `false`；
   - `SwapChainVulkan::RecordFailure`：支持透传 `bool deviceIdleAlreadyCalled = false`，当 `result == VK_ERROR_DEVICE_LOST` 时同步调用 `_device->MarkDeviceLost(..., deviceIdleAlreadyCalled)`；`SwapChainVulkan::DrainAndClear` 在入口尚未丢失时执行的那次 `vkDeviceWaitIdle` 若首次返回 `VK_ERROR_DEVICE_LOST`，以 `deviceIdleAlreadyCalled = true` 传入 `RecordFailure` 复用本次设备级等待（确保全程只调用一次 `vkDeviceWaitIdle`），而在入口 `_device->IsDeviceLost()` 已为真时直接跳过冗余的 `vkDeviceWaitIdle` 与 `_pendingAcquires` 围栏等待循环，**两条分支随后均继续完整清空 `_outstandingAcquire` / `_pendingAcquires` / `_frames` 并对 `_retiredGenerations` 与当前 `_swapchain` 调用 `vkDestroySwapchainKHR` 释放全部原生句柄**；`SwapChainVulkan::DiscardAcquiredFrame` 在健康设备上维持 `RADRAY_ABORT`，但**当 `_device->IsDeviceLost()`（或 `_terminalResult == VK_ERROR_DEVICE_LOST`）时清理 `_outstandingAcquire`、失效 `frame` 并返回 `SwapChainStatus::Error`**，保证“已通过 `AcquireNext` 取帧但随后 `Prepare / Record / Submit` 因设备丢失失败”的场景可合法注销 `SwapChainValidation` 的未消费令牌，后续 `SwapChain::Destroy()` 零验证层报错。

5. **Runtime 多槽调度与上下文析构传导（[`frame_pipeline.h`](../../modules/runtime/include/radray/runtime/frame_pipeline.h) / [`frame_pipeline.cpp`](../../modules/runtime/src/frame_pipeline.cpp) / [`render_graph_pipeline.h`](../../modules/runtime/include/radray/runtime/render_graph_pipeline.h) / [`render_graph_pipeline.cpp`](../../modules/runtime/src/render_graph_pipeline.cpp) / [`frame_scheduler.h`](../../modules/runtime/include/radray/runtime/frame_scheduler.h) / [`frame_scheduler.cpp`](../../modules/runtime/src/frame_scheduler.cpp) / [`gpu_device.cpp`](../../modules/runtime/src/gpu_device.cpp)）**
   - 保持 `Extract / Prepare / Record` 的 `void noexcept` 签名与健康设备 OOM fail-fast 契约不变，同时打通 `Prepare / Record` 阶段首次检出 `DeviceLost` 的非 `RADRAY_ABORT` 传导：
     - `RenderGraphFramePipeline::Prepare(FrameSlot&)`：严格区分**必需资源**与**可选 GPU 计时资源**——若入口 `slot.GetDevice().IsDeviceLost()`，或必需阶段 `CompiledGraph->Prepare(...)`（如首次创建瞬态纹理/缓冲触发 `DXGI_ERROR_DEVICE_REMOVED` / `VK_ERROR_DEVICE_LOST`）、`slot.AcquireCommandBuffer()` 失败且 `slot.GetDevice().IsDeviceLost() == true`，复位 `state.Profiling`、`state.CompiledGraph` 与 `state.Command` 并直接返回而不调用 `RADRAY_ABORT`（当 `!slot.GetDevice().IsDeviceLost()` 时，必需资源失败维持既有 `RADRAY_ABORT`）；**而可选阶段 `state.PrepareTiming(slot)` 若在健康设备（`!IsDeviceLost()`）上遭遇查询池或读回缓冲分配失败，继续保持既有 `RADRAY_WARN_LOG` + `QueryCount = 0` 降级出帧契约而不 abort**，仅当 `PrepareTiming` 内部检出 `slot.GetDevice().IsDeviceLost() == true` 时按设备丢失复位状态并提前返回；
     - `RenderGraphFramePipeline::Record(FrameSlot&)`：入口若 `slot.GetDevice().IsDeviceLost()` 直接返回；`Command->Begin()`、`CompiledGraph->Record(...)` 或 `Command->End()`（如 D3D12 `ID3D12GraphicsCommandList::Close` 报告设备移除）后若 `slot.GetDevice().IsDeviceLost() == true`（或 `Record` 返回 `false` 且 `IsDeviceLost() == true`），若命令缓冲已进入录制态则调用丢失态安全的 `Command->End()`（或交由后续 `CommandAllocator::Reset()` 逻辑收尾），复位借用状态并直接返回而不调用 `RADRAY_ABORT`（健康设备失败维持 `RADRAY_ABORT`）；
   - 将 `FramePipeline::Submit(FrameSlot&)`、`RenderGraphFramePipeline::Submit(FrameSlot&)`、`RenderGraphFramePipeline::SubmitFrame(FrameSlot&, gpu::CommandBuffer&)` 与 `FrameScheduler::RunFrame()` 返回类型调整为 `bool`（声明与实现严格分离在同名 `.h`/`.cpp` 中，同步更新示例与单测覆写）：
     - `RenderGraphFramePipeline::Submit`：调用 `SubmitFrame`；无论成败均复位 `Profiling`、`CompiledGraph` 与 `Command` 借用指针，**仅当返回 `true` 时**更新 `ReadbackState = gpu::BufferState::HostRead` 并调用 `CommitSubmittedStates()`；
     - `FrameScheduler::RunFrame()`：`BeginFrame()` 后若 `slot.GetDevice().IsDeviceLost()` 立即返回 `false`；依次执行 `Extract`、`Prepare`、`Record`（当 `slot.GetDevice().IsDeviceLost()` 已翻转为真时跳过后续阶段与 `_pipeline->Submit(slot)`，直接按设备丢失收尾返回 `false`）；若进入 `_pipeline->Submit(slot)` 且返回 `false`，**用 `slot.GetDevice().IsDeviceLost()` 严格区分设备丢失与健康设备校验违约——若 `!slot.GetDevice().IsDeviceLost()`（例如健康设备上触发 `QueueValidation::Submit` 校验拒绝），立即在 `FrameScheduler::RunFrame` 原地调用 `RADRAY_ABORT` fail-fast 中止，绝不把健康设备内部提交违约伪装成 `DeviceLost` 软返回**；凡因 `slot.GetDevice().IsDeviceLost() == true` 提前结束时，统一保持 `slot._inFlight = false` 与 `_slotBegun = true`（不递增 `_nextFrameId`、不轮转 `_currentSlotIndex`，交由后续 `WaitIdle()` 按序重置命令存储并清理槽内 `Emplace` 对象），复位 `_running = false` 并返回 `false`；
   - `FrameScheduler::WaitSlot(FrameSlot&)`：`_pipeline->Wait(slot)` 返回后，**仅当 `!slot.GetDevice().IsDeviceLost()` 时才调用 `_pipeline->OnSlotCompleted(slot)`**（防止设备丢失后映射读取未完成的 GPU 剖析时间戳），随后置 `slot._inFlight = false`；`DeviceContext::~DeviceContext()` 调用 `_directQueue->Wait()` 在丢失态下免等待安全返回并释放 `_device`。

---

## 三、测试与验收门禁

1. **校验与双后端真设备测试（`test_radray_gpu_impl`，D3D12 + Vulkan）**
   - **`ValidateTextureReadbackRegion` 单元测试**（因 `validation.cpp` 归属 `gpu_impl`，直接单测统一放入 `modules/gpu_impl/tests/`）：覆盖正常路径与全部边界负例（`TextureFormat::UNKNOWN` 前置拒绝且不触发除零、零尺寸、越界、深度/模板格式整幅与子区域拒绝、未对齐设备 `DestinationOffset` / `RowPitch`、在设备对齐限额设为 `1` 的假设备上输入 `BC1` 且 `RowPitch = 9` 触发非 `BytesPerBlock` 倍数拒绝、纹素换算步长溢出 `uint32_t`、`RowsPerImage < rows`、末片最小容量与差 1 字节溢出、BCn 非块对齐起点、缺少 `CopySource` / `CopyDestination` 用途）；
   - **局部 `RenderArea` 清屏**：同一颜色+深度纹理先整幅清为颜色 A / 深度 `1.0`，再开启局部 `RenderArea` 清为颜色 B / 深度 `0.25`，读回整幅断言矩形内为 B、矩形外严格保留 A；
   - **`CopyTextureToBufferRegion`（含非 512B 对齐尾片 `prefixRows = 1` 分支）**：
     - 2D 非零 mip 与 BCn 纹理从非零 `(X, Y)` 读回窄矩形至非零对齐 `DestinationOffset`，校验目标字节与外围哨兵字节；
     - `Dim3D` 纹理采用 **`Depth = 2`、每片拷贝 `2` 行、`RowPitch = 256`、`RowsPerImage = 3`**（使第 1 片起始偏移为 `3 * 256 = 768 B`，满足 `768 % 512 != 0`），同时覆盖目标缓冲带尾部填充与仅分配至末片最后一行紧凑容量两种情形，预填哨兵字节后断言第 0 片落在第 `0..1` 行、第 `2` 行填充区哨兵完好未被改写、第 1 片准确落在第 `3..4` 行且精确命中 `prefixRows = 1` 分支；
     - **Vulkan 队列族 `minImageTransferGranularity`（含 BCn 块乘积换算）与纯传输队列 4 字节偏移校验**：以可控队列族能力验证：① 同一 `8 × 8` RGBA8 纹理的局部子区域 `(1, 1, 2, 2)` 在粒度 `(1, 1, 1)` 的 `Direct` 队列上返回 `true`，在粒度 `(0, 0, 0)` 的整幅专用 `Copy` 队列上返回 `false`；② 在 `X` 轴粒度为 `2` 的队列上对 `16 × 16` `BC1_UNORM`（`BlockWidth = 4`，有效纹素步长 `2 × 4 = 8`）双向测试上传与读回，成对断言起点 `X = 4` 被拒绝返回 `false`、起点 `X = 8`（以及贴合 mip 末端的尾块宽度）放行返回 `true`；③ 在缺少 `GRAPHICS | COMPUTE` 能力的纯传输队列且设备 `TextureDataPlacementAlignment = 1` 的条件下，对 `R8_UNORM` 整幅 mip 双向测试上传与读回，成对断言 `bufferOffset = 1` 被拒绝返回 `false`、`bufferOffset = 4` 放行返回 `true`；
   - **单平面深度整子资源 `CopyTextureToTexture`**：在 D3D12 与 Vulkan 真设备上将已写入梯度深度的源 `D32_FLOAT` 纹理整幅 `CopyTextureToTexture` 至目标 `D32_FLOAT` 纹理，随后将目标纹理作为深度附件执行 `DepthCompare::Less` 光栅绘制并读回颜色附件，断言拷贝后的深度遮挡结果精确生效且零验证层报错；同时覆盖深度局部子矩形拷贝与含模板 `D24_UNORM_S8_UINT` / `D32_FLOAT_S8_UINT` 纹理间拷贝的校验拒绝负例；
   - 在可控 Vulkan 队列族上，以两张同尺寸 `D32_FLOAT` 纹理录制整子资源拷贝。对缺少 `VK_QUEUE_GRAPHICS_BIT` 的队列，断言两端均为 `4× MSAA` 时触发校验拒绝且原生命令计数为零，两端均为单采样时向后端转发一次。
   - **设备丢失 RHI 校验层与收尾同步行为**：通过测试侧包装后端（不在生产类中添加任何仅供测试的注入字段）启用真实 RHI 校验层包装，验证：① **分别在 `CommandBuffer::Begin()` 当下及 `Begin()` 成功后的录制中途注入设备丢失，执行 `CommandAllocator::Reset()` 与对象/设备析构时不会触发 `allocator has an open recording` 拒绝或 `live child objects remain` 致命中止**；② `QueueValidation::Submit` 在校验拒绝与底层设备丢失时返回 `false` 且保持命令缓冲为 `Executable` 态并允许 `CommandAllocator::Reset()` 正常清理 epoch 引用；③ 首次锁存设备丢失时当场完成原生 idle 同步并在 `SwapChain::Destroy()` / `Device` 析构中继续完整释放全部原生句柄，且**从 `SwapChain::Destroy() -> DrainAndClear()` 首次检出设备丢失时复用该次设备级等待（断言原生 `device idle` 全程仅调用一次且后续全部交换链/设备句柄正常销毁）**；④ 底层 `IsDeviceLost() == true` 时 `SwapChain::DiscardAcquiredFrame` 可注销已获取帧令牌，业务 `Fence::GetCompletedValue()` 始终严格维持丢失前真实完成值。

2. **Runtime 与 RDG 测试（`test_radray_runtime`，含 Fake RHI 与双后端 GPU 测试）**
   - **局部写保活与有符号 `RenderArea` 校验**：覆盖负坐标 `Rect{-1, 0, ...}` 在 `Compile` 期拒绝；验证同一 `RDGTextureHandle`“整幅清红 → 子矩形清蓝”及多 mip / 多层纹理逐子资源清不同颜色且仅 `MarkOutput` 最终纹理时前序 Pass 未被裁掉且双后端读回像素完整保留；验证不同 `RenderArea` 相邻 Pass 不被 `CanMerge` 合并；
   - **顺序边、区间顺序屏障与外部查询池生命周期**：
     - 构造 `Begin -> Raster A -> 独立 Copy B -> End -> 独立 Copy C`，对 `Begin` 与 `End` 标 `NeverCull()` 并调 `AddOrderBarrier`，直接断言 `GetRecordedPassNames()` 实际调度顺序满足 `Begin < {A, B} < End < C`；同时分别测试 ① 两个存活屏障之间的普通节点全部被裁剪时仍保持 `Begin < End`，以及 ② `A`（存活光栅屏障）→ `B`（无输出可裁剪屏障）→ `C`（存活独立 `CopyPass`）时断言 `B` 被裁剪且 `GetRecordedPassNames()` 严格满足 `A < C`；
     - 在双后端真设备上通过首尾 `NeverCull() + AddOrderBarrier` 的 `CopyPass` 执行外部 `QueryPool` 的 `ResetQueryPool` 与 `ResolveQueryData`：同时验证 ① “含内嵌 `WriteTimestamp` 且标 `NeverCull() + AddOrderBarrier` 的 `Raster A` → 存活独立 `Copy B`”在 `GetRecordedPassNames()` 中严格满足 `A < B`，以及 ② “含内嵌 `WriteTimestamp` 的 `Raster A` → 后继整幅清屏覆盖同一附件的 `Raster B` → 仅 `MarkOutput` 最终附件”场景下查询结果在双后端合法可读且零验证层报错；
     - 断言越界句柄、未初始化句柄（`UINT32_MAX`）、自依赖、逆声明序，以及在已声明后续依赖节点 `A -> B` 后对旧节点调用 `AddOrderBarrier(A)` 或连续重复调用 `AddOrderBarrier(B)` 在 `Compile` 期均安全失败返回空；验证先声明节点 `A` 再向非动态缓冲执行零字节合法空操作 `UploadBuffer` 时返回无效 `RDGPassHandle`（`IsValid() == false`）且编译结果仅含节点 `A`；
   - **`AsyncReadbackQueue` 子区域读回与旧重载整 mip 回归保护**：验证大图非零偏移子矩形读回数据、四态流转与越界/深度格式请求拒绝及 `CancelUnsubmitted` 回退；**保留 `7 × 3` 源纹理预留 `6 × 3` 时调用不带区域的 `EnqueueReserved` 返回 `false` 的既有负例，并成对验证显式传入 `TextureRegion2D{..., 6, 3, 1}` 的新重载返回 `true` 且读回正确**；
   - **`DeviceLost` 单向终态、`Prepare/Record` 丢失传导、可选计时降级与健康设备违约分流（基于 `test_fakes.h` 假设备模拟并覆盖挂载真实校验包装层）**：覆盖在无交换链但已有在途帧槽的场景下于 `BeginFrame -> WaitSlot`、`RunFrame -> Prepare`（首次创建瞬态纹理 `CreateTexture` 触发设备丢失）、**启用真实校验层包装时 `RunFrame -> Record`（分别在 `CommandBuffer::Begin` 与 `Begin` 成功后的图录制中途触发设备丢失）**、`RunFrame -> Submit` 及 `Fence::GetCompletedValue / WaitFor` 阶段触发 `IsDeviceLost() == true` 的完整路径，断言：① **在 `BeginFrame -> WaitSlot` 检出丢失并紧接回收帧槽时，事件顺序严格满足“检出丢失 → 原生 idle 同步 → 首个槽内 `Emplace` GPU 对象销毁”，同时业务 `Fence::GetCompletedValue()` 维持丢失前真实已完成值而不跳变为 `UINT64_MAX`**，未完成的 `AsyncReadbackQueue` 请求轮询保持 `Ready == false` 不读取未初始化暂存内存；② `WaitSlot` 跳过 `OnSlotCompleted`，不触发未完成 GPU 时间戳映射读取；③ **在 `Prepare`、`Record`（含录制中途打开命令缓冲态）或 `Submit` 阶段触发 `IsDeviceLost() == true` 时均不触发 `RADRAY_ABORT`、`RunFrame()` 正常返回 `false` 且不推进持久资源终态**，随后执行 `FrameScheduler::WaitIdle()`、调度器析构与 `DeviceContext` 析构全程零阻塞、零 `RADRAY_ABORT`、零校验层残留对象报错，槽内登记的 `Emplace` 对象按从旧到新顺序完整析构；④ **在健康设备（`IsDeviceLost() == false`）上开启 GPU profiling 并仅让 `PrepareTiming` 的查询池/读回缓冲分配失败时，该帧记录 `WARN` 后正常提交成功（返回 `true`）且 `GetCompletedGpuTiming` 为空；而当必需的 `CreateTexture` 返回空或 `Submit` 因校验拒绝返回 `false` 时（死亡测试），在 `Prepare` 或 `FrameScheduler::RunFrame()` 原处触发 fail-fast `RADRAY_ABORT` 而不会返回 `false`**。

3. **工程规范与文档门禁**
   - 遵守 `.h` / `.cpp` 同名一一对应、非模板成员函数实现落入 `.cpp`、零新增全局变量/异常；
   - `ctest` 全套通过后同步更新 `docs/architecture/render-rhi.md` 与 `docs/architecture/runtime.md`，执行 `python tools/check_docs.py` 与 `git diff --check`。

---

## 四、后续阶段（阶段三至阶段五）引擎侧前置架构约束

1. **阶段三（MDI 间接绘制合批与多 Pass 可见性复用）**：`MeshDrawCommand` MDI 间接参数构建与 `UploadArena` 切片写入必须在 RDG `Prepare` 阶段完成（严禁在只读 `Record` 阶段分配暂存内存），支持同一可见性排序与 MDI 参数流跨同帧多个 Pass 共享复用；几何池支持按需开启 `BufferUse::Resource`（`ShaderRead`）与加速结构构建输入用途位。
2. **阶段四（Bindless 资源表与两阶段 Hi-Z GPU 剔除）**：`BindlessResourceRegistry` 优先基于 D3D12 SM 6.0 无界描述符表（复用百万级 Shader-Visible 资源堆）与 Vulkan 1.2 核心 Descriptor Indexing（`PARTIALLY_BOUND | UPDATE_AFTER_BIND`），配合 Two-Phase Reverse-Z `MIN` 深度金字塔剔除。
3. **阶段五（跨队列异步拷贝、Execution Group 并行录制与硬件光线追踪）**：跨队列传输严格区分 `COPY` 与 `DIRECT`/`COMPUTE` 队列交接边界的 `COMMON` 状态、拷贝命令列表执行期的隐式状态提升（`COMMON -> COPY_DEST / COPY_SOURCE`）与提交结束自动衰减回 `COMMON` 规则，通过队列 Fence 移交所有权并在非独立物理拷贝队列（`Dedicated == false`）上自动旁路；多线程命令录制以 RDG Execution Group 多 `CommandBuffer` 为粒度，以实测 CPU `Record` 耗时为启用门禁。
