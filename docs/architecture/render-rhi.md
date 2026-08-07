> - 适用: 加 RHI 接口或改后端；排查 barrier / 描述符 / 同步问题；找某个后端实现在哪一段
> - 权威: 本文是 RHI 抽象形状与两个后端映射关系的唯一说明。上层怎么用它见 `architecture/frame-and-gpu.md`
> - 锚点: `modules/render/include/radray/render/rhi.h`, `modules/render/include/radray/render/render_pass_registry.h`, `modules/render/include/radray/render/sampler_cache.h`, `modules/render/src/rhi.cpp`, `modules/render/src/sampler_cache.cpp`, `modules/render/src/d3d12/d3d12_impl.cpp`, `modules/render/src/vk/vulkan_impl.cpp`

# RHI 与后端

`radrayrender` = 一层后端无关的 RHI（`rhi.h`，1.5k 行纯接口与描述符）+ D3D12 与 Vulkan
两份实现。它不知道资产、场景、帧节奏，只知道 GPU 对象。

## 所有权：一个 shared，其余全 unique

| 对象 | 持有方式 |
|---|---|
| `Device` | `shared_ptr`，且 `enable_shared_from_this` |
| 其他一切 | `Nullable<unique_ptr<T>>`，由创建者独占 |

`Device::Create` 是唯一的入口，全部 `CreateXxx` 都是 `Device` 上的纯虚工厂。返回
`Nullable<unique_ptr<T>>` 表达"可能没有值"，`Get()` 取裸指针，`Release()` 转移所有权
（缓存类就靠它把对象搬进自己的 map）。

`Device` 之所以是 `shared_ptr`：后端实现对象需要回指 device 拿分配器、描述符堆和函数表，
而 device 的生命周期由上层（`GpuSystem`）决定。其余对象一律独占，不存在共享所有权——
需要共享的是上层的事；当前 runtime 不再持有 layout cache。

**两段式析构**：每个实现类都有幂等的 `Destroy()` 和内部的 `DestroyImpl()`，析构函数兜底调
后者。所以 `Destroy()` 可以安全重复调用，缓存要"先显式销毁再 erase"时就用它。

`RenderBase` 是全部对象的根：`GetTag()` / `IsValid()` / `Destroy()` 三个纯虚，禁拷贝禁移动。
`RenderObjectTag` 是位标志形式的类型标签，子类位或合成父类位
（`GraphicsCmdEncoder = CmdEncoder | (CmdEncoder<<1)`），目前只被日志的 `format_as` 消费。

## 后端选择：编译期开关 + 单点 visit

选后端**不是**运行期能力探测，而是"你传了哪个 descriptor"：

```cpp
using DeviceDescriptor = std::variant<D3D12DeviceDescriptor, VulkanDeviceDescriptor>;
```

`Device::Create` 对它 `std::visit`，分派到 `d3d12::CreateDevice` / `vulkan::CreateDeviceVulkan`。
这是全仓库唯一的后端分派点。对应后端未编译时打日志返回 `nullptr`。

编译期开关在顶层 `CMakeLists.txt`：`RADRAY_ENABLE_D3D12`（需 WIN32）、`RADRAY_ENABLE_VULKAN`。
`modules/render/CMakeLists.txt` 据此追加 `src/d3d12/` 或
`src/vk/` 的源文件并定义同名宏。**源文件 glob 非递归**，新增 `.cpp` 要重新 configure。

运行期探测的是**选哪块 GPU**：D3D12 走 DXGI 1.6 的 `EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`
并回退到枚举评分，Vulkan 按 `VkPhysicalDeviceType` 打分。

Vulkan 还有一个进程级全局：`VkInstance` 存在 `g_vkInstance`，经 `InstanceVulkan::InitEnv` /
`ShutdownEnv` 管理，独立于 `Device`。D3D12 的对应物是 `DXGIFactory::Create`。

## 绑定模型

统一操作仍是两层：`PipelineLayout` 持有 backend-native layout，`ShaderParameterSet` 携带值。
公共 `rhi.h` 不再暴露 pipeline layout descriptor；DXIL artifact 只能交给 D3D12 overload，
SPIR-V artifact 只能交给 Vulkan overload。backend-only 的 layout input 由 artifact decoder
从 compiler-owned records 临时组装，不允许 caller 另写一份 layout schema。

`ShaderParameterValue` 是 `variant<ShaderBufferBinding, ShaderTexelBufferBinding, TextureView*, Sampler*>`。

两个后端各自把 target-native records 编译成原生布局：

| | D3D12 | Vulkan |
|---|---|---|
| 每个 Group | 两个描述符表 root parameter（resource 表 + sampler 表） | 一个 `VkDescriptorSetLayout` |
| `Dynamic*` 类型 | root CBV/SRV/UAV，bind 时按 `dynamicOffsets` 设 | 动态 uniform/storage buffer |
| push constant | `32BIT_CONSTANTS` root parameter | `VkPushConstantRange` |
| 上限校验 | 无 | 有（`maxDescriptorSet*` / `maxPerStage*`） |

**`Set` 只记脏值，`FlushWrites()` 才写描述符。** D3D12 把值写进预分配的 GPU 堆区间；
Vulkan 构造 `VkWriteDescriptorSet` 数组，并按需惰性建 `VkBufferView` 承载 texel buffer。

`ShaderDescriptor` 这类喂给 RHI 的资源描述仍属于 render 层。compiler-owned metadata 由 render
decoder 转换成 backend-only 过渡 input，不能让 RHI 反向依赖 compiler client。

### Binding handle 与 vertex input

binding name 只在当前 artifact layout 上解析为不透明 `BindingHandle`。handle 带 layout generation，
DXIL 还保留 `b/t/u/s` register namespace；unknown、inactive 或其他 layout 的 handle 写入直接
失败，Debug 下跨 layout 使用会断言。`BindShaderParameterSet` 的 group index 仍保留，因为它
同时对应 D3D12 register space 与 Vulkan descriptor set。

graphics PSO 创建前会做共享 CPU 校验：semantic、format、location、slot、offset/stride 以及
重复 binding/attribute 必须有效；校验失败时不调用 D3D12/Vulkan native PSO API。

### 描述符分配

D3D12 分两套：`CpuDescriptorAllocator` 是分页堆，每页用 `D3D12MA::VirtualBlock` 做子分配；
`GpuDescriptorAllocator` 是单个 shader-visible 堆 + `FirstFitAllocator`。Device 持 4 个 CPU
分配器（CBV_SRV_UAV / RTV / DSV / Sampler）和 2 个 GPU 堆（resource 65536、sampler 256）。
`CmdListD3D12::Begin` 时把两个 GPU 堆 `SetDescriptorHeaps`（copy 队列除外）。
`DescriptorHeapViewRAII` 负责归还。

Vulkan 侧 `DescriptorSetLayoutCacheVulkan` 按 key 去重 layout，`DescriptorSetAllocatorVulkan`
按 `{layout, poolSizes}` 在页（= 一个 `VkDescriptorPool`）上分配。
`DescriptorSetLayoutVulkan` 是 render 层里**唯一**做引用计数的对象（`IntrusivePtr`），
被 `PipelineLayoutVulkan` 持有，按 refcount 驱逐。

## 命令录制

```
Device::CreateCommandBuffer(queue) → CommandBuffer
  Begin() / End()
  ResourceBarrier(span<ResourceBarrierDescriptor>)
  BeginRenderPass(...) → unique_ptr<GraphicsCommandEncoder>
  EndRenderPass(unique_ptr<...>)          // 收回并销毁
  拷贝 / query
```

D3D12 每个 `CommandBuffer` 自带一个 `ID3D12CommandAllocator`，`Begin()` 里 reset。
Vulkan 每个 `CommandBufferVulkan` 独占一个 `CommandPoolVulkan`，`Begin()` reset 池并以
`ONE_TIME_SUBMIT` 开始，且**把已结束的编码器存到 `_endedEncoders` 直到下次 `Begin`**——
让编码器及它引用的 framebuffer 活到命令缓冲被重录。

## 资源状态：显式 barrier，无自动推导

`ResourceBarrierDescriptor` 是 `variant<BarrierBufferDescriptor, BarrierTextureDescriptor>`，
调用方给出 before/after 状态。render 层**不做** per-resource 状态跟踪，初始状态在创建时固化
（buffer 按 memory type 推，texture 一律 `COMMON`）。跨队列经 `OtherQueue` +
`IsFromOrToOtherQueue` 表达。

后端各自把状态对翻译成原生形式：D3D12 把 UAV→UAV 变成 UAV barrier，把无变化和
PRESENT↔COMMON 这类等价转换直接跳过；Vulkan 生成 buffer/image memory barrier，
access mask 与 layout 由 helper 映射，swapchain image 从 Undefined 转换时补 src stage。

## 同步

`CommandQueueSubmitDescriptor` 一次表达三类同步：fence + value 对、`WaitToExecute`
（acquire 信号量）、`ReadyToPresent`（present 信号量）。

**两个后端在这里不对称，这是现状而非设计意图**：Vulkan 完整实现三类；D3D12 的 `Submit`
只处理 fence，忽略两个 swapchain 同步槽，`AcquireNext` 也对它们传 `nullptr`——D3D12 靠
DXGI 的 frame-latency waitable object 达到同一效果。

`Fence` 在 D3D12 是 `ID3D12Fence` + Win32 event（`_fenceValue` 表示"下一个可用值"）；
在 Vulkan 是 timeline semaphore，**不支持 `timelineSemaphore` 直接 `RADRAY_ABORT`**，
跨队列 barrier 缺 timeline 同样 abort。这是刻意的"宁可终止，不静默降级"。

`SwapChainVulkan` 的两个信号量按 Khronos 推荐拆分，**索引维度刻意不同**：

- **acquire 信号量按 in-flight 帧索引。** 传给 `vkAcquireNextImageKHR`，被一次 submit wait
  消费。复用前必须证明那次 submit 已完成，故 `SwapChainSyncObjectVulkan` 记下 submit 的
  timeline fence/value，回收池里按此等待或跳过——这是
  VUID-vkAcquireNextImageKHR-semaphore-01779 的要求。
- **present 信号量按 swapchain image 索引。** 核心 Vulkan 的 `vkQueuePresentKHR` 不给 fence，
  submit fence 不能证明呈现已停止使用该信号量。重新 acquire 到同一 image 才是证明，
  所以它存在 `Frame::readyToPresent` 上。

`SwapChainFrame` 是 move-only 句柄，带 owner + token，经 protected 的 `MakeFrame` /
`ValidateFrame` / `InvalidateFrame` 保证不能伪造也不能用过期帧。

## 视口：两个后端都原样透传

`SetViewport(Viewport)` 在 D3D12 直填 `D3D12_VIEWPORT`，在 Vulkan 直填 `VkViewport`，
**两边都不做 Y 翻转**。所以 `Viewport` 的语义是"原点在左上"，与 D3D 一致。

Vulkan 的 NDC Y 轴朝下，要得到与 D3D12 一致的画面，处理它是**调用方的责任**——在投影矩阵里
翻，或者传一个负 `Height` 的 viewport。RHI 刻意不替你决定，因为这两种做法对
front-face winding 的影响不同。相机侧因此保持后端无关（见
`architecture/render-framework.md`）。

当 Vulkan 物理设备 API 版本低于 1.1 时，负 `Height` 依赖
`VK_KHR_maintenance1`；runtime 会在现有 device-extension 集合中只补这个扩展。API 版本为
1.1 或更高时使用核心能力，不再请求已提升进核心的扩展，也不加入 AMD fallback。runtime 不
新增一套 capability 检测或用户提示；扩展缺失时沿用现有 device-extension 校验和设备创建
结果。

## RenderPassRegistry：一个刻意不归一化的缓存

按 descriptor 内容去重 `RenderPass` 与 `Framebuffer`。属 render 层是因为它只依赖 `rhi.h`，
同层已有 `SamplerCache` 做同一件事。

**这里不做归一化。** `ColorAttachments` 的下标**就是**渲染目标槽位
（D3D12 的 RTV 序号 / Vulkan 的 `pAttachments` 下标），交换两个附件得到的是一个不同的
render pass。所以 key 保持原序，哈希也按顺序喂入。这是最容易被后来者"顺手排个序"改坏的
地方，`RenderPassRegistryTest` 专门盯它。

**不做引用计数**：RenderPass 与 Framebuffer 没有跨资产共享的持有者，只有帧内正在录制的
命令缓冲在用。故缓存独占所有权，析构即销毁。

**唯一需要调用方配合的约定**：`Framebuffer` 存的是 `TextureView` 裸指针，所以 view 销毁前
必须调 `RemoveFramebuffersUsing(view)` 把引用它的 framebuffer 摘掉。交换链尺寸变化时重建
后备缓冲 view 就走这条路。它返回摘除条目数，便于确认真的清到了东西。

其余约定：`Clear()` 先清 framebuffer 再清 pass（反序会留下"pass 已死而 framebuffer 还在"的
窗口）；`ClearFramebuffers()` 只清 framebuffer，因为 pass 不引用 view，尺寸变化无需重建；
`Clear()` 不置空 `_device`，清完可继续用；`GetOrCreateFramebuffer` 的 `desc.Pass` 必须来自
同一个 registry。

## 两个后端实现文件的分区

`vulkan_impl.cpp`（5.6k 行）与 `d3d12_impl.cpp`（4.6k 行）都在文件顶部有 banner 列出章节，
每章以 `// == 章节名 ==` 开头。跳转：

```
Grep "^// == " modules/render/src/d3d12/d3d12_impl.cpp
```

两份实现的大致顺序一致：辅助函数与适配器/物理设备选择 → 描述符堆与分配器 → 环境对象与
device 创建 → 各 `CreateXxx`（buffer/texture/view → renderpass/framebuffer/shader →
pipeline layout → parameter set → PSO/sampler）→ queue 与 fence → command buffer 与
barrier → 编码器 → swapchain → 各对象类的收尾实现。

`d3d12_helper.cpp` 与 `vulkan_helper.cpp` 是纯枚举映射/格式转换库，外加 `format_as` 重载。
`vulkan_helper.cpp` 顶部还是 `VOLK_IMPLEMENTATION` / `VMA_IMPLEMENTATION` 的单翻译单元
包含点。

## 现状陷阱

- **D3D12 的 swapchain 同步槽未接线**：`Submit` 忽略 `WaitToExecute` / `ReadyToPresent`。
- **D3D12 纹理不带优化 clear value**：`CreateTexture` 里那段 `D3D12_CLEAR_VALUE` 逻辑被整段
  注释掉，`clearPtr` 恒为 `nullptr`。
- **`FlushMappedRanges` / `FlushMappedRange` / `InvalidateMappedRange` 在 D3D12 是空实现**，
  Vulkan 侧有完整实现。upload heap 无需 flush 可以解释，readback 的 invalidate 语义未覆盖。
- **`CmdListD3D12::_keepAliveBuffers` 是死字段**，只被 clear 从未 push_back。
- **非 Win32/非 Apple 平台没有 surface 创建**（`vulkan_impl.cpp` 有 TODO 标记），走 `#else`
  后直接报错返回。
- **`format_as(RenderObjectTag)` 漏了 `QueryPool` 与 `ShaderParameterSet`**，二者日志显示
  `UNKNOWN`。
- **Vulkan GPU-based validation 默认关闭**：部分驱动上会崩。

## 测试

| 套件 | 覆盖 |
|---|---|
| `RenderPassCacheKeyTest` | key 顺序语义、`unordered_map` 契约、descriptor 回读 |
| `FramebufferCacheKeyTest` | 同上，外加 `References` 反查 |
| `RenderPassRegistryTest` | 去重命中、计数、`RemoveFramebuffersUsing` 摘除 |

无可用 device 时测试 SKIP 而非 FAIL。
