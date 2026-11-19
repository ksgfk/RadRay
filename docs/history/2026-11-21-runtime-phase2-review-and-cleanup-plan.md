> - 适用: 审视阶段二（`09806ee`..`c6d2e85`）实施中的问题处理、高层架构接口成因与 Corner Case 来源，并按收敛后的计划清理过度防御代码与状态复位遗漏
> - 权威: 经用户要求保存并修订的复盘与迭代计划；不作为当前已实现契约，当前契约见 [RHI 与后端](../architecture/render-rhi.md) 与 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `docs/history/2026-11-21-runtime-phase2-implementation-notes.md`, `modules/gpu_impl/include/radray/gpu_impl/backend/vulkan_impl.h`, `modules/gpu_impl/src/d3d12/d3d12_impl.cpp`, `modules/gpu_impl/src/vk/vulkan_impl.cpp`, `modules/gpu_impl/src/vk/vulkan_helper.cpp`, `modules/gpu_impl/src/rhi_validation.cpp`, `modules/runtime/include/radray/runtime/rdg.h`, `modules/runtime/src/rdg.cpp`, `modules/runtime/src/frame_scheduler.cpp`, `modules/runtime/src/render_graph_pipeline.cpp`

# Runtime 与 RHI 阶段二实施复盘、架构成因与过度防御清理计划

## 状态、基线与范围

- 记录日期：`2026-11-21`（含同日审阅修订）
- 基线：阶段二实施完成后的 `develop` 分支提交 `c6d2e85`（涵盖 `09806ee`、`67b6c8a`、`73016d4`、`c6d2e85`）及实施记录 [2026-11-21-runtime-phase2-implementation-notes.md](2026-11-21-runtime-phase2-implementation-notes.md)
- 状态：经审阅收紧边界后的代码收敛迭代计划；待按项执行，归档时未改动生产代码
- 影响范围：
  - `RadRay::gpu_impl`：在保持 `Fence::WaitFor` 丢失态恒返回 `false` 契约的前提下，精简 `FenceD3D12::WaitFor` 冗余分支并消除 `FenceVulkan::WaitFor` 已完成路径上的二次 `GetCompletedValue()` 调用与零超时 `vkWaitSemaphores` 调用；移除 `DeviceVulkan` 中脱离 RHI 单线程串行契约的 `_lostIdleComplete` 双原子阻塞等待（保留首次丢失时的单次 `vkDeviceWaitIdle` 与返回码校验）；
  - `RadRay::runtime`：修复 `FrameScheduler::RunFrame()` 在两条 `DeviceLost` 提前返回分支遗漏复位 `#if RADRAY_IS_DEBUG` 下 `slot._inFrameCallback = false` 的缺陷；收敛 `RenderGraphFramePipeline::Record` 在 `Begin()` 成功但图录制中途丢失时的退出路径，图返回时若已锁存丢失，跳过帧尾时间戳、查询解析和读回屏障，配对调用逻辑 `Command->End()` 与末尾 `AbandonIfLost` 收尾。

---

## 一、阶段二实施四项问题的复盘与架构归因

对 [2026-11-21-runtime-phase2-implementation-notes.md](2026-11-21-runtime-phase2-implementation-notes.md) 记录的四处实施问题及 `DeviceLost` Corner Case 膨胀原因甄别如下：

### 1. 局部附件视图的准备与合并及组合深度模板 Aspect 校验

- **出现的问题**：
  1. `CompiledRDG::Prepare` 原有两处独立的默认视图查找：首轮遍历 `Accesses` 预创建默认 mip 0 视图，随后组装物理光栅 Group 时再次按默认 mip 0 查找，导致声明了非零 mip 的光栅附件仍绑定 mip 0 视图；
  2. 通用 `gpu::NormalizeSubresourceRange` 允许传入单一 `TextureAspect::Depth` 或 `Stencil`，而 RHI 创建组合格式（`D24_UNORM_S8_UINT`、`D32_FLOAT_S8_UINT`）的深度模板附件视图（DSV）要求同时包含 `Depth | Stencil`，若构图期不拦截，会拖到 `Prepare` 阶段创建视图失败触发 `RADRAY_ABORT`。
- **根本原因（高层接口与数据建模问题）**：
  1. **职责耦合**：RDG 的 `RDGAccess` 是**整资源粒度**的依赖与屏障追踪节点（不保存子资源范围），原实现却在 `Prepare` 中用 `RDGAccess` 驱动所有视图（含光栅附件视图）的预创建；
  2. **接口参数类型过宽**：`WriteColor`、`SetDepthStencil`、`AddClearColor`、`AddClearDepthStencil` 直接复用了五元组 `gpu::SubresourceRange{BaseArrayLayer, ArrayLayerCount, BaseMipLevel, MipLevelCount, Aspects}`。但光栅附件在双后端下 `MipLevelCount` 恒为 `1`、`Aspects` 恒为纹理格式的完整 aspect 集合（只读或单平面清屏已由 `TextureState` 与 `clearDepth`/`clearStencil` 表达）。复用通用范围结构使两个无效自由度暴露给调用方，迫使 `ValidateAttachments` 在编译期补齐防御性拒绝。
- **已有处理评估**：
  - `09806ee` 将 `Prepare` 中基于 `RDGAccess` 的默认视图预创建收窄为仅 `ShaderRead` 与 `UnorderedAccess`，光栅附件的预创建与 Group 组装统一走 `ResolveAttachmentView`，并在 `ValidateAttachments` 校验 `normalized->MipLevelCount == 1` 与 `normalized->Aspects == gpu::GetTextureFormatAspects(desc.Format)`。**该处理在现有接口签名下是正确的结构解耦。**

### 2. D3D12 块压缩尾级 Mip 读回与哨兵测试同步屏障

- **出现的问题**：
  1. 在 `1 × 1` 的 BC1 尾级 mip 上执行 `CopyTextureToBufferRegion` 时，直接以逻辑纹素宽高构造 `D3D12_BOX{0, 0, 0, 1, 1, 1}` 触发 D3D12 调试层错误 873（`COPYTEXTUREREGION_INVALIDSRCBOX`）；
  2. 哨兵测试在同一命令缓冲内先用 `CopyBufferToBuffer` 填充整个读回缓冲，紧接着用 `CopyTextureToBufferRegion` 覆写局部行，触发 Vulkan Synchronization Validation 的 `WRITE_AFTER_WRITE` 报警。
- **根本原因**：
  - Vulkan 的 `VkBufferImageCopy::imageExtent` 要求传逻辑纹素尺寸（`1 × 1` mip 传 `4 × 4` 会违反 `VUID-vkCmdCopyImageToBuffer-pRegions-00183`）；而 D3D12 的 `ID3D12GraphicsCommandList::CopyTextureRegion` 对 BCn 块压缩子资源要求 `D3D12_BOX` 的 `right`/`bottom` 始终对齐到 `4 × 4` 物理压缩块边界（Phase 1 的 `CopyBufferToTextureRegion` 因单切片传 `pSrcBox = nullptr`、多切片已用物理块尺寸构造 `box` 而未触发此分支）。
- **已有处理评估**：
  - `09806ee` 保持公共 RHI `TextureRegion2D` 与 Vulkan 后端使用逻辑纹素范围，在 `CmdListD3D12::CopyTextureToBufferRegion` 内用 `block.Columns(image.Width) * block.BlockWidth` 与 `block.Rows(image.Height) * block.BlockHeight` 构造物理块对齐的 `D3D12_BOX`，并在测试两次拷贝写入之间加入 `CopyDestination -> CopyDestination` 缓冲屏障。**RHI 抽象边界与后端实现均完全正确。**

### 3. Vulkan 丢失态的可见时机与首次检出时的 `vkDeviceWaitIdle`

- **出现的问题**：
  - `DeviceVulkan::MarkDeviceLost` 在首次翻转丢失状态时当场调用 `vkDeviceWaitIdle`；为防止一个线程还在 `vkDeviceWaitIdle` 内等待、另一个线程已观察到 `_deviceLost == true` 并开始销毁在途资源，`67b6c8a` 增加了第二个原子变量 `_lostIdleComplete` 并在 `IsDeviceLost()` 中执行 `_lostIdleComplete.wait(false)`。
- **根本原因（高层生命周期接口约束）**：
  - `FrameScheduler::BeginFrame() -> FrameSlot&` 返回非空引用，无法向上层返回失败；当 `BeginFrame()` 内部的 `WaitSlot(slot)` 首次检出 `VK_ERROR_DEVICE_LOST` 返回后，`BeginFrame()` 仍会无条件继续执行 `slot.ResetCommandStorage()` 与 `slot.ResetFrameData()`，当场销毁该槽上一轮的 GPU 对象；
  - Vulkan 要求池中的命令缓冲退出 pending 状态后才能重置命令池。RadRay 选择在首次检出丢失时同步执行一次设备级 idle，让所有队列的在途槽都能在后续回收阶段释放对象。围栏查询也可能是首次检出点。见 [Vulkan 命令池重置条件](https://docs.vulkan.org/refpages/latest/refpages/source/vkResetCommandPool.html)。
- **已有处理评估**：
  - 通过 `deviceIdleAlreadyCalled` 避免 `SwapChainVulkan::DrainAndClear` 触发二次 `vkDeviceWaitIdle` 的设计是合理的；但新增的 `_lostIdleComplete.wait(false)` 属于脱离 RHI 线程模型的过度防御（详见第二节 `I3`）。

### 4. Vulkan 颜色写掩码 `MapType(ColorWrites)` 落入不可达分支

- **出现的问题**：
  - 深度遮挡测试在管线带有颜色附件但 `ColorTargetState::WriteMask = {}`（零掩码）时，在 Vulkan 下意外向颜色附件写入了白色。
- **根本原因**：
  - `vulkan_helper.cpp` 原 `MapType(ColorWrites)` 误把 4 位通道掩码当作单值枚举，仅用 `switch` 列举了 6 个枚举常量的组合并以 `Unreachable()` 结尾；传入 `{}`（0）及其他部分通道组合时踩入未定义行为。
- **已有处理评估**：
  - `73016d4` 改为 `static_assert` 核对四通道位定义后直接返回 `static_cast<VkColorComponentFlags>(v.value())`，**修复简洁且正确**。

### 5. 深层审视：为什么阶段二 T5（`DeviceLost`）会衍生出大量 Corner Case？

审阅过程中围绕 `DeviceLost` 之所以出现多条边界争议，根源在于阶段二 T5 的契约设计在三处叠加了跨层状态组合：

1. **RDG 层“纯 CPU 命令编码（`Record`）”与跨后端 RHI 阶段边界锁存丢失的耦合**：
   - 在 RDG 层的设计契约中，所有资源、视图与上传映射均在 `Prepare` 完成，`Record` 仅负责消费冻结数据并编码命令；且 Vulkan 原生 `vkBeginCommandBuffer`、`vkEndCommandBuffer`、`vkResetCommandPool` 与 `vkCmd*` 的返回码表不含 `VK_ERROR_DEVICE_LOST`。
   - 但在跨后端 RHI 实现中，RadRay 在 D3D12 命令列表 `Reset` 或 `Close` 失败后查询 `GetDeviceRemovedReason()`，据此可在阶段返回前锁存设备丢失，Vulkan `BeginRenderPass` 在 framebuffer 缓存未命中时会调用原生创建接口（启用 Tracy 时 `CommandBufferVulkan::Begin` 还会经 `TracyVkCollect` 调用 `vkGetQueryPoolResults`，尽管 Tracy 当前未向宿主透传该返回值），且用户 Pass 回调也可能返回 `false`。D3D12 对设备移除的延迟报告见[命令列表错误](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles#command-list-errors)。
   - T5 用 [`test_rdg.cpp`](../../modules/runtime/tests/test_rdg.cpp) 第 1753 行的 `ValidatedRecordingClosesAfterLossAtBeginOrInsidePass`（Fake `LoseOnBegin` 与 `CopyPass` 回调内置 `Device->Lost = true`）作为“跨后端 RHI 在 `Begin()` 或 `Record` 阶段返回时已锁存丢失”的统一契约测试，迫使实现同时处理“原生命令缓冲未开启”与“原生命令缓冲已开启但半途终止”两条路径。
2. **让 `Fence::WaitFor` 的 `bool` 返回值同时绑定“围栏值已到达”与“设备当前未丢失”两重语义**：
   - 即便某围栏值在设备丢失前早已真实完成（`value <= _lastCompletedValue`），[RHI 与后端](../architecture/render-rhi.md#fence-等待与不可呈现窗口) 仍规定“超时和设备丢失均返回 `false`”。这使 `WaitFor` 无法单纯依据 `_lastCompletedValue >= value` 返回 `true`，而必须在完成判断上附加 `!_device->IsDeviceLost()`。
3. **`FramePipeline::Prepare / Record` 与 `CommandBuffer::Begin / End` 保持 `void` 签名**：
   - 错误无法通过返回值向上层短路，迫使 `RenderGraphFramePipeline` 在内部调用链上靠旁路轮询 `AbandonIfLost(slot)` 感知失败。

---

## 二、审阅甄别后的缺陷修复与过度防御清理方案

综合审阅结论，按以下 4 项执行项（`I1`–`I4`）与 1 项保留项（`I5`）落地：

### I1：修复 `FrameScheduler::RunFrame()` 在 `DeviceLost` 提前返回路径遗漏复位 Debug `_inFrameCallback`（真实缺陷）

- **出现的问题**：
  - 在 [`frame_scheduler.cpp`](../../modules/runtime/src/frame_scheduler.cpp) 中，`BeginFrame()` 第 54–56 行在 `#if RADRAY_IS_DEBUG` 下设置 `slot._inFrameCallback = true;`；
  - `RunFrame()` 在第 70 行（`BeginFrame()` 返回后检出 `slot.GetDevice().IsDeviceLost()` 直接 `return false;`）以及第 78–82 行（`!submitted` 且 `IsDeviceLost()` 时置 `_running = false; return false;`）两条提前返回路径上，**均未将 `slot._inFrameCallback` 复位为 `false`**。
- **原因**：
  - 原先 `RunFrame()` 无失败返回分支，`slot._inFrameCallback = false` 只写在 `Submit` 之后的第 84 行；`67b6c8a` 新增两条 `DeviceLost` 提前返回路径时只清理了 `_running = false`，遗漏了 Debug 专属的 `slot._inFrameCallback`。
  - 在 `RunFrame()` 返回 `false` 到 `WaitIdle()` 调用之间的空窗期，`slot._inFrameCallback` 错误停留在 `true`，使“禁止在帧回调期之外调用 `slot.Emplace`”的 Debug 断言保护失效。
- **解决方案**：
  - 在 [`frame_scheduler.cpp`](../../modules/runtime/src/frame_scheduler.cpp) 的 `RunFrame()` 中，通过共同的退出清理路径（或局部 `ScopeGuard`）确保两条 `DeviceLost` 提前返回分支与正常完成分支一样，在 `#if RADRAY_IS_DEBUG` 下置 `slot._inFrameCallback = false;`（关闭当前帧的 `Emplace` 登记窗口），同时保持 `_slotBegun = true`、当前槽指针与当前帧号不变（使 `GetCurrentSlot()` 仍返回该槽、再次 `BeginFrame()` 仍拿到同一帧号，直至 `WaitIdle()` 统一回收）。

### I2：在保持 `WaitFor` 丢失态返回 `false` 契约的前提下精简 `FenceD3D12::WaitFor` 与 `FenceVulkan::WaitFor`（清理冗余调用）

- **出现的问题**：
  1. [`d3d12_impl.cpp`](../../modules/gpu_impl/src/d3d12/d3d12_impl.cpp) 的 `FenceD3D12::WaitFor` 在入口写出了：
     ```cpp
     if (_device->IsDeviceLost()) return false;
     if (GetCompletedValue() >= value) return !_device->IsDeviceLost();
     if (_device->IsDeviceLost()) return false;
     if (timeoutMs == 0) return false;
     ```
     其中 `GetCompletedValue()` 内部首行本就会检查 `if (_device->IsDeviceLost()) return previous;`，导致入口前三次检查相互重叠；
  2. [`vulkan_impl.cpp`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp) 的 `FenceVulkan::WaitFor` 在围栏已完成的快路径上，先在 `if (_fence->GetCompletedValue() < value)` 调用一次 `vkGetSemaphoreCounterValue`，跳过 `if` 块后又在 `return _fence->GetCompletedValue() >= value && !_device->IsDeviceLost();` **重复调用第二次 `vkGetSemaphoreCounterValue`**；且当围栏尚未完成而 `timeoutMs == 0` 时未提前返回，多执行了一次 `vkWaitSemaphores(..., 0)` 系统调用。
- **原因与审阅边界**：
  - [RHI 与后端](../architecture/render-rhi.md#fence-等待与不可呈现窗口) 规定“`Fence::WaitFor(value, timeoutMs)` 超时和设备丢失均返回 `false`”——即便 `value <= _lastCompletedValue`（目标值在丢失前已完成），只要设备在调用前已丢失或在本次 `GetCompletedValue()` 查询中首次检出丢失，`WaitFor` 仍须返回 `false`。
  - 原实现为了防守这一契约，在 `FenceD3D12::WaitFor` 堆砌了重叠前置判断，并在 `FenceVulkan::WaitFor` 造成了快路径二次查询。
- **解决方案**：
  - **严格保留“设备丢失时 `WaitFor` 恒返回 `false`（含 `value <= _lastCompletedValue`）”的公共契约**，将两后端收敛为对称结构：
    - `FenceD3D12::WaitFor`：
      ```cpp
      if (GetCompletedValue() >= value) return !_device->IsDeviceLost();
      if (timeoutMs == 0 || _device->IsDeviceLost()) return false;
      ```
      循环内保持超时与 `WAIT_OBJECT_0` 唤醒后的 `GetCompletedValue() >= value && !_device->IsDeviceLost()` 检查；
    - `FenceVulkan::WaitFor`：
      ```cpp
      if (_fence->GetCompletedValue() >= value) return !_device->IsDeviceLost();
      if (timeoutMs == 0 || _device->IsDeviceLost()) return false;
      ```
      仅在未完成且 `timeoutMs > 0` 时进入 `vkWaitSemaphores`，返回 `VK_SUCCESS` 或 `VK_TIMEOUT` 时做单次尾查 `return _fence->GetCompletedValue() >= value && !_device->IsDeviceLost();`，检出 `MarkDeviceLost` 直接返回 `false`；
  - 直接调用 `FenceVulkan::WaitFor` 和 `FenceD3D12::WaitFor`，在各后端验证 `value == _lastCompletedValue`（如 `7`）时：① 调用前已丢失时 `WaitFor(7, 0)` 返回 `false`；② 本次 `WaitFor(7, 0)` 内部 `GetCompletedValue()` 首次检出丢失时同样返回 `false`。

### I3：移除 `DeviceVulkan::_lostIdleComplete` 双原子阻塞等待，保留单次 `vkDeviceWaitIdle` 与返回码校验（清理伪多线程过度防御）

- **出现的问题**：
  - [`vulkan_impl.h`](../../modules/gpu_impl/include/radray/gpu_impl/backend/vulkan_impl.h) 与 [`vulkan_impl.cpp`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp) 在 `std::atomic<bool> _deviceLost{false}` 之外又维护了 `std::atomic<bool> _lostIdleComplete{false}`，令 `IsDeviceLost()` 与 `MarkDeviceLost()` 的非首次分支阻塞在 `_lostIdleComplete.wait(false)` 上。
- **原因**：
  - 该实现假设存在多线程并发调用 RHI 的竞争，但 [RHI 与后端](../architecture/render-rhi.md#可选-rhi-校验) 明确规定“RHI 不支持并发 CPU 调用；创建、录制、提交、查询和销毁均由调用方串行化”，本后端在 `vkDeviceWaitIdle` 期间也要求调用方串行访问设备的队列（`QueueVulkan` 无内部锁）；同时若同线程在 `vkDeviceWaitIdle` 钩子内重入查询 `IsDeviceLost()`，`_lostIdleComplete.wait(false)` 会导致单线程自死锁。
- **解决方案**：
  - 仅删除 `DeviceVulkan::_lostIdleComplete` 成员及其 `.store(true)` / `.notify_all()` / `.wait(false)`，将 `DeviceVulkan::IsDeviceLost()` 简化为 `return _deviceLost.load();`；
  - **完整保留** `DeviceVulkan::MarkDeviceLost` 现有的 `deviceIdleAlreadyCalled` 分支与 `vkDeviceWaitIdle` 返回码检查：
    ```cpp
    bool DeviceVulkan::MarkDeviceLost(VkResult result, std::string_view operation, bool deviceIdleAlreadyCalled) noexcept {
        if (result != VK_ERROR_DEVICE_LOST) return false;
        if (!_deviceLost.exchange(true)) {
            if (!deviceIdleAlreadyCalled) {
                const auto idle = _ftb.vkDeviceWaitIdle(_device);
                if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST) RADRAY_ABORT("vkDeviceWaitIdle after device loss failed: {}", idle);
            }
            RADRAY_ERR_LOG("{}: device lost ({})", operation, result);
        }
        return true;
    }
    ```

### I4：保留 `Begin()` 后丢失拦截与后端 `End()` 现状，在 `Begin()` 成功但图录制中途丢失时配对调用逻辑 `Command->End()`

- **出现的问题**：
  - [`render_graph_pipeline.cpp`](../../modules/runtime/src/render_graph_pipeline.cpp) 的 `RenderGraphFramePipeline::Record` 在 `state.Command->Begin()` 成功之后，若 `state.CompiledGraph->Record(...)` 中途发生设备丢失，第 178 行 `if (state.AbandonIfLost(slot)) return;` 会直接退场，跳过了末尾的 `state.Command->End()`，使命令缓冲（及其 RHI 校验包装器）停留在 `RecordingState::Recording` 与 `Open = true`，依赖后续 `WaitIdle() -> CommandAllocator::Reset()` 强行擦除打开的录制状态。
- **原因与审阅边界**：
  1. **必须保留 `state.Command->Begin()` 返回后的 `if (state.AbandonIfLost(slot)) return;` 检查**：若设备丢失发生在 `Begin()` 当下（如 `ValidatedRecordingClosesAfterLossAtBeginOrInsidePass` 的 `atBegin = true` 分支），命令缓冲未进入录制态，必须立即放弃录制，不得继续调用 `ResetQueryPool` 或进入用户 Pass 回调；
  2. **不前移双后端 `End()` 中的 `_profilerZones->Zones.clear()`**：Tracy 的 `VkCtxScope`（`TracyVulkan.hpp:655`）与 `D3D12ZoneScope`（`TracyD3D12.hpp:678`）析构函数在 `m_active` 为真时会直接向命令缓冲录制 `vkCmdWriteTimestamp` 或 `EndQuery` / `ResolveQueryData`。`CompiledRDG::Record` 返回前仍会配对调用 `PopDebugGroup()`，启用逐 pass 计时时还可能写入组尾时间戳。本轮只控制图返回后的帧尾计时。丢失态下，双后端 `End()` 提前返回，`CommandBufferValidation::End()` 在没有活动 encoder 时闭合包装器状态；
  3. **配对逻辑 `Command->End()`**：当 `state.Command->Begin()` 成功返回、而随后的 `state.CompiledGraph->Record(...)` 中途检出设备丢失时，跳过帧尾时间戳、查询解析与读回屏障，但继续执行 `state.Command->End()` 与末尾 `state.AbandonIfLost(slot)`，使命令缓冲在 `Record` 退出时即完成逻辑闭合（`CommandBufferValidation::End()` 当场将 `State` 置为 `Executable`、`Open = false`、`DebugDepth = 0`，未开启校验层时底层 `End()` 由首行 `if (_device->IsDeviceLost()) return;` 安全空转）。
- **解决方案**：
  - 保持 `CmdListD3D12::End()`、`CommandBufferVulkan::End()` 与 `CommandBufferValidation` 现有实现不变；
  - 将 [`render_graph_pipeline.cpp`](../../modules/runtime/src/render_graph_pipeline.cpp) 的 `RenderGraphFramePipeline::Record` 收敛为：
    ```cpp
    void RenderGraphFramePipeline::Record(FrameSlot& slot) noexcept {
        RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.Record");
        auto& state = *_slotStates[slot.GetIndex()];
        if (state.AbandonIfLost(slot)) return;
        if (!state.Command || !state.CompiledGraph) RADRAY_ABORT("RenderGraphFramePipeline: graph not prepared");
        state.Command->Begin();
        if (state.AbandonIfLost(slot)) return;
        if (state.QueryCount) {
            state.Command->ResetQueryPool(state.Queries.get(), 0, state.QueryCount);
            state.Command->WriteTimestamp({state.Queries.get(), gpu::QueryPipelineStage::Top, 0});
        }
        const bool recorded = state.CompiledGraph->Record(*state.Command, state.QueryCount > 2 ? state.Queries.get() : nullptr);
        if (!recorded && !slot.GetDevice().IsDeviceLost())
            RADRAY_ABORT("RenderGraphFramePipeline: RDG Record failed");
        if (recorded && state.QueryCount && !slot.GetDevice().IsDeviceLost()) {
            state.Command->WriteTimestamp({state.Queries.get(), gpu::QueryPipelineStage::Bottom, 1});
            const gpu::ResourceBarrierDescriptor before = gpu::BarrierBufferDescriptor{state.Readback.get(), state.ReadbackState, gpu::BufferState::CopyDestination};
            state.Command->ResourceBarrier(std::span{&before, 1});
            state.Command->ResolveQueryData({state.Queries.get(), 0, state.QueryCount, state.Readback.get(), 0});
            const gpu::ResourceBarrierDescriptor after = gpu::BarrierBufferDescriptor{state.Readback.get(), gpu::BufferState::CopyDestination, gpu::BufferState::HostRead};
            state.Command->ResourceBarrier(std::span{&after, 1});
        }
        state.Command->End();
        state.AbandonIfLost(slot);
    }
    ```

### I5（审阅后保留现状，不作修改）：`GetUnsupportedGraphicsStateVulkan` 的 `ColorWrites` 越界位检查

- **审阅甄别记录**：
  - 尽管按 [RHI 与后端](../architecture/render-rhi.md#可选-rhi-校验) 的分层原则（“关闭后直接返回真实后端……后端将 CPU 参数和调用顺序合法性作为前提”），CPU 描述符位掩码合法性由 `rhi_validation.cpp` 统一负责（D3D12 后端关闭校验时同样不重复做软拒绝），但 [`vulkan_helper.cpp`](../../modules/gpu_impl/src/vk/vulkan_helper.cpp) 第 203 行的 `if (target.WriteMask.value() & ~uint32_t(ColorWrite::All))` 仅 1 行代码，且直接支撑 `test_vulkan_device_features.cpp` 的独立 helper 单测与关闭校验时的防御性拒绝。为避免无收益的契约改动，**该项保留现状不修改**。

---

## 三、验证与验收门禁

1. **定向与全量单元测试**：
   - `test_radray_runtime`：
     - 验证 `FrameScheduler::RunFrame()` 在 `DeviceLost` 下的两类帧槽对象生命周期：① 若 `BeginFrame()` 在等待复用在途槽（`WaitSlot`）时首次检出丢失，该槽**上一帧**登记的对象在本次 `BeginFrame()` 内当场完成析构（在 `RunFrame()` 返回 `false` 时已可观测到析构，如 `LossDuringWaitSkipsCompletionAndReleasesSlotAfterWaitReturns`）；② 若 `BeginFrame()` 完成后，在当前帧 `Extract`、`Prepare`、`Record` 或 `Submit` 阶段触发丢失，`RunFrame()` 返回 `false` 时立即关闭 `Emplace` 登记窗口（通过 `RunFrame` 的共用退出路径核对 Debug 标记复位），保留 `GetCurrentSlot()` 与 `BeginFrame().GetFrameId()` 原槽与原帧号，**当前未提交帧**已登记的对象不在 `RunFrame()` 返回时提前销毁，而仅在随后的 `WaitIdle()` 中析构一次（如 `DeviceLossStopsSubmissionAndKeepsCurrentFrameUntilCleanup`）；
     - 保留带校验包装器的 `ValidatedRecordingClosesAfterLossAtBeginOrInsidePass`，验证回调、提交和诊断。另将未包装的 fake command 直接交给 pipeline，在 `RunFrame()` 返回后、`WaitIdle()` 之前检查 Pass 与 `End()` 的次数。`Begin()` 当下丢失时两者均为 0，Pass 内丢失时两者均为 1；
   - `test_radray_gpu_impl`：
     - 验证 `FenceD3D12::WaitFor` / `FenceVulkan::WaitFor` 在目标值等于缓存完成值（`value == _lastCompletedValue`）时，调用前已丢失与本次查询首次检出丢失两条路径均返回 `false`；
     - 验证 `VulkanDeviceLoss` 在首次由队列提交或交换链排空检出丢失时均只执行一次同步 `vkDeviceWaitIdle`，随后才销毁交换链、信号量与设备句柄，并在有限时间内返回（不触发 30 秒围栏超时死等）；
     - 验证 `VulkanColorWriteMask` 16 种合法组合与越界位 `0x10` 拒绝用例通过；
   - 全量 `ctest --output-on-failure` 通过。
2. **规范与文档检查**：
   - 执行 `python tools/check_docs.py` 与 `git diff --check` 零错误通过。
