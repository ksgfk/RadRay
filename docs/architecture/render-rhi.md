> - 适用: 加 RHI 接口或改后端；排查 barrier / 描述符 / 同步问题；找某个后端实现在哪一段
> - 权威: 本文说明显式 RHI layout 契约、所有权与两个后端映射。Shader 编写方式见 [authoring 指南](../guide/shader-authoring.md)
> - 锚点: `modules/render/include/radray/render/rhi.h`, `modules/render/include/radray/render/backend/vulkan_impl.h`, `modules/render/include/radray/render/sampler_cache.h`, `modules/render/src/rhi.cpp`, `modules/render/src/rhi_validation.cpp`, `modules/render/src/sampler_cache.cpp`, `modules/render/src/d3d12/d3d12_impl.cpp`, `modules/render/src/vk/vulkan_impl.cpp`

# RHI 与后端

`radrayrender` 提供后端无关的 RHI（`rhi.h` 中的接口与描述符）、可选 CPU 校验层，以及 D3D12 / Vulkan
原生实现。它不知道资产、场景、帧节奏，只知道 GPU 对象。

## 可选 RHI 校验

`Device::Create(desc, RhiValidationOptions)` 在创建真实后端后按固定配置决定是否包装。
`RhiValidationOptions::IsEnabled` 在 Debug（定义 `RADRAY_IS_DEBUG` 且未定义 `NDEBUG`）中默认开启，
Release、RelWithDebInfo、MinSizeRel 默认关闭；
调用方可以显式覆盖。`LogCallback` / `LogUserData` 接收 RHI 误用诊断；未提供回调时写项目错误日志。
回调可能由不同 worker 调用，接收者负责线程安全并存活至 device 和对象销毁；回调不重入同一对象。
该开关独立于 D3D12 debug layer、GBV 和 Vulkan validation / synchronization validation。

```cpp
auto device = Device::Create(descriptor, {.IsEnabled = true});
// 性能运行显式传 {.IsEnabled = false}。
```

开启后，device 创建或借出的 queue、allocator、command、encoder、buffer、texture、view、shader、
layout、parameter set、PSO、sampler、fence、query pool、swapchain 和同步对象都通过同一 device 的包装层。
嵌套描述符、variant 和 span 先完整校验再解包，返回描述符里的对象指针指向包装对象。
借用 queue、缓存 sampler、backbuffer 与同步对象在各自 owner 的有效期内具有稳定包装身份；
swapchain 重建会释放旧 image / sync 包装。组布局与完整布局分别包装，数值 BindingId、GroupSlot、PushId 原样透传。

关闭后直接返回真实后端，不创建校验上下文、包装对象、注册表或校验状态。
后端将 CPU 参数和调用顺序合法性作为前提，只保留实际执行所需的原生 allocator 复用、PSO 缓存、
描述符 dirty / initialized 信息、布局映射、原生保活、交换链同步回收与 profiler 数据。
能力探测、原生创建失败和不可恢复执行错误处理仍在对应实现中。
依赖具体后端类型的工具或测试须显式关闭包装，公共 RHI 调用方不强转后端类型。

D3D12 与 Vulkan 普通对象操作直接使用有效对象的原生指针或句柄，非空性由校验层预检或关闭校验时的调用方契约保证。
创建前的 DXGI factory / Vulkan 环境检查、幂等销毁、可选接口 / 句柄、原生失败结果处理仍然保留。

校验层检查 device 归属与有效性、范围与对齐、usage、录制与 allocator epoch、encoder 配对、
附件 / PSO 兼容、binding 数字 ID、资源类型、完整 dynamic offsets，以及 CPU 上可检测的父子销毁顺序。
批量调用全部预检通过才向后端转发；状态在成功后提交，不自动修正录制顺序或补发同步。

| 非法调用 | 行为 |
|---|---|
| 有失败结果的操作 | 记录日志并返回空值、false 或既有错误状态 |
| 普通 void 操作（不含 EndRenderPass / EndComputePass） | 记录日志并跳过，校验状态不变 |
| EndRenderPass / EndComputePass | 记录日志并 ABORT |
| Present / DiscardAcquiredFrame 的非法 frame 或失效对象 | 记录日志并 ABORT |
| 没有可靠失败值的查询 | 记录日志并 ABORT |
| 无法撤销的非法析构 | 记录日志并 ABORT |

显式 `Destroy()` 可被拒绝并重试；析构无法保留对象，违反依赖顺序时立即终止。
原生创建失败、Unsupported、超时和 RetryLater 沿用既有结果。`EndRenderPass` / `EndComputePass`
按值接收 `unique_ptr<T>` encoder，调用时转移所有权，正常返回前销毁 encoder；校验失败立即 ABORT，不提供恢复或重试路径。
swapchain 包装保存真实 outstanding frame；Present / DiscardAcquiredFrame 按值接收 frame，调用时转移 token，
校验失败立即 ABORT。正常返回后清理 outstanding 状态，所有返回状态均不允许重试同一个 frame。
原生重建尝试可能在失败时也释放旧 images / sync；包装随之失效，但存活的 swapchain 仍可再次 Recreate，
AcquireNext 继续透传原生 RequireRecreate 等状态。

上下文与加锁注册表由每个 device 持有，独立 worker 可以并发使用各自对象；同一对象及共享 allocator
仍由调用方串行访问。本层不跟踪完整 GPU 在途依赖、跨队列同步或资源状态，也不保证检测直接访问
已释放对象、裸指针 ABA 或未等 GPU 完成就 Reset。调用方仍须满足这些生命周期与同步契约。

Vulkan 的 viewport 可以用负高度表达 Y 翻转；D3D12 要求正高度。公共 shader source 是字节 span，
不要求指针对齐；Vulkan 创建 shader module 时按需对齐到原生 word 数据。

`QueryPoolDescriptor::DebugName` 是非拥有的 `std::string_view`，调用方须保证其引用的字符串
在 `CreateQueryPool` 调用期间有效；两个后端均即时设置原生对象名称，不保存该 view。
D3D12 与 Vulkan 查询池仅平铺保存查询类型与数量，不保存创建描述符。

## Timestamp 时钟关联

`QueryPool::GetTimestampCalibration` 只提供查询 tick 的频率/周期，不能关联 CPU 与 GPU 时钟。
`CommandQueue::GetTimestampClockCalibration` 返回真实 GPU/CPU 原始 tick、各自 Hz 频率、CPU
时钟域和校时调用前后的 CPU tick。D3D12 使用 queue 的 `GetClockCalibration` 与
`GetTimestampFrequency`，CPU 域为 QPC，CPU 频率来自 `QueryPerformanceFrequency`。
调用不提交命令、不等待队列；调用方按需要采样，不在每次 query 写入或取值时调用。
调用区间是观测耗时，不能作为硬件校时精度上限。上层自行把明确的 CPU 域映射到其时钟。

`GetTimestampClockCalibration` 是纯虚接口，各后端必须显式实现；Vulkan 明确返回 `Unsupported`。
D3D12 的无效队列、API 失败和无效频率/区间分别
返回 `InvalidQueue`、`ApiFailure`、`InvalidSample`，原生错误在 `NativeError`。只有 `Success`
结果的 tick 与频率可用于转换，失败不以 CPU 时间或零值伪造 GPU 样本。已有 query 的 GPU
write/resolve/availability 语义不变。

## D3D12 执行失败

D3D12 的 `ColorTargetState::WriteMask={}` 是合法零写掩码：颜色附件仍存在，draw 不写颜色，
depth test/write 按原 depth state 执行。它不等同于没有颜色附件；未知掩码位仍拒绝。

D3D12 命令列表 Close、队列 Wait/Signal、Present 与 ResizeBuffers 的失败立即终止进程，
HRESULT 检查成功时直接返回，仅失败时查询 `GetDeviceRemovedReason()`，诊断同时保留调用 HRESULT
和设备移除原因。Submit 入口与 ExecuteCommandLists 返回后独立检查设备状态，因为设备移除后部分
队列调用仍可能返回成功。创建接口原有的可空结果契约不变。

启用 D3D12 debug layer 时，诊断通道在内部 command signature、descriptor heap 和 queue 创建前
建立。支持 `ID3D12InfoQueue1` 时使用 callback；否则由 `TryDrainValidationMessages` 轮询
`ID3D12InfoQueue`。DXGI 消息另从 `IDXGIInfoQueue` 的 `DXGI_DEBUG_DXGI` producer 收集，
两者进入同一个 `RenderLogCallback`。callback 可能来自其它线程，调用方必须保证日志接收者的
线程安全与寿命，不在 callback 内调用 D3D12 或再次 Drain。

失败检查在终止前先收集验证消息，再报告 HRESULT 与设备状态。正常诊断边界覆盖原生对象创建、
view/sampler 创建、descriptor `FlushWrites`、command list Close/Reset、allocator Reset、
ExecuteCommandLists 和 Submit 收尾，以及 Present/ResizeBuffers。Fence 的新完成值和 `WaitFor`
返回时收集 GPU 侧消息，因此调用方的完成轮询也能看到 GBV 诊断；收集不增加 GPU 等待。
[GBV 输出发生在 GPU 执行后](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-d3d12-debug-layer-gpu-based-validation#debug-output)，
提交时的一次收集不能代替完成后的收集。测试应在真实 GPU 完成并收集消息后断言验证错误数。

轮询按 retrieval filter 的可见消息数量和游标读取，只有消息成功记录后才推进游标；读取失败报告
诊断并保留消息。活动队列不调用 `ClearStoredMessages`，避免删除 GBV 在读取期间追加的消息。
调试队列取消固定消息数量上限，原生消息历史随诊断会话保留；游标避免正常重复输出。外部清空队列
或修改 retrieval filter 会重新建立读取范围，filter 修改可能重放历史消息。应用应在开始验证前配置
filter，并串行修改 filter/清空队列；外部清空后须在追加新消息前 Drain 一次，以重新建立游标。
DXGI producer 属于进程级诊断，新 Device 从诊断通道建立时开始收集，避免重放其它 Device 的旧消息。
实际发生的队列容量丢弃也会报告，不能被当作完整验证。
销毁 Device 时，内部 queue、缓存、allocator 和 descriptor heap 清理后再次收集，最后才注销 callback
并释放诊断接口，日志接收者必须存活至这一过程完成。

Fence 查询把 `UINT64_MAX` 视为设备移除，禁止上层将其比较为成功完成。
`Fence::WaitFor(value, timeoutMs)` 是有限 CPU 等待，0 只查询；false 表示超时，不能作为完成。
即使 timeoutMs 为 UINT64_MAX，也钳制到后端可表达的有限时长。调用方串行等待同一 fence。
D3D12 检查事件注册、系统等待结果，并在唤醒后重新核验完成值；设备移除和较早超时的事件
注册也能唤醒事件。Vulkan 使用有限 vkWaitSemaphores。兼容 Wait() / Wait(value) 的期限为
30 秒；超时明确终止，不在 GPU 仍可能使用资源时继续销毁。上层可用 WaitFor/poll 做可取消关闭。
这些检查在检测到错误的边界终止，避免继续发布成功的 flight 完成通知；不提供设备恢复。
D3D12 flip-model 不能对已最小化、已隐藏、已销毁或客户区为 0 的 HWND 提交 GPU 写入，也不能
对其调用 `Present`：驱动会以 `DXGI_ERROR_ACCESS_DENIED` 移除设备。Acquire 在这种 HWND 上
返回 `RetryLater`。Present 遇到不可呈现窗口时跳过 DXGI Present 并消费 token，
返回 RetryLater，不等待 present queue；提交完成仍须实际 fence
证明。正常 Present 使用 [DXGI_PRESENT_DO_NOT_WAIT](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present)，
DXGI_ERROR_WAS_STILL_DRAWING 返回 RetryLater，保留未排入呈现的 readiness；不会当成 device loss。
当前调用方自行负责停止旧 surface 的录制、等待已提交使用及呈现结束，并释放旧引用后再修改窗口或交换链。
不要在 Win32 钩子或 WndProc 内阻塞等待依赖消息泵推进的渲染线程。

## 区域纹理上传

`CommandBuffer::CopyBufferToTextureRegion` 接受 `BufferToTextureCopyDescriptor`，其中
`BufferTextureCopyRegion` 明确源 byte offset、row pitch 和目标 mip、array layer、X/Y、宽高。
`ValidateBufferTextureCopyRegion` 验证单采样、非压缩 color 2D/2DArray、copy usage、子资源与区域边界、
设备 pitch/placement 对齐及源 buffer 范围。源内容从指定 offset 开始，只上传区域的行；
目标区域外的 texel 保持原值。D3D12 映射到 placed footprint + CopyTextureRegion，Vulkan
映射到 VkBufferImageCopy + vkCmdCopyBufferToImage。不支持的 command buffer 实现返回 false。

`CopyTextureToTexture` 校验原生图像维度，而非采样 view 的逻辑类型；等格式、等采样数的
2D / 2D array / cube layer 可以互相复制，mip、layer、区域、copy usage 仍逐项检查。
D3D12 backbuffer 描述提供 CopySource / CopyDestination，并按原生 DENY_SHADER_RESOURCE
标志报告 Resource；不会因其为外部纹理而丢失实际支持的复制用途。

D3D12 flip swapchain 的 sRGB 请求使用 UNORM DXGI 存储格式，并保留逻辑 sRGB 格式创建 RTV；
创建与 resize 使用同一映射。编码发生在 RTV 写入，符合
[DXGI 色彩空间契约](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/converting-data-color-space)。

## 所有权：设备、独占对象与借用

| 对象 | 持有方式 |
|---|---|
| `Device` | `shared_ptr`，且 `enable_shared_from_this` |
| buffer、texture、view、allocator、command、shader、layout、parameter set、PSO 等 | 创建入口返回 `Nullable<unique_ptr<T>>`，调用方持有 owner |
| queue、缓存 sampler、swapchain image 与同步对象 | 从所属 device/cache/swapchain 借用，不转移 owner |
| graphics/compute encoder | Begin 返回独占 owner，按值交给 End 消费 |

`Device::Create` 创建 RHI device；资源和显式布局通过其纯虚工厂创建，encoder 由 command 开始。`Nullable<unique_ptr<T>>` 表达可能创建失败；`Get()` 借用，`Release()` 转移 owner。

`Device` 之所以是 `shared_ptr`：后端实现对象需要回指 device 拿分配器、描述符堆和函数表，
而 device 的生命周期由调用方管理。RHI 不为每个资源公开共享工厂；调用方可以显式共享
一个独占创建的 owner，但仍须满足全部使用者的完成条件。native layout 只通过调用方共享同一 RHI 对象复用。

**两段式析构**：对象具有幂等的 `Destroy()`，析构函数兜底释放。所以 `Destroy()` 可以安全重复调用，缓存要"先显式销毁再 erase"时就用它。

`RenderBase` 是全部对象的根，禁拷贝禁移动。除 `GetTag()` 在 `rhi.h` 中直接返回类型标签外，
所有实例操作都是纯虚接口；保留构造、虚析构、静态工厂和无状态辅助函数。
`RenderObjectTag` 是位标志形式的类型标签，子类位或合成父类位
（`GraphicsCmdEncoder = CmdEncoder | (CmdEncoder<<1)`），用于日志和 RHI 校验注册表的对象类型检查。

## 命令录制存储

`Device::CreateCommandAllocator(queue)` 创建独占的 RHI recording storage，绑定同 device 的实际 queue。
`CreateCommandBuffer(allocator)` 创建独占 wrapper/native list，固定借用创建时的 allocator。
allocator 必须晚于全部 children 销毁。销毁 child 只注销并释放自身，不销毁共享 storage。
开启 RHI 校验时，创建检查 backend、device、有效性与实际 queue；提交检查录制状态及绑定 queue。

一个 epoch 内每个 child 只 Begin/End 一次。Begin 不重置 storage；End 只关闭当前录制。
每份关闭的录制最多提交一次；再次提交需要 allocator Reset 后重新录制。
`CommandAllocator::Reset()` 由调用方在全部 GPU 使用完成、且没有打开的录制后调用，不等待 GPU。
Reset 使旧录制失效，并清理所有 child 的原生 keepalive 与校验 epoch 引用，包括本轮不再使用的 child；
成功 End 的 encoder 立即销毁。
开启 RHI 校验时，违反 Begin/End、encoder、Reset、父子销毁顺序或提交已失效录制会被拒绝并诊断；创建仍返回可空结果。
可检测状态不等于完整 ABA 防护：裸指针重新录制后不能识别调用方保留的旧逻辑引用。

D3D12 storage 在创建时持有唯一的 `ID3D12CommandAllocator`，创建失败返回空值；不自动池化或扩容。
同一 allocator 同时最多一条 list 处于 Begin/End 区间，关闭后可以继续顺序录制其他 child，无需先提交或等待 GPU。
开启 RHI 校验时，第二条重叠 Begin 在原生调用前被拒绝，且可以在原录制成功 End 后重试；被拒绝的 End 不释放占用。
Vulkan 一个 storage 持有一个未开启逐 buffer reset 的 command pool，绑定真实 queue family；安全复用时整池 Reset。
同线程交错录制不增加 pool。共享 allocator 的全部操作由调用方串行化；独立 storage 可以由独立 worker 录制。
两端直接暴露各自的录制约束，调用方通过 `GetBackend()` 选择分配策略。
共享录制内存不提供 GPU 数据同步，barrier 和 Submit 数组顺序仍由调用方负责。

名称分别作用于 allocator 与 child，child 命名不会改写共享 pool 的名称。

## 后端选择

选后端**不是**运行期能力探测，而是"你传了哪个 descriptor"：

```cpp
using DeviceDescriptor = std::variant<D3D12DeviceDescriptor, VulkanDeviceDescriptor>;
```

`Device::Create` 对它 `std::visit`，分派到 `d3d12::CreateDevice` / `vulkan::CreateDeviceVulkan`。
这是全仓库唯一决定**创建哪个后端**的分派点。对应后端未编译时打日志返回 `nullptr`，不尝试
另一个后端。

Shader 直接通过 `ShaderDescriptor::Source` 接收原始 DXIL 或 SPIR-V 字节码，`Category` 与设备匹配。
`ShaderEntry` 在 PSO 描述中选择 entry。render 不解码 artifact、不解析反射、不链接 shader/compiler；
调用方可以使用独立 shader/compiler 模块，也可以提供外部工具生成的合法字节码。

编译期开关在顶层 `CMakeLists.txt`：`RADRAY_ENABLE_D3D12`（需 WIN32）、`RADRAY_ENABLE_VULKAN`。
`modules/render/CMakeLists.txt` 据此追加 `src/d3d12/` 或
`src/vk/` 的源文件并定义同名宏。**源文件 glob 非递归**，新增 `.cpp` 要重新 configure。

运行期探测的是**选哪块 GPU**：D3D12 走 DXGI 1.6 的 `EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`
并回退到枚举评分，Vulkan 按 `VkPhysicalDeviceType` 打分。

Vulkan 还有一个进程级全局：`VkInstance` 存在 `g_vkInstance`，经 `InstanceVulkan::InitEnv` /
`ShutdownEnv` 管理，独立于 `Device`。D3D12 的对应物是 `DXGIFactory::Create`。
`InstanceVulkan::IsValidationEnabled()` 与 `DXGIFactory::IsValidationEnabled()` 报告实际启用的验证状态；
运行时据此区分验证不可用和后续设备初始化失败。

## 绑定模型

调用方定义 shader 与资源之间的 ABI，后端只按显式描述创建布局并执行绑定。

```text
BindingGroupLayoutDescriptor → BindingGroupLayout → ShaderParameterSet
                                      ↓
PipelineLayoutDescriptor（GroupSlot + 组布局、push blocks）→ PipelineLayout
                                                             ↓
                                               Shader + PSO descriptor → PSO
```

`Device::CreateBindingGroupLayout` 与 `CreatePipelineLayout` 返回可空独占 owner。
创建描述里的 spans 只借用至返回；布局复制自身需要的不可变描述与后端映射。`GetDesc()` 返回的 spans
借用至布局销毁。完整布局和参数集借用组布局，PSO 借用完整布局，device 必须覆盖所有这些对象的寿命。

| 数字命名空间 | 作用域 | 使用方式 |
|---|---|---|
| BindingId | 一个组 | `set->Set(bindingId, arrayIndex, value)` |
| GroupSlot | 一个完整布局 | `encoder->BindShaderParameterSet(groupSlot, set, offsets)` |
| PushId | 一个完整布局 | `encoder->SetPushConstants(pushId, byteOffset, bytes)` |

ID 由调用方指定，不含地址、generation 或 HLSL 名称。同一个数字可用于不同命名空间。
绑定项声明 `Kind`、`Count`、`Stages`，并独立填写可选 `D3D12`、`Vulkan` 映射；当前后端只消费自己的一侧。
D3D12 映射包含 `RegisterType`（CBV/SRV/UAV/Sampler）、Register、Space、Table/RootDescriptor placement。
Vulkan 映射包含 Set、Binding、Regular/Dynamic placement。一个组的全部 Vulkan 绑定必须属于同一 set；
空组使用 set 0。GroupSlot 不必等于 set，也不等于 D3 register space；不同组不能占用同一 native set。
组内 binding ID 唯一，普通 descriptor 数组用 Count 表达；dynamic/root buffer 与固定 sampler 限定 Count=1。

`ShaderParameterValue` 为 buffer、typed buffer、texture view、sampler 的 variant。
`ShaderBufferBinding` 携带 buffer range 和 structured stride；typed buffer 使用独立
`ShaderTexelBufferBinding`，同时携带格式；raw buffer 的 stride 为 0。Set 只更新指定数组元素。
`ImmutableSampler` 直接持有 render 自有 `SamplerDescriptor`，归组布局，不能通过 Set 覆盖。
固定与普通 sampler 使用相同 filter/address/LOD/bias/anisotropy/compare/border/reduction 语义；
Vulkan 还支持合法的 unnormalized coordinates，D3D12 拒绝不能表达的 unnormalized、mirror-clamp 和整数 border。
设备不支持的 sampler 能力返回失败，不静默降级。sampler cache key 包含这些字段。

Shader 是否使用全部槽位不影响布局。调用方负责字节码与显式布局的 ABI 匹配，以及跨完整布局使用参数集时的
原生兼容性。多个 PSO 可共享一个完整布局；多个完整布局可共享同一组布局和参数集。
可选 RHI validation 仅检查槽位存在、可设置的资源类型、数组/buffer 范围、动态偏移、device 归属和必要调用状态；
不按完整结构、hash、原生坐标或对象身份判断布局兼容，不执行完整 shader ABI 校验。

### D3D12 组存储与 Root Signature

组布局不创建 RS；它按 BindingId 排序，在组内按 `(heap 类型, shader visibility)` 建立稳定的 descriptor tables，
并分别保存 root buffer 和固定 sampler 项。CBV/SRV/UAV 可以共表，sampler 单独成表。
创建完整布局时先放 push constants，再组合各组的 root descriptors、tables 和固定 sampler；
保存 GroupSlot 到本次 root parameter 编号的映射，序列化 RS 1.1 后直接 `CreateRootSignature`。
`RootSigD3D12` 直接持有 `ComPtr<ID3D12RootSignature>`，序列化数据只作为创建输入。
根签名超过 64 DWORD 或原生创建失败返回空值。

参数集每个 table 分配连续 CPU mirror 与 shader-visible 区间，组内 offset 不随完整布局变化。
Set 更新 mirror 和 dirty ranges；FlushWrites 只发布 dirty 部分，并避免跨未初始化空洞批量复制。
root buffer 保存 GPU 基地址，绑定时加偏移。绑定根据当前 PSO 的完整布局映射下发，不重排参数集 descriptor 存储。
不同完整布局可以给同一组不同 root 编号，调用方共享组布局即可保留稳定的组存储。

### Vulkan 组布局与参数集

`BindingGroupLayoutVulkan` 直接创建并拥有一个 `VkDescriptorSetLayout`，保存 descriptor 数量和按 native binding
排序的动态偏移顺序。固定 sampler 从 device sampler cache 取得，必须在 cache/device 销毁前释放全部组布局与参数集。
参数集只从组布局取得 handle、pool sizes 与槽位信息，通过现有 descriptor allocator 分配；不访问完整布局。
`PipelineLayoutVulkan` 借用组布局，直接创建并拥有 `VkPipelineLayout`。set 0..最大 set 之间的空洞使用该完整布局
私有的一个空 `VkDescriptorSetLayout`；同一对象的多个空洞共用它，其他完整布局各自创建。

Set 直接更新 descriptor set，typed buffer view 归参数集。`FlushWrites()` 不另建发布批次。
销毁完整布局只释放它自身的 native pipeline layout 和私有空 set layout，不销毁借用的组布局。

### Dynamic offsets 与 push constants

`ShaderParameterDynamicOffset{BindingId, ArrayIndex, Offset}` 使用组内 ID，Offset 单位为字节。
每个 dynamic/root buffer 必须显式提供一次偏移，包括零；当前 ArrayIndex 必须为 0。
输入 span 顺序任意；Vulkan 按组创建时确定的 native 顺序打包，D3D12 加到对应 root buffer 地址。
调用方保证偏移、range 和设备对齐限制合法；校验开启时拒绝缺项、多项、重复、越界和错位。

Push constants 由显式 PushConstantDescriptor 定义独立 PushId、Size、Stages 与 DX/VK 位置。
Vulkan 限一个逻辑块，D3D12 支持多个 root constant 块；部分更新的 byteOffset 和 data.size 都必须按 4 字节对齐，
且范围位于声明块内。Vulkan 在块内偏移上加描述中的 native Offset；D3D12 转为块内 DWORD offset。

### Vertex input

开启 RHI 校验时，graphics PSO 创建前做共享 CPU 校验：semantic、format、location、slot、offset/stride 以及
重复 binding/attribute 必须有效；校验失败时不调用 D3D12/Vulkan native PSO API。

VB 与 PSO 的可移植绑定顺序以 `rhi.h` 的 `GraphicsCommandEncoder::BindVertexBuffers` 契约为准。
D3D12 不保存 VB 绑定缓存；每次绑定从当前 PSO 取得 stride 并直接下发。校验层拒绝未先绑定 PSO 的调用。
切换 PSO 时不自动重新下发 VB。
Vulkan 的 VB 绑定同样不保存去重缓存，每次合法调用都直接下发。
两个后端的索引缓冲绑定也不保存去重缓存；每次合法的 `BindIndexBuffer` 调用都会下发原生绑定。
图形编码器仍保留当前 PSO 与 layout 状态，相同 PSO 的重复绑定会跳过原生调用。

### 描述符分配

D3D12 分两套：`CpuDescriptorAllocator` 是分页堆，每页用 `D3D12MA::VirtualBlock` 做子分配；
`GpuDescriptorAllocator` 是单个 shader-visible 堆 + `FirstFitAllocator`。Device 持 4 个 CPU
分配器（CBV_SRV_UAV / RTV / DSV / Sampler）和 2 个 GPU 堆（resource 65536、sampler 2048）。
Sampler 堆使用 D3D12 允许的完整 shader-visible 容量，容纳多视图、多 flight 同时持有的 parameter sets。
`CmdListD3D12::Begin` 时把两个 GPU 堆 `SetDescriptorHeaps`（copy 队列除外）。
`DescriptorHeapViewRAII` 负责归还。

参数集创建时为每个 table 分配等长 CPU mirror 与 GPU 区间；不同 table 区间不保证相邻。
分配失败通过已有 RAII 释放已成功的区间。GPU 区间随 set 持有；每次绑定不生成 descriptor 快照。
Vulkan 的 `DescriptorSetAllocatorVulkan` 继续按 `{layout, poolSizes}` 在 descriptor pool 页上分配与复用。
描述符分配器、sampler cache、render-pass/framebuffer 机制与布局所有权独立。

## 原生布局直接所有权与显式共享

RHI 没有 pipeline layout cache，也没有 Vulkan descriptor set layout cache。相同描述的两次创建分别执行
原生创建，不保留去重 key、hash、cache entry、驱逐表或布局专用引用计数。复用由调用方保存并共享同一个
RHI 布局对象完成；取消缓存不保证驱动内部不会共享实现。

这项决定取代[历史计划](../history/2026-10-22-explicit-rhi-binding-layout-rationale.md)中保留原生布局缓存的安排。
先前由 shader active resource 集合定义布局，使 shader 变体、资源裁剪和 compiler metadata 影响参数 ABI，
并把名称解析、policy modifier、缓存身份和参数集兼容性带进 RHI。显式组布局让 ABI 和复用边界归调用方；
删除隐式去重可减少所有权链、缓存关闭顺序和跨布局身份依赖。代价是重复创建相同描述会重复支付原生创建与内存成本，
调用方必须主动共享布局，必要时在自己的生命周期范围内组织复用。

Destroy 幂等，部分创建失败由局部 owner 释放已创建的资源。没有新增全局共享状态、隐式延迟销毁或异常恢复。
调用方必须等 GPU 完成、录制引用退出、依赖对象释放后才能销毁布局或复用参数集。
参数集初次绑定后由 validation 封存；完成全部使用后 `ResetBindingsForReuse()` 才能重新写入整个组。
直接所有权与取消缓存都不能替代 fence 同步。

## 命令录制

```
Device::CreateCommandAllocator(queue) → CommandAllocator
Device::CreateCommandBuffer(allocator) → CommandBuffer
  Begin() / End()
  ResourceBarrier(span<ResourceBarrierDescriptor>)
  BeginRenderPass(附件描述) → 可空 unique_ptr<GraphicsCommandEncoder>
  EndRenderPass(unique_ptr<...>)          // 结束，epoch 仍保留引用
  拷贝 / resolve / query
CommandAllocator::Reset()                // 真实完成后结束旧 epoch
```

allocator 的原生映射与后端限制见[命令录制存储](#命令录制存储)。
Begin 不结束旧 epoch，重复借出同一命令前必须 Reset。
两个后端的 allocator Reset 都要求全部录制关闭、GPU 使用结束；Vulkan 先 reset native pool，
再清理已结束编码器及去重的 RP/FB pin。调用方负责在真实 GPU 完成后安排 Reset，再复用 storage。

D3D12 不为 command buffer 创建或持有空 root signature；`EndRenderPass` 与 `EndComputePass`
均不额外重置原生 root signature。后续 encoder 在绑定 pipeline 时设置其 layout 的 root signature，
参数组由该 encoder 显式绑定。使用 `nullptr` 重置签名仍有 GBV 驱动兼容性限制：在 RTX 3060、
驱动 `32.0.15.9144` 上，结束 compute encoder 并清空签名后，后续录制可触发 `nvwgf2umx.dll`
的 `0xC0000005` 访问违例；同一录制关闭 GBV 或保留签名后可通过。

graphics encoder 的 `DrawIndirect`/`DrawIndexedIndirect` 与 compute encoder 的 `DispatchIndirect` 消费
带 `BufferUse::Indirect` 的参数；固定 count 和 count=0 的行为由 RHI 保持，RHI 不提供 GPU count
buffer。`CommandBuffer::ResolveTexture` 一次处理一个 mip 和连续 array layers，两个后端都要求同格式、
同尺寸的 2D color source/destination，source 为 MSAA、destination 为单采样；depth/stencil、缩放与
格式转换被拒绝。

`RenderPassBeginDescriptor::AllowUavWrites` 是执行期开关，不属于 render pass/graphics PSO compatibility
事实。D3D12 将它映射到
[`D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_render_pass_flags)；
Vulkan 传统 render-pass 路径不需要对应 begin flag，写阶段是否合法由 device capabilities 与 barrier
契约控制。

## 资源状态：显式 barrier，无自动推导

`ResourceBarrierDescriptor` 是 `variant<BarrierBufferDescriptor, BarrierTextureDescriptor, BarrierUavDescriptor>`，
调用方给出 before/after 状态。render 层**不做** per-resource 状态跟踪，初始状态在创建时固化
（buffer 按 memory type 映射；D3D12 texture 使用 COMMON，Vulkan image 使用 UNDEFINED layout）。Buffer/Texture barrier 均不支持跨队列
ownership transfer；Vulkan 的源/目标 queue family 均为 `VK_QUEUE_FAMILY_IGNORED`。

后端各自把状态对翻译成原生形式：D3D12 把 UAV→UAV 变成 UAV barrier，把无变化和
PRESENT↔COMMON 这类等价转换直接跳过；Vulkan 生成 buffer/image memory barrier，
access mask 与 layout 由 helper 映射，swapchain image 从 Undefined 转换时补 src stage。

`BarrierUavDescriptor` 显式表达 UAV 写入后的 shader read/write memory ordering，`Target` 必须是
非空 texture 或 buffer。D3D12 使用 resource UAV barrier；Vulkan 使用 shader write → shader
read/write 的全局 memory dependency，不改变 layout，因此不会假设 texture 的所有 mip 都处于
GENERAL。`SubresourceRange::Aspects` 选择 Color、Depth、Stencil，Auto 在归一化时展开为格式的全部
aspects。采样 depth/stencil 默认只选 Depth，可显式选择 Stencil；DS attachment 必须包含全部 aspects。
D3D12 transition 按选中 plane 展开 mip/layer；stencil SRV 使用对应 plane 与 X24/X32 类型映射。
Vulkan 当前采用 coupled depth/stencil layout，转换会扩大至两个 aspects；不启用独立 DS layouts 时
这是 [Vulkan barrier 规范](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageMemoryBarrier.html) 的要求。
DepthRead 的 Vulkan access mask 同时覆盖 attachment read/write：传统只读附件的 Store 操作仍使用
depth/stencil write access，离开只读 pass 的 barrier 必须等待该操作；这不会允许 PSO 写入只读附件。
该要求见 [VkAttachmentStoreOp](https://docs.vulkan.org/refpages/latest/refpages/source/VkAttachmentStoreOp.html)。
`NormalizeSubresourceRange` 同时拒绝格式不具备的 aspect、零 count、越界和加法溢出。

`BarrierTextureDescriptor::Range` 是纹理 barrier 范围的唯一来源，默认 `SubresourceRange::AllSub()`
覆盖整张纹理；count 为 `SubresourceRange::All` 时表示从对应 base 到末尾。两个后端使用归一化后的
范围，D3D12 全范围 transition 使用整资源 barrier，部分范围按 plane/mip/layer 展开；D3D12 UAV
barrier 仍作用于整资源。

Buffer barrier 携带 `BufferRange`，Vulkan 映射 offset/size，D3D12 legacy barrier 保守转换整 buffer。
Texture/Buffer barrier 都携带 BeforeStages/AfterStages，UNKNOWN 保留保守映射；Vulkan 只收窄 shader
stage，保留 transfer、host、attachment 阶段。ShaderRead 对应普通 shader resource，不附带未公开的
input-attachment access。

`PushDebugGroup` / `PopDebugGroup` 必须平衡，分别映射 D3D12 command-list Unicode event 与
Vulkan debug-utils label；Vulkan 未启用 debug-utils 时不录标签。

启用 `RADRAY_ENABLE_PROFILER` 时这对调用同时是 GPU 时间戳 zone 的边界：每个 Direct/Compute queue
在构造时创建一个 Tracy GPU context（Copy queue 不采样），命令缓冲持有已打开 zone 的栈，`End()` 关闭
未平衡的 zone，销毁时丢弃并告警。两个后端的 zone 与 debug group 不完全一一对应：

| 后端 | zone 边界 | 原因 |
|---|---|---|
| Vulkan | 每个 debug group 一个 zone | `vkCmdWriteTimestamp` 在 render pass 内合法；`Begin()` 时回收查询槽 |
| D3D12 | render pass 外的 group 一个 zone；render pass 内的 push 不开 zone，pop 推迟到 `EndRenderPass` | Tracy 在 zone 结束时调用 `ResolveQueryData`，它在 `BeginRenderPass/EndRenderPass` 内被运行时禁止 |

因此 D3D12 同一原生 render pass 内的嵌套 group 不产生独立 GPU zone；`Submit()` 后收集已完成的时间戳。
宏入口与开关见 [Core](core-facilities.md#性能采样宏)。

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

`ValidateTextureDescriptor` 共享 CPU 规则与 backend support facts；开启 RHI 校验时，`CreateTexture`
先校验再分配。非法 dimension/format/usage、零尺寸、超 mip/extent、MSAA+mips、MSAA+UAV、
非 Device texture memory 或不支持的 allocation hint 返回包含字段的 reason。
validation 不替换 format，也不降低 sample count。资源不足/native create 失败仍返回空。
上层 view 必须明确选择兼容格式；RHI 不提供自动 reinterpret 策略。

## 同步

`CommandQueueSubmitDescriptor` 一次表达三类同步：fence + value 对、`WaitToExecute`
（acquire 信号量）、`ReadyToPresent`（present 信号量）。

Vulkan 的 acquire wait 使用 `ALL_COMMANDS`。RHI 不限制 acquired image 的首次访问阶段，
布局转换、transfer readback 与颜色附件访问都必须在 acquire 完成之后；不能只等待颜色/计算阶段。

**两个后端在这里不对称，这是现状而非设计意图**：Vulkan 完整实现三类；D3D12 的 `Submit`
只处理 fence，忽略两个 swapchain 同步槽，`AcquireNext` 也对它们传 `nullptr`——D3D12 靠
DXGI 的 frame-latency waitable object 达到同一效果。

`Fence` 在 D3D12 是 `ID3D12Fence` + Win32 event（`_fenceValue` 表示"下一个可用值"）；
在 Vulkan 是 timeline semaphore；设备创建时显式检查 `timelineSemaphore`，不支持则创建失败。

`SwapChainVulkan` 使用无扩展的 acquire fence 回收方案，借鉴 Khronos
[swapchain_recreation（b69afa54965d）](https://github.com/KhronosGroup/Vulkan-Samples/tree/b69afa54965dbfb18f04a4a0baccf98b88c6de37/samples/api/swapchain_recreation)。
不启用 `VK_EXT/KHR_swapchain_maintenance1`，也不把调用方的 timeline fence 当作呈现完成证明。

- **acquire 信号量由后续同 image 的 acquire fence 证明可回收。** 每次成功 acquire 同时提交
  binary semaphore 与原生 `VkFence`；返回 image index 本身不代表 fence 已完成。后续同 image
  的 fence signal 后，才回收上一轮已被提交消费的 acquire semaphore。首次 acquire 的 fence
  同样保活至完成。回收池只存已完成的对象，池空时分配，不在正常帧路径无限等待。
  `Submit` 的 `SignalFences` 可以为空；调用方仍必须通过 `WaitToExecute` 等待 acquire，并让
  `ReadyToPresent` 的 signal 依赖该等待及 backbuffer 工作。
- **present 信号量按 swapchain image 索引。** 它存在 `Frame::readyToPresent` 上；再次 acquire
  同一 image 后，后续提交等待 acquire semaphore，再 signal 此 present semaphore。
- **旧链按代次退休。** 成功重建保留旧原生 swapchain 及其 frame/sync 资源。一旦后继代某次
  成功 present 经同 image 的后续 acquire fence 证明完成，即可回收更早的退休代；每个旧代
  还必须没有自身未完成的 acquire fence。不固定等待新链第一次呈现的 image，全部完成记录
  都参与回收判定。未呈现的代不需要后继呈现证明，但仍等待其 acquire 操作完成。

`Recreate` 前调用方必须终结 outstanding frame、等待所有使用旧 backbuffer 的 GPU 工作，
并释放旧 view 与 command 引用。成功重建不执行 `WaitIdle`；旧 image/sync 的校验包装随重建
失效，内部保活不延长调用方句柄有效期。连续重建可能积压多代资源，恢复呈现后回收，不以
数量上限强制释放尚未获证明的对象。

重建一旦尝试替换旧链，无论 native 创建还是 image 初始化失败，都通过故障清理排空设备、
等待成功提交的 acquire fence，销毁新链和所有保留代，保持无可 acquire 的状态；保留 surface
与请求配置，下一次从空链重试。绝不恢复或再次传入已退休的 `oldSwapchain`。
surface 的 `maxImageCount == 0` 表示没有上限，只应用最小值限制。

Acquire 的 `TIMEOUT` / `NOT_READY` / `OUT_OF_DATE` 不产生 frame 或完成证明；`SUBOPTIMAL`
仍产生必须终结的 frame。Present 的 `SUCCESS` / `SUBOPTIMAL` 可提供后继呈现证明；
`OUT_OF_DATE` / `SURFACE_LOST` 的排队等待仍保活，不能作为这种证明。Present OOM 不假定
semaphore wait 已排队，消费 RHI token 后禁止继续 acquire，必须重建或关闭。
`SURFACE_LOST` 要求重建交换链对象，`DEVICE_LOST` 要求设备恢复，不再等待无法完成的 fence。

关闭与重建失败采用 `vkDeviceWaitIdle` 加已有 acquire fence 等待后清理，surface 最后销毁，
重复 `Destroy` 安全。这是无扩展的实际清理方案；仍存在 Vulkan Guide 说明的
[退出时呈现资源回收保证缺口](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html#_vk_ext_swapchain_maintenance1_extension)，
不能把验证层无报错或普通队列 fence 完成当作补足该保证的证据。

`QueueVulkan::Submit` 始终提交单个 `VkSubmitInfo`，不再为同步验证插入前置空批次。
多交换链与同队列 timeline 串联的旧校验层问题、当时用例和校验层版本门槛见
[归档构建快照](../history/2026-10-21-runtime-build-test-snapshot.md#vulkan-多交换链同步验证已知问题)，不属于当前测试覆盖。

`SwapChainFrame` 是 move-only 句柄，带 owner + token，由 protected `MakeFrame` 创建。
校验包装用 `ValidateFrame` 检查 owner/token；Present / DiscardAcquiredFrame 按值接收，移动后调用方 frame 失效。
外来、已消费或失效的 frame 在开启校验时立即 ABORT；关闭校验时由调用方保证契约。

`DiscardAcquiredFrame` 是显式的非呈现终结：调用者先丢弃未提交的 backbuffer 命令并等待已提交工作。
D3D12 消费 outstanding frame，返回既有 result 的 Success，之后必须 Recreate 或关闭；
`DiscardAcquiredFrame` 是纯虚接口，Vulkan 当前不支持该操作，调用立即 ABORT。析构不是 token 取消。
D3D12 Present 的不可呈现 HWND/实际 DXGI_STATUS_OCCLUDED 分支返回 RetryLater，并已消费该帧，
不能把它统计成实际可见呈现。Vulkan Present 的 RequireRecreate / Error 同样消费帧，不保留重试 token。

D3D12 记录已经取得但尚未通过实际 Present 使用的 frame-latency readiness。
显式撤销、ResizeBuffers 或隐藏窗口跳过 Present 不会消费这个 readiness；下一次合法
acquire 复用它，实际 Present 成功后才重新等待。否则连续撤销/重配会重复消耗 waitable
object 的信号并永久 RetryLater。调用方仍须在撤销/重配前排空 GPU，不能借此绕过在途资源维护。

## 视口：两个后端都原样透传

`SetViewport(Viewport)` 在 D3D12 直填 `D3D12_VIEWPORT`，在 Vulkan 直填 `VkViewport`，
**两边都不做 Y 翻转**。所以 `Viewport` 的语义是"原点在左上"，与 D3D 一致。
`Viewport` 与裁剪用的 `Rect` 定义在 `modules/core/include/radray/viewport.h`。
`PrimitiveTopology` 定义在 `rhi.h` 的枚举区，成员名和 `int32_t` 底层类型保持稳定。

Vulkan 的 NDC Y 轴朝下，要得到与 D3D12 一致的画面，处理它是**调用方的责任**——在投影矩阵里
翻，或者传一个负 `Height` 的 viewport。RHI 刻意不替你决定，因为这两种做法对
front-face winding 的影响不同。

当前 Vulkan 最低版本为 1.2，负 `Height` 使用核心能力，不提供 Vulkan 1.0 的扩展回退路径。

## 直接附件与 Vulkan 原生缓存

Instance、物理设备与 VMA 按 Vulkan 1.2 接入，低于该版本或缺少必需 timeline semaphore feature 时拒绝创建。
传统单 subpass VkRenderPass/VkFramebuffer 是主路径；不启用 dynamic rendering、imageless framebuffer、
separate depth/stencil layouts 或 synchronization2。SPIR-V 编译目标同样固定为 Vulkan1_2。

公共 RHI 没有 RenderPass/Framebuffer owner、对应 Device 工厂或 PSO 的兼容 pass 指针。RenderPassBeginDescriptor 直接描述有序 color views、可选 depth/stencil view、load/store、clear、extent/layers、render area、name 与 UAV writes。开启 RHI 校验时，ValidateRenderScope 在任何 native Begin 之前校验 view/device、格式角色、usage、sample、mip/layer、extent、重复重叠与只读操作。

PSO 的 AttachmentSignature 从现有 ColorTargets、DepthStencil、MultiSample 字段派生，只含有序格式、depth/stencil 格式及共同 samples。load/store、clear、尺寸、area 和 name 不进入 PSO 兼容身份。绑定时检查当前 scope 的签名；只读 scope 仍要求 PSO 关闭写入。

`CommandBuffer::BeginRenderPass(desc)` 经可选校验后获取原生对象并开始录制，无独立准备阶段或 token。
未处于 recording、已有 active encoder、附件校验或原生对象获取失败时返回 `nullptr`，不录制 clear/Begin，
不改变 active encoder，调用方可以修正描述后重试。描述及其 span 只借用到调用返回；附件 owner 仍须存活至命令真实完成。
调用方在 BeginRenderPass 前录制附件 barrier，失败时仍负责处理这些已录制的状态转换。
D3D12 首次 Begin 时缓存该命令的 `ID3D12GraphicsCommandList4`，附件描述仅在调用内保留；
局部 area 的 clear 在所有校验完成后先用带 rect 的 Clear，再进入 preserve 的 native pass。Vulkan 使用 renderArea，
只有 pass 和 framebuffer 均获取成功后才加入命令的去重引用。

VulkanRenderPassCache 由 DeviceVulkan 独占，借用 device。execution key 包含格式、samples、load/store、depth read-only；depth-only 格式规范化无 stencil 的操作。framebuffer key 包含有序稳定 view ID、兼容签名及 extent/layers，不依赖某个 load/store 版本的 pass 指针。PSO 创建临时借用代表 pass，成功后不长期保留；framebuffer 也不保留创建 pass 的 owner。

每条 command 在 epoch 内对 pass 和 framebuffer 去重持有 intrusive use。End、Return、CPU Submit 和取消通知都不解除引用；真实完成以后 native allocator Reset 才使录制失效并释放引用。缓存零引用条目进入 LRU，引用数表示使用而非缓存常驻。默认总预算 RP=256/FB=1024，闲置预算 RP=128/FB=256；压力只淘汰闲置条目，live 条目不可驱逐，到顶时 BeginRenderPass 返回空。缓存清理不等待 GPU idle。

view ID 在该 device 内单调且不复用。view 析构按局部 reverse index 先检查所有相关 FB 没有 command use，再同步移除闲置 FB，最后调用 vkDestroyImageView。mutex 串行化 acquire/release/invalidate/trim，允许同 device 的多个独立宿主共同计数；缓存不持有业务 texture/view 或 device shared owner。

运行基线为 Vulkan 1.2 与 timelineSemaphore，不启用 dynamic rendering、imagelessFramebuffer、separateDepthStencilLayouts 或 synchronization2。depth-only 也使用 core depth/stencil layout，VMA 和 instance 使用 1.2；shader 编译策略的默认 SPIR-V target environment 同样为 Vulkan1_2。

开启 RHI 校验时，ShaderParameterSet 初次绑定后成为封存的 descriptor 版本，Set 拒绝原地修改；封存状态仅存于校验包装。调用者确认全部引用 epoch 已结束以后可 ResetBindingsForReuse，Vulkan 同时释放旧 typed buffer views；重新借出时完整写入整个组。数值 ID 不提供对象寿命保证，不能替代组布局、完整布局和资源的 native 寿命。

## 两个后端实现文件的分区

`vulkan_impl.cpp` 与 `d3d12_impl.cpp` 都在文件顶部有 banner 列出章节，
每章以 `// == 章节名 ==` 开头。跳转：

```
rg -n '^// == ' modules/render/src/d3d12/d3d12_impl.cpp modules/render/src/vk/vulkan_impl.cpp
```

两份实现的大致顺序一致：辅助函数与适配器/物理设备选择 → 描述符堆与分配器 → 环境对象与
device 创建 → 各 `CreateXxx`（buffer/texture/view → shader →
pipeline layout → parameter set → PSO/sampler）→ queue 与 fence → command buffer 与
barrier → 编码器 → swapchain → 各对象类的收尾实现。

`d3d12_helper.cpp` 与 `vulkan_helper.cpp` 是纯枚举映射/格式转换库，外加 `format_as` 重载。
`vulkan_helper.cpp` 顶部还是 `VOLK_IMPLEMENTATION` / `VMA_IMPLEMENTATION` 的单翻译单元
包含点。

## 现状陷阱

- **SwapChain 尚不支持 HDR 输出配置**：公共 descriptor 不提供色彩空间或 HDR metadata 设置；选择浮点或高位深格式不能替代完整 HDR 输出契约。
- **D3D12 的 swapchain 同步槽未接线**：`Submit` 忽略 `WaitToExecute` / `ReadyToPresent`。
- **D3D12 纹理不带优化 clear value**：`CreateTexture` 的 `clearPtr` 恒为 `nullptr`。
- **`Buffer::FlushMappedRange` / `InvalidateMappedRange` 在 D3D12 是空实现**，
  Vulkan 侧有完整实现。upload heap 无需 flush 可以解释，readback 的 invalidate 语义未覆盖。
- **非 Win32/非 Apple 平台没有 surface 创建**（`vulkan_impl.cpp` 有 TODO 标记），走 `#else`
  后直接报错返回。
- **Vulkan GPU-based validation 默认关闭**：部分驱动上会崩。

## 测试

| 套件 | 覆盖 |
|---|---|
| `ExplicitBindingLayoutTest` | 双后端真实 dispatch/读回、不同 DX/VK 坐标、set 空洞、数组、各类 buffer、texture、普通/固定 sampler、动态偏移、部分及多块 push、组/参数集共享 |
| `ExplicitLayoutOwnershipD3D12` | 通过原生 debug 消息观察独立创建与最终释放，不依赖句柄比较 |
| `ExplicitLayoutOwnershipVulkan` | 测试侧函数表 probe 观察独立创建、私有空洞、幂等释放和部分失败清理 |
| `DescriptorDirtyRangesD3D12Test` | 描述符 dirty range 合并与未初始化空洞 |
| `VulkanSwapChainRetirementTest`、`VulkanSwapChainNativeTest`、`VulkanSwapChainWsiTest` | 保留的交换链回收与 WSI 验证 |

render 测试只使用版本控制的原始 stage 字节码和手写布局；artifact/contract 测试归 shader 模块。
测试侧 probe/原生诊断负责观测，生产对象不添加测试计数。
后端不可用与初始化失败的判定见[GPU 测试规则](../guide/build-test.md#测试)；必测后端缺失或跳过不得算作通过。
