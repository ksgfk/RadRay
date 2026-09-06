> - 适用: 加 RHI 接口或改后端；排查 barrier / 描述符 / 同步问题；找某个后端实现在哪一段
> - 权威: 本文说明 schema 7 当前 RHI layout 契约、所有权与两个后端映射。上层使用方式见帧与 GPU 文档
> - 锚点: `modules/render/include/radray/render/rhi.h`, `modules/render/include/radray/render/backend_shader_artifact.h`, `modules/render/include/radray/render/render_pass_registry.h`, `modules/render/include/radray/render/sampler_cache.h`, `modules/render/src/rhi.cpp`, `modules/render/src/backend_shader_artifact.cpp`, `modules/render/src/sampler_cache.cpp`, `modules/render/src/d3d12/d3d12_impl.cpp`, `modules/render/src/vk/vulkan_impl.cpp`

# RHI 与后端

`radrayrender` = 一层后端无关的 RHI（`rhi.h`，1.5k 行纯接口与描述符）+ D3D12 与 Vulkan
两份实现。它不知道资产、场景、帧节奏，只知道 GPU 对象。

layout 章节以下以 schema 7 contract 为准，并且已经是实现形态：group-wide `ShaderLayoutPolicy`、
公开 handle 编码、裸 binding dynamic offset 与 default Vulkan immutable sampler 都已删除。
`BindingHandle` 的内部 token 是 layout generation 加该 layout metadata table 的 record index，
位布局不是 ABI，只有两个后端可以拆开它。

## 区域纹理上传

`CommandBuffer::CopyBufferToTextureRegion` 接受 `BufferToTextureCopyDescriptor`，其中
`BufferTextureCopyRegion` 明确源 byte offset、row pitch 和目标 mip、array layer、X/Y、宽高。
`ValidateBufferTextureCopyRegion` 验证单采样、非压缩 color 2D、copy usage、子资源与区域边界、
设备 pitch/placement 对齐及源 buffer 范围。源内容从指定 offset 开始，只上传区域的行；
目标区域外的 texel 保持原值。D3D12 映射到 placed footprint + CopyTextureRegion，Vulkan
映射到 VkBufferImageCopy + vkCmdCopyBufferToImage。不支持的 command buffer 实现返回 false。
Graph 对应 typed Pass 见 [Renderer foundation](renderer-foundation.md)。

D3D12 flip swapchain 的 sRGB 请求使用 UNORM DXGI 存储格式，并保留逻辑 sRGB 格式创建 RTV；
创建与 resize 使用同一映射。编码发生在 RTV 写入，符合
[DXGI 色彩空间契约](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/converting-data-color-space)。

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

## 后端选择：descriptor visit + artifact 动态桥

选后端**不是**运行期能力探测，而是"你传了哪个 descriptor"：

```cpp
using DeviceDescriptor = std::variant<D3D12DeviceDescriptor, VulkanDeviceDescriptor>;
```

`Device::Create` 对它 `std::visit`，分派到 `d3d12::CreateDevice` / `vulkan::CreateDeviceVulkan`。
这是全仓库唯一决定**创建哪个后端**的分派点。对应后端未编译时打日志返回 `nullptr`，不尝试
另一个后端。

另有一处不选择后端的受限分派：`CreateBackendShaderArtifact` 接收已经创建的 `Device`、metadata
blob 与显式 `ShaderArtifactDecodeOptions`，先要求 device backend 与 options target 一致，再调用
对应 typed decoder 和 backend typed layout 入口。artifact envelope target 仍由 decoder 核对；
任一步失败都不尝试另一 target。运行时 caller 因而不包含 backend impl 头，typed concrete 入口与
反向组合的编译失败边界仍保留。

编译期开关在顶层 `CMakeLists.txt`：`RADRAY_ENABLE_D3D12`（需 WIN32）、`RADRAY_ENABLE_VULKAN`。
`modules/render/CMakeLists.txt` 据此追加 `src/d3d12/` 或
`src/vk/` 的源文件并定义同名宏。**源文件 glob 非递归**，新增 `.cpp` 要重新 configure。

运行期探测的是**选哪块 GPU**：D3D12 走 DXGI 1.6 的 `EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`
并回退到枚举评分，Vulkan 按 `VkPhysicalDeviceType` 打分。

Vulkan 还有一个进程级全局：`VkInstance` 存在 `g_vkInstance`，经 `InstanceVulkan::InitEnv` /
`ShutdownEnv` 管理，独立于 `Device`。D3D12 的对应物是 `DXGIFactory::Create`。

## 绑定模型

统一 command 操作仍是两层：`PipelineLayout` 持有 backend-native layout 与内部 binding metadata，
`ShaderParameterSet` 携带值。layout 构造则明确拆成三段：

```text
target artifact decode
  -> backend-specific target-typed resolve
  -> native layout creation
```

DXIL artifact 只能 resolve 成 owning/hashable `ResolvedD3D12Layout`，SPIR-V artifact 只能 resolve 成
`ResolvedVulkanLayout`；两者是对应 `Device::CreatePipelineLayout` 路径的唯一数据输入。公共层不增加
target-erased `PipelineLayoutDescriptor`，caller 不能绕过 artifact 自造第二份 schema。resolved value
拥有 strings、records、sampler recipes 与 dynamic order，不借用 artifact spans，也不包含 native
handles。

pipeline 通过 `ShaderProgramLayoutRecipe` 并列提供 D3D12/Vulkan typed options。current backend
只消费自己的字段；每个 Target layout modifier 用 canonical declaration name + expected logical kind
精确选择一项。selector 不存在/inactive、kind mismatch、duplicate target 或 D3D12 explicit carrier 上
出现 D3D12 modifier，都由 resolve 按 `ShaderLayoutResolveError` 返回并 fail closed：resolve 不做
Debug abort，否则这些拒绝路径在唯一会跑测试的配置里无法验证。这不是可恢复 API——caller 唯一的
正确反应是修 recipe 或 shader。

`ShaderParameterValue` 是 `variant<ShaderBufferBinding, ShaderTexelBufferBinding, TextureView*, Sampler*>`。
`BackendShaderArtifact::FindBindingInfo(canonicalName)` 提供只读的 target-resolved descriptor 事实：
logical kind、group、数组 count、实际 stages、dynamic placement 与 immutable/static sampler；push 或
未知名称返回空。它不暴露可供 caller 重建 layout 的 schema，Graph 参数准备只消费这份查询结果。

logical resource kind 与 native placement 分开保存。CBuffer、typed/structured/raw buffers、texture/
storage texture 与 sampler 决定 value/descriptor class；D3 Table/RootDescriptor 和 Vulkan
Regular/Dynamic 只存在于 resolved records。logical kind 与 placement 是两个独立轴，两个后端的内部
entry 都同时保留它们并由二者推导 native descriptor/root parameter type；把它们融合成单一枚举会让
uniform/storage、texel/image 的区分靠命名巧合成立，因此公共面不再有这样的枚举。

两个后端的 base policy 与合法 modifier 如下：

| | D3D12 | Vulkan |
|---|---|---|
| 无 RootSignature policy | Implicit descriptor tables | ordinary descriptors |
| descriptor table policy | serialized table 原样消费 | ordinary descriptors |
| root CBV policy | root CBV | dynamic uniform buffer |
| root SRV/UAV buffer policy | root SRV/UAV | dynamic storage buffer |
| RootConstants policy | `32BIT_CONSTANTS` root destinations | authored push declaration的 `VkPushConstantRange` |
| StaticSampler policy | full native static sampler state | full-state immutable `VkSampler` |
| Target modifier | 仅 Implicit 合法 count=1 buffer Table<->RootDescriptor | uniform/storage Regular<->Dynamic；full immutable sampler install/replacement |

DXIL artifact 的 serialized Root Signature range 非空时，D3D12 直接把同一 carrier 传给
`ID3D12Device::CreateRootSignature`，并用 `D3D12CreateVersionedRootSignatureDeserializer`
建立 descriptor table、root descriptor、RootConstants 与 static sampler destinations。此路径
不根据 active metadata 重建作者 RS，也不接受 D3 modifier；D3 static sampler 现有 direct-consumption
路径本身正确。range 为空时，resolver 按 active facts 生成 Implicit canonical topology，再把精确
合法 buffer modifiers应用为root descriptors。ordinary global RS 1.0/1.1是范围；Local RS与
directly-indexed heaps不支持。runtime不新增跨artifact/native Root Signature cache。

`ResolvedVulkanLayout` 是创建 `VkPipelineLayout` 所用数据的完整权威，至少持有：

- set 0 到 max active set 的有序 set recipes；slot 空洞由有效 empty set-layout recipe 占位；
- 每个 active descriptor 的 logical kind、set/binding/count、Regular/Dynamic placement 与 actual
  stage flags；
- full-state immutable sampler recipes及descriptor引用；
- 最多一个 active logical push block 的range/stages；
- Vulkan规定的dynamic offset packing order（set、binding、array element）；
- canonical `ResolvedLayoutHash`与name->metadata table映射。

native创建顺序为immutable sampler objects -> descriptor set layouts -> `VkPipelineLayout`；backend
layout保持sampler/set-layout引用至少覆盖pipeline layout和parameter sets的使用期。descriptor limits、
dynamic-buffer limits、push size/alignment、sampler feature/extension与native create结果在此边界产生
diagnostic/failure。unsupported sampler state不得用default state静默替换；pipeline可提供明确的
Vulkan sampler replacement modifier。hash只覆盖sampler semantics，不覆盖`VkSampler` handles。
full state 使用 Vulkan target-typed fixed-width recipe；公共 `SamplerDescriptor` 不扩张成
D3/Vulkan static-sampler policy 的统一副本。

**`Set` 只记脏值，`FlushWrites()` 才写描述符。** D3D12 把值写进预分配的 GPU 堆区间；
Vulkan 构造 `VkWriteDescriptorSet` 数组，并按需惰性建 `VkBufferView` 承载 texel buffer。

`ShaderDescriptor` 这类喂给 RHI 的资源描述仍属于 render 层。compiler-owned metadata 不能让 RHI 反向
依赖 compiler client。schema 7 declaration `TypeIndex` 只连接 CPU payload schema，不进入 resolved
layout records、native layout 创建或 `ResolvedLayoutHash`。Vulkan 已经不经过任何过渡 input：set
entries、empty set holes、dynamic order、push range 与 name table 全部直接由
`ResolvedVulkanLayout` 建立，因此 Vulkan layout 只有一种描述方式。D3D12 同样如此：
`DeviceD3D12::CreateRootSignatureInternal` 直接接受 `ResolvedD3D12Layout`，explicit carrier 与
Implicit 两条路径共用同一份 parameter group 构建，因此整个 render 层不再存在第二种 layout 描述。

### Binding handle、dynamic offset 与 push

descriptor与push declaration的canonical HLSL name都通过`PipelineLayout::FindBinding`解析为
`BindingHandle`。handle公共面只有default-invalid、validity和equality；caller不能构造或读取group、
slot、namespace、generation、table index。内部token的位布局不是ABI，且不能跨target、Variant、
recompile或layout复用。

`PipelineLayout`内部metadata table record分两类：

- Descriptor：logical kind、group/binding、array facts和一个或多个backend native destinations；
- Push：resolved size/stages、D3 root destinations和/或Vulkan ranges。

两类 record 共用同一个 name 表，因此 push declaration 与 descriptor declaration 用同样的方式取
handle；handle 命名的是 record 而不是 register，record kind 决定它只能走 parameter set write 还是
只能走 push 提交，写错一侧会被拒绝。handle 还携带发放它的 layout 的 generation，跨 layout 使用
同样被拒绝。

一个D3 declaration按visibility-disjoint参数fan-out时仍只有一个handle，一次write/offset提交到全部
destinations。`BindShaderParameterSet`的group index保留，因为它仍选择D3 register space/Vulkan set；
这不要求handle公开group。

`ShaderParameterDynamicOffset`使用`BindingHandle + Offset`，不再携带裸binding number。两个后端都
按 resolved order 为每个 slot 反查 caller 值：D3 走 group 的 root descriptor order，Vulkan 走
`DynamicOffsetOrder`。错误layout/group/handle、duplicate/missing offset与未对齐offset会被
报告并失败；缺一个或给重复的都不能变成一次静默移位，否则后面每个 dynamic buffer 都会拿到别人的
offset。因此一个 group 里每个 root descriptor / dynamic descriptor 都要恰好一个 offset，两个后端
接受同一组输入。

D3 root descriptor 的地址是 buffer GPU VA + bound range offset + dynamic offset，authored root CBV
与 Implicit modifier 生成的 root descriptor 走同一条路径并记同一种 destination，所以同一个
dynamic-cbuffer arena offset 在两种 topology 上落在同一段字节；offset 位移后的窗口必须仍在资源内，
constant buffer 还要求位移后地址保持 256-byte 对齐。visibility-disjoint fan-out 时一次提交写到全部
destinations。

push提交为`SetPushConstants(BindingHandle, bytes)`，不再传group。每次写`[0,size)`prefix，要求
`0 < size <= resolved block size`且4-byte aligned；未覆盖remainder不变，不支持destination offset。
一个Vulkan Variant最多一个active logical push block，D3仍可有多个RootConstants declarations。

### Vertex input

graphics PSO 创建前会做共享 CPU 校验：semantic、format、location、slot、offset/stride 以及
重复 binding/attribute 必须有效；校验失败时不调用 D3D12/Vulkan native PSO API。

### 描述符分配

D3D12 分两套：`CpuDescriptorAllocator` 是分页堆，每页用 `D3D12MA::VirtualBlock` 做子分配；
`GpuDescriptorAllocator` 是单个 shader-visible 堆 + `FirstFitAllocator`。Device 持 4 个 CPU
分配器（CBV_SRV_UAV / RTV / DSV / Sampler）和 2 个 GPU 堆（resource 65536、sampler 2048）。
Sampler 堆使用 D3D12 允许的完整 shader-visible 容量，容纳多视图、多 flight 同时持有的 parameter sets。
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
  拷贝 / resolve / query
```

D3D12 每个 `CommandBuffer` 自带一个 `ID3D12CommandAllocator`，`Begin()` 里 reset。
Vulkan 每个 `CommandBufferVulkan` 独占一个 `CommandPoolVulkan`，`Begin()` reset 池并以
`ONE_TIME_SUBMIT` 开始，且**把已结束的编码器存到 `_endedEncoders` 直到下次 `Begin`**——
让编码器及它引用的 framebuffer 活到命令缓冲被重录。

graphics encoder 的 `DrawIndirect`/`DrawIndexedIndirect` 与 compute encoder 的 `DispatchIndirect` 消费
带 `BufferUse::Indirect` 的参数；固定 count 和 count=0 的行为由 RHI 保持，RHI 不提供 GPU count
buffer。`CommandBuffer::ResolveTexture` 一次处理一个 mip 和连续 array layers，两个后端都要求同格式、
同尺寸的 2D color source/destination，source 为 MSAA、destination 为单采样；depth/stencil、缩放与
格式转换被拒绝。

`RenderPassBeginDescriptor::AllowUavWrites` 是执行期开关，不属于 render pass/graphics PSO compatibility
事实。D3D12 将它映射到
[`D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_render_pass_flags)；
Vulkan dynamic render-pass 路径不需要对应 begin flag，写阶段是否合法由 device capabilities 与 barrier
契约控制。

## 资源状态：显式 barrier，无自动推导

`ResourceBarrierDescriptor` 是 `variant<BarrierBufferDescriptor, BarrierTextureDescriptor, BarrierUavDescriptor>`，
调用方给出 before/after 状态。render 层**不做** per-resource 状态跟踪，初始状态在创建时固化
（buffer 按 memory type 推，texture 一律 `COMMON`）。跨队列经 `OtherQueue` +
`IsFromOrToOtherQueue` 表达。

后端各自把状态对翻译成原生形式：D3D12 把 UAV→UAV 变成 UAV barrier，把无变化和
PRESENT↔COMMON 这类等价转换直接跳过；Vulkan 生成 buffer/image memory barrier，
access mask 与 layout 由 helper 映射，swapchain image 从 Undefined 转换时补 src stage。

`BarrierUavDescriptor` 显式表达 UAV 写入后的 shader read/write memory ordering，`Target` 必须是
非空 texture 或 buffer。D3D12 使用 resource UAV barrier；Vulkan 使用 shader write → shader
read/write 的全局 memory dependency，不改变 layout，因此不会假设 texture 的所有 mip 都处于
GENERAL。D3D12 的 subresource transition 展开完整 mip/layer range 与 depth/stencil planes；
`NormalizeSubresourceRange` 统一展开 All，拒绝零 count、越界和加法溢出。

`PushDebugGroup` / `PopDebugGroup` 必须平衡，分别映射 D3D12 command-list Unicode event 与
Vulkan debug-utils label；Vulkan 未启用 debug-utils 时不录标签。

## 设备能力与纹理支持

`Device::GetCapabilities()` 返回创建后不可变的 `RenderDeviceCapabilities`，包括原
`DeviceDetail`、limits、features 与实际创建的 queue 数量。名称避免 Windows 的
`DeviceCapabilities` 宏冲突；旧 `GetDetail()` 从同一份 Detail 返回兼容值。

buffer offset limits 同时公开 constant 与 storage buffer 对齐：D3D12 分别报告 256 与 4，Vulkan
来自 `minUniformBufferOffsetAlignment` / `minStorageBufferOffsetAlignment`。`UavWriteStages` 按 shader
stage 报告 storage write 能力：Compute 必有；D3D12 Pixel 必有并在 Feature Level 11_1 以上加入
Vertex，Vulkan 只在实际启用 `fragmentStoresAndAtomics` / `vertexPipelineStoresAndAtomics` 时加入对应
阶段。Vulkan 特性语义以
[`VkPhysicalDeviceFeatures`](https://docs.vulkan.org/spec/latest/chapters/features.html) 为准；上层必须在
录制前拒绝缺失阶段，不能静默降级。

D3D12 通过 `D3D12DeviceDescriptor::QueueCounts` 在 device 创建阶段建 queue，默认仅一条
Direct queue；`GetCommandQueue` 只查询已创建的 slot，不再隐式增加 queue。Vulkan 仍使用
`VulkanDeviceDescriptor::Queues`，Dedicated 仅对确实来自专用 family 的 queue 为真。
D3D12 无法报告专用物理引擎，Dedicated 为 false。buffer size 报告 backend 的资源大小上限，
不保证当前预算下的 allocation 一定成功。

`QueryTextureSupport` 的 key 为 dimension + format + usage，返回 sample mask、filter/blend
事实及该组合的 extent/layer/mip/resource-size 上限。常见 2D 组合在 device 创建时缓存；其余
查询直接读取 native device/physical-device 能力，不修改缓存。D3D12 查询 format support 与
MSAA quality；Vulkan 查询相同 image flags/usage 的 image format properties。`Resource` 只映射
sampled usage，RHI 没有 input attachment 使用接口。

`ValidateTextureDescriptor` 共享 CPU 规则与 backend support facts；两个 `CreateTexture` 都先
校验再分配。非法 dimension/format/usage、零尺寸、超 mip/extent、MSAA+mips、MSAA+UAV、
非 Device texture memory 或不支持的 allocation hint 返回包含字段的 reason。
validation 不替换 format，也不降低 sample count。资源不足/native create 失败仍返回空。
Stage A 的 graph view 限制同 format；RHI 原有的格式 view 能力没有新增隐式 reinterpret 策略。

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
[render-framework](render-framework.md)）。

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

`RenderPassDepthStencilAttachmentDescriptor::ReadOnly` 表示整个 depth/stencil attachment 只读，要求
已有 aspect 使用 Load/Store，framebuffer view 的 usage 必须对应 DepthRead；可写 pass 使用 DepthWrite。
完整 render pass key 包含此标志。D3D12 BeginRenderPass 设置只读 depth/stencil flags；Vulkan attachment
reference 与 initial/final layout 使用与 DepthRead barrier 一致的只读 layout。PSO 的 attachment 兼容 key
仍只包含格式和 sample count；调用方必须同时关闭 PSO 的 depth/stencil 写入。

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
