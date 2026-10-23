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
接收者须存活至 device 和对象销毁；回调不重入 RHI。原生诊断回调仍可能由驱动线程调用，接收者负责线程安全。
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
参数与调用顺序校验统一放在 `rhi_validation.cpp`，后端检查保持薄，聚焦原生能力、格式约束与执行结果。

`FlushMappedRange` / `InvalidateMappedRange` 要求普通 buffer 当前处于 `Map` / `Unmap` 区间内，
未映射的调用由 RHI 校验拒绝。`PersistentMap` allocation 在创建时已映射，允许在显式 `Map` 之前或
`Unmap` 之后执行缓存操作；usage 与范围校验仍然适用。Vulkan 非 coherent 范围的 atom 对齐由 VMA 处理。

D3D12 普通 Upload / ReadBack buffer 分别固定在 GENERIC_READ / COPY_DEST，RHI buffer barrier 不为它们
录制 transition；Upload 不允许 CopyDestination，ReadBack 只允许 MapRead / CopyDestination。
带 UnorderedAccess 的可映射 buffer 使用 CUSTOM heap，不受这项固定状态限制；其 Upload 初始状态与
HostWrite 映射 COMMON，ReadBack 初始状态与 HostRead 映射 COPY_DEST。Host 与 UAV/copy 状态之间
仍须显式 barrier，CPU 读写前由调用方保证 GPU 已完成。自定义 ReadBack 同时声明 Resource 时允许 SRV。
原生限制见 [D3D12 heap types](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_type)。

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

RHI 不支持并发 CPU 调用；创建、录制、提交、查询和销毁均由调用方串行化，包括不同对象与独立 allocator。
内部局部锁不构成线程安全承诺。GPU 多队列执行仍可并行，但须由调用方显式同步。
本层不跟踪完整 GPU 在途依赖、跨队列同步或资源状态，也不保证检测直接访问
已释放对象、裸指针 ABA 或未等 GPU 完成就 Reset。调用方仍须满足这些生命周期与同步契约。

Vulkan 的 viewport 可以用负高度表达 Y 翻转；D3D12 要求正高度。公共 shader source 是字节 span，
不要求指针对齐；Vulkan 创建 shader module 时按需对齐到原生 word 数据。

`QueryPoolDescriptor::DebugName` 是非拥有的 `std::string_view`，调用方须保证其引用的字符串
在 `CreateQueryPool` 调用期间有效；两个后端均即时设置原生对象名称，不保存该 view。
D3D12 与 Vulkan 查询池仅平铺保存查询类型与数量，不保存创建描述符。

Vulkan 的 `TimestampQueries` 表示至少一个已创建 queue family 支持 timestamp；不代表所有队列均支持。
校验层按 command buffer 实际 family 检查 `timestampValidBits` 和 timestamp stage。
`ResetQueryPool` / `ResolveQueryData` 要求 graphics 或 compute family，专用 transfer family 被拒绝。

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

### D3D12 / DXGI 诊断通道

诊断实现位于内部 `modules/render/src/d3d12/d3d12_diagnostics.h/.cpp`，没有全局注册表、
函数内静态实例、全局锁或全局订阅者列表。`RenderLogCallback` 签名不变。
调用方通过 `DXGIDiagnosticContext::Create()` 创建上下文，并传入
`DXGIFactoryDescriptor::DiagnosticContext`；启用 debug layer 时缺少上下文会创建失败。
同进程所有同时存活的验证 Factory **必须显式共享同一个上下文**，包括被 Device 保留的原生 Factory。
上下文由调用方管理此共享关系，后端不自动发现或合并独立上下文；独立上下文不能隔离原生 DXGI producer。
顺序运行的独立 case 可以各建上下文，但上个 case 的原生对象和通道必须先完成关闭。

上下文按原生 COM identity 共享 D3D12 通道，按 producer 共享 DXGI 通道，每个主动读取通道只有一个消费者。
通道表、序号、锁、observer 和日志接收者均为上下文成员。Factory/Device 会话及订阅显式持有上下文；
Factory 包装对象可以先释放，Device 继续持有会话。最后一个使用者关闭通道，最后一个强引用释放上下文。
创建原生 Factory 前会话已经就绪，失败路径和最终释放仍在其生命周期内；测试也可在创建前订阅上下文。
内部 `ObserveDiagnostics(context, observer)` 只订阅指定上下文；相同 callback/userData 只在该上下文内去重。
关闭 validation 不建立会话。结构化 observer 与日志收到同一消息；消息保存来源、通道、producer、
severity、category、原生 ID、自有描述字符串、HRESULT 与采集故障类型。测试计数、消息历史、
预期匹配和自检均位于测试侧，不放入生产对象。

D3D12 在创建原生 Device 后、创建内部资源前选择一种模式，活动期间不混用：

| 初始化结果 | D3D12 采集 | DXGI 采集 |
|---|---|---|
| `ID3D12InfoQueue1` 查询及注册成功 | callback 立即分发日志与 observer | 关键节点同步读取 |
| 查询返回 `E_NOINTERFACE` | 基础 `ID3D12InfoQueue` 在关键节点同步读取 | 关键节点同步读取 |
| 其它查询错误或注册失败 | 初始化失败，保留可用失败诊断，不降级 | 保留可用失败诊断 |

callback 使用 `D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS`，不读取、清空、配置容量或维护 D3D12
存储队列；该队列溢出不代表 callback 丢失。DXGI 只读取 `DXGI_DEBUG_DXGI` 与 `DXGI_DEBUG_APP`，
不使用会重复聚合 D3D12 消息的 `DXGI_DEBUG_ALL`。

#### 同步节点与生命周期

没有后台 worker、定时扫描、扫描请求序号或后台自检。
`detail::FlushDiagnostics(context)` 或 `detail::FlushDiagnostics(session)` 在调用线程读取和分发指定
上下文中的通道，不扫描其他上下文；callback 模式只读取 DXGI，基础接口模式同时读取 D3D12 与 DXGI。
它不增加 GPU 等待，也不承诺尚未产生的异步消息已经到达。

同步节点集中于初始化完成/失败、Present 返回前、队列 Wait 完成后、Factory/Device/SwapChain
生命周期变化及致命错误退出前。普通资源操作、描述符更新、Close/Reset 和 Submit 不散布读取。
生命周期 scope 在变化前后收集，并在变化后更新 DXGI producer 容量。上下文的锁将主动读取与原生
拓扑变化串行化；callback 不取得该串行锁。callback 在注册期间可由原生线程即时进入。

Device 销毁先清理内部队列、allocator、heap、signature 等对象，保持采集有效；随后同步读取、
关闭 D3D12 通道，再释放原生 Device/Factory 引用并末次读取 DXGI。最后持有者注销 callback 时，
上下文保持有效直到原生 `UnregisterMessageCallback` 返回；该返回保证在途回调结束。
注销失败立即终止，不能继续释放可能被回调使用的上下文。生产析构不隐式等待 GPU。

callback 不调用 D3D API，上下文不引用 `DeviceD3D12*`。接收者可能运行在调用线程或原生线程，
必须线程安全、不抛异常，不调用 RHI/D3D、再次采集或销毁相关对象。相同 callback/userData 共享
日志出口；接收对象必须存活到相关会话结束。参见 [Microsoft callback 契约](https://microsoft.github.io/DirectX-Specs/d3d/MessageCallback.html)。
覆盖从注册成功开始，不补采注册前消息，也不承诺注销后最终 native Device Release 的全部内部
D3D12 消息；原生 Device 创建失败依靠 HRESULT 与 DXGI 诊断。

#### 复制、清空与保守校验

主动读取通道容量设为 **65,536 条**，push/pop 空 storage/retrieval filter；活动期间外部代码不得
读取清空、修改容量或过滤器。DXGI 容量可能是多个原生队列的合计，新建原生对象后重新配置。
APP producer 延迟创建时用应用消息初始化。DXGI 容量配置保留；基础 D3D12 关闭时恢复原容量。

两类主动读取使用相同契约：

1. 取得稳定计数快照（accepted、stored、discarded、denied），复制全部消息到自有内存，再次取快照。
   两个快照必须完全相同；复制失败或期间有变化的批次不清空。
2. 与上一次已知状态比较。累计计数必须不回退，discarded/denied 不变，新增 accepted 必须等于
   新增 stored；溢出、外部清空和不能解释的变化均报告采集不完整。
3. 完整复制的非空批次才执行 ClearStoredMessages，然后取快照。清空期间新增 accepted 必须等于
   清空后剩余 stored，discarded/denied 不变。复制后、清空前到达并被删去的消息使校验失败；
   清空后仍保留的新消息留给下一次读取。空批次同样检查外部清空和计数变化。

快照最多尝试 3 次；单条最大 1 MiB，单批最多 65,536 条、描述文本总计最多 64 MiB。
读取失败、超限、溢出、过滤器/容量破坏或计数异常使本会话永久不完整，后续仅尽力输出。
同一通道的重复采集故障去重，类型或 HRESULT 变化另行报告。不再维护滚动游标、精确缺口数或恢复成功状态。
有限容量和非原子清空无法保证永不丢失；无法确认完整性就必须失败，不能按零错误通过。

#### 每个测试的统一收尾

公共 `DeviceContext::Reset()` 幂等收尾。测试先停止并 join 自己的 CPU 线程，等待所有已创建的
Direct/Compute/Copy 队列及 slot，再显式释放 fixture 成员。局部 GPU 资源在 Submit 前声明
`GpuWorkGuard`，保证提前 ASSERT 返回也先等待、后析构。Reset 再次等待所有队列，完成检查点、
Device/Factory 释放、通道关闭和末次 DXGI 读取；停止订阅后统一匹配、断言、记录报告，最后释放 probe。
等待失败、设备移除或 30 秒超时会终止，不能继续作为成功收尾。

正常用例要求零非预期 ERROR/CORRUPTION、零采集故障、成功检查点及 GPU 等待；warning 默认只记录。
预期错误匹配来源、producer、ID、severity、数量和消息区间，采集故障还匹配故障类型和 HRESULT；
仅声明的故障可豁免。断言由测试线程执行，日志和 callback 不决定测试通过。
自检 marker 仅确认通路可用，不是完成屏障。同进程共享诊断通道的 case 必须串行执行。
[GBV 在 GPU 执行后输出](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-d3d12-debug-layer-gpu-based-validation#debug-output)，
因此必须先等待 GPU 再结算；sleep、连续空队列或 marker 都不能替代等待和生命周期关闭。
真实 Win10 的计数及自然降级验收见[构建与测试](../guide/build-test.md#d3d12--dxgi-采集验收)。

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
D3D12 每次复制的源、目标必须属于不同 subresource，即使同一 subresource 内的区域互不重叠也拒绝。
D3D12 backbuffer 描述提供 CopySource / CopyDestination，并按原生 DENY_SHADER_RESOURCE
标志报告 Resource；不会因其为外部纹理而丢失实际支持的复制用途。

Vulkan 的整子资源 buffer/image 复制一次只处理一个 aspect。组合 depth/stencil 格式必须显式选择
`Depth` 或 `Stencil`，不能使用展开为双 aspect 的 `Auto`。Stencil 每 texel 为 1 字节，D16 depth 为
2 字节，D24/D32 depth 为 4 字节；按 mip、layer 顺序排列。深度/模板的行 pitch 按
`max(TextureDataPitchAlignment, 4)` 对齐，初始 buffer offset 同时满足设备 placement 和 4 字节对齐。
范围校验与原生复制使用同一分量布局。Vulkan 1.2 路径要求在 graphics family 上执行深度/模板复制。

D3D12 的整子资源 buffer/texture 复制同样一次只处理一个 aspect，组合格式须显式选择 Depth 或 Stencil，
分别映射 plane 0 / 1。按 mip、layer 顺序排列，每个 subresource 的 buffer offset 按 512 字节对齐。
footprint 的格式、行 pitch 和实际所需大小均来自 `GetCopyableFootprints`，范围校验使用相同结果；
不能仅根据逻辑 texture format 的字节数估算 depth/stencil plane 的 footprint。

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
同线程交错录制不增加 pool；独立 storage 仍遵守 RHI 的 CPU 串行调用契约。
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
Vulkan 初始化会复制应用名和引擎名，保证传给 native API 的字符串以空字符终止。
请求 synchronization / GPU-assisted validation 时，枚举 validation layer 提供的 instance 扩展，
启用 `VK_EXT_validation_features` 后才挂入 `VkValidationFeaturesEXT`；layer 存在但缺少该扩展时初始化失败。
仅开启基础 validation 时不挂该结构。自定义 allocator 的零大小 reallocation 等价于释放原分配。

## Vulkan API 与 Feature 策略

Instance、物理设备与 VMA 以 Vulkan 1.2 为基线，SPIR-V target environment 为 `Vulkan1_2`。
loader 或物理设备版本不足则初始化失败；设备支持更高版本不自动提高 API 请求或开启额外功能。
无 feature 开关的核心能力按版本和 valid usage 使用；有开关的能力不论属于核心还是扩展，
都按当前 RHI 需求查询后启用，规范要求支持也不等于默认开启。

`vulkan_helper.h/.cpp` 内的 `GetRhiFeatureRequestsVulkan` 在创建设备前确定请求，
`SelectDeviceFeaturesVulkan` 从零生成 Enabled，不把 Supported 整体复制到创建参数。
请求集不新增公共配置接口：必需能力缺失时失败，可选能力缺失时保留回退或判为不可用。

| RHI 需求 | Feature | 级别 |
|---|---|---|
| Fence / timeline 同步 | `timelineSemaphore` | 必需 |
| UINT32 索引、CubeArray view | `fullDrawIndexUint32`、`imageCubeArray` | 可选 |
| 光栅与混合状态 | `depthClamp`、`depthBiasClamp`、`fillModeNonSolid`、`independentBlend`、`dualSrcBlend` | 可选 |
| 间接绘制 | `multiDrawIndirect`、`drawIndirectFirstInstance` | 可选 |
| 普通与 immutable sampler | `samplerAnisotropy`、`samplerMirrorClampToEdge`、`samplerFilterMinmax` | 可选 |
| Vertex / Pixel UAV 写入 | `vertexPipelineStoresAndAtomics`、`fragmentStoresAndAtomics` | 可选 |
| Framebuffer 跨 view 复用 | `imagelessFramebuffer` | 可选，缺失时使用传统 framebuffer |
| Depth/stencil 逐 aspect 转换 | `separateDepthStencilLayouts` | 可选，缺失时使用显式组合状态 |

未列出的 feature 均关闭，包括全部 Vulkan 1.1 feature、`shaderOutputLayer`、`multiview`、
`robustBufferAccess`、`hostQueryReset`、
`uniformBufferStandardLayout`、扩展 subgroup 类型、BDA、indirect count、descriptor indexing、
shader 动态资源索引、扩展数值类型和无格式 storage image 读写。
Vulkan 的 `IsLayeredRenderingFromVertexShaderSupported` 返回 false；这不改变 D3D12 的能力报告。
固定 descriptor 数组与参数集动态 offsets 继续支持，它们不隐含开启 shader 动态索引。
调用方提供的 shader 必须满足此启用集：不使用未开启的 SPIR-V capability，storage image 使用合法显式格式，
buffer 布局遵从未开启额外 layout feature 时的规则；render 不反射或改写字节码来补足能力。

sampler 创建、RHI 校验、间接绘制和 UAV stage 报告读取 Enabled，而非硬件 Supported。
未启用 `multiDrawIndirect` 时拆成单 draw；未启用 `drawIndirectFirstInstance` 时，间接参数的 firstInstance 必须为零。
其余可选能力缺失时不能使用依赖它的 view、状态或 shader 操作；sampler 不满足启用能力时返回空。
未来增加功能时同步更新请求、依赖检查和测试，不以设备版本或支持位代替请求。

查询链和创建链分别由 `FeatureChainVulkan` 持有，使用 Features2 + Vulkan11 + Vulkan12 聚合结构，
不重复挂接聚合结构覆盖的独立 feature 结构。创建链仅含 Enabled，`pEnabledFeatures=nullptr`；
设备长期保存 Supported / Enabled 值快照，所有 `pNext` 清空。
当前使用的核心 Features2 / Properties2 / 格式与内存属性查询、descriptor layout support、RenderPass2 创建与 begin/end、
VMA 的 requirements2 / bind-memory2 以及 timeline 等待、计数查询入口缺失时拒绝初始化并清理 native 对象。

保留 WSI、debug utils、portability 和 conservative rasterization 扩展规则；
启用 `VK_KHR_portability_subset` 时，当前不请求 `samplerMipLodBias`，因此公开的 sampler bias
范围固定为 `[0, 0]`，普通与 immutable sampler 创建均拒绝非零 bias。
不请求或启用 `VK_GOOGLE_hlsl_functionality1`、`VK_GOOGLE_user_type`，缺少它们不阻止设备创建。
正式工具链不依赖这两个扩展，外部与旧 shader 的离线兼容边界见 [Shader pipeline](shader-pipeline.md#target-lanes)。
不再为已提升到 1.2 核心的 properties2、timeline、旧 maintenance1 路径请求冗余扩展。
超出 1.2 的新功能须检查扩展、对应 feature 与入口；查询属性本身不启用功能。
Vulkan11 / Vulkan12 properties 集中查询并保存，`MaxBufferSize` 统一取 Vulkan11 的 `maxMemoryAllocationSize`，
不查询 maintenance4 属性，也不依赖 `VK_KHR_maintenance4`。这个值表示单次内存分配上限，
用于 RHI buffer 大小的预检查，不保证该大小的 buffer 一定可创建；实际创建仍受资源要求和可用内存限制。
初始化日志输出实际启用集、官方 driver ID/name/info/conformance 与原始 `driverVersion`，不按厂商猜测版本编码。

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
Vulkan 支持合法的 unnormalized coordinates 与整数 border，D3D12 拒绝这两种配置；D3D12 mirror-clamp 映射为 MIRROR_ONCE。
普通、缓存、immutable sampler 在可选校验层复用相同参数检查：枚举、有限 LOD/bias、LOD 范围、anisotropy 上限，
compare 只能搭配 WeightedAverage；unnormalized 必须满足 min/mag 相同、mip nearest、LOD 为零、禁止 compare/anisotropy 等限制。
`DeviceFeatures` 报告 SamplerAnisotropy、SamplerAnisotropyPointMip、SamplerMirrorClampToEdge、SamplerFilterMinmax、
SamplerUnnormalizedCoordinates 与 SamplerIntegerBorderColor；limits 报告 anisotropy 上限与 bias 范围。
D3D12 的 anisotropy + nearest mip 还取决于 `AnisoFilterWithPointMipSupported`，不静默换为 linear mip；
min/max reduction 依照设备 tiled-resource tier 的对应能力报告。Vulkan 读取实际 Enabled features。
`TextureSupport::MinMaxFiltering` 还检查实际采样格式的支持，不能只复制设备 feature；Vulkan 整数格式是否支持同样以格式位为准。
纹理与 sampler 的最终配对、坐标和 shader 使用方式由调用方检查，不增加 reflection 或自动配对系统。
关闭 RHI 校验时调用方保证参数合法；native 创建失败仍返回空。sampler cache key 包含全部描述符字段。

Shader 是否使用全部槽位不影响布局。调用方负责字节码与显式布局的 ABI 匹配，以及跨完整布局使用参数集时的
原生兼容性。多个 PSO 可共享一个完整布局；多个完整布局可共享同一组布局和参数集。
可选 RHI validation 仅检查槽位存在、可设置的资源类型、数组/buffer 范围、动态偏移、device 归属和必要调用状态；
不按完整结构、hash、原生坐标或对象身份判断布局兼容，不执行完整 shader ABI 校验。

### D3D12 组存储与 Root Signature

组布局不创建 RS；它按 BindingId 排序，在组内按 `(heap 类型, shader visibility)` 建立稳定的 descriptor tables，
并分别保存 root buffer 和固定 sampler 项。CBV/SRV/UAV 可以共表，sampler 单独成表。
创建完整布局时先放 push constants，再组合各组的 root descriptors、tables 和固定 sampler；
保存 GroupSlot 到本次 root parameter 编号的映射，以 RS 1.1 描述并按设备支持版本序列化，必要时降为 RS 1.0，
再调用 `CreateRootSignature`。降级使用官方 `D3DX12SerializeVersionedRootSignature` helper。
`RootSigD3D12` 直接持有 `ComPtr<ID3D12RootSignature>`，序列化数据只作为创建输入。
每次创建返回独立的 RHI owner 并调用原生创建接口，但相同描述可由 D3D12 复用同一 COM 对象；
独立所有权指各 owner 持有并释放自身引用，不保证原生指针不同。销毁一个 owner 不影响另一个。
根签名超过 64 DWORD 或原生创建失败返回空值。

参数集每个 table 分配连续 CPU mirror 与 shader-visible 区间，组内 offset 不随完整布局变化。
Set 更新 mirror 和 dirty ranges；FlushWrites 只发布 dirty 部分，并避免跨未初始化空洞批量复制。
root buffer 保存 GPU 基地址，绑定时加偏移。绑定根据当前 PSO 的完整布局映射下发，不重排参数集 descriptor 存储。

RawBuffer / RWRawBuffer 的 StructureByteStride 必须为 0，range 按 4 字节对齐。D3D12 structured buffer
stride 不超过 2048 字节；typed buffer 校验实际 DXGI format 的 buffer 及对应 SRV/UAV 支持。
不同完整布局可以给同一组不同 root 编号，调用方共享组布局即可保留稳定的组存储。

### Vulkan 组布局与参数集

`BindingGroupLayoutVulkan` 直接创建并拥有一个 `VkDescriptorSetLayout`，保存 descriptor 数量和按 native binding
排序的动态偏移顺序。固定 sampler 从 device sampler cache 取得，必须在 cache/device 销毁前释放全部组布局与参数集。
参数集只从组布局取得 handle、pool sizes 与槽位信息，通过现有 descriptor allocator 分配；不访问完整布局。
只有 immutable sampler 的非空组同样需要创建并绑定参数集；固定 sampler 在分配 descriptor set 时初始化，
不会随 pipeline layout 自动绑定。校验层对 Vulkan 的每个非空声明组检查绑定，D3D12 的纯静态 sampler 组无需绑定。
`PipelineLayoutVulkan` 借用组布局，直接创建并拥有 `VkPipelineLayout`。set 0..最大 set 之间的空洞使用该完整布局
私有的一个空 `VkDescriptorSetLayout`；同一对象的多个空洞共用它，其他完整布局各自创建。
创建完整布局时，普通与动态 uniform/storage descriptor 分别检查全局数量限制；逐 stage 限制合计两者。

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

Vulkan graphics PSO 按实际 Enabled features 检查 depth clamp、depth bias clamp、非实心填充、
independent blend 和 dual-source blend，并检查保守光栅化扩展；不支持的请求返回空值。
`PrimitiveState::Default()` 的 `UnclippedDepth=true` 同样受此约束。两个后端的 blend constants
固定为 `{0, 0, 0, 0}`；Vulkan 由 PSO 静态状态提供，D3D12 在 command buffer Begin 时设置。
RHI 暂无动态设置入口。D3D12 多颜色附件启用 independent blend，使每个附件自己的 blend 和 write mask 生效；
write mask 支持 RGBA 的所有子集。

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

D3D12 的 `ResolveTexture` 只允许 Direct command list，并要求格式支持 MULTISAMPLE_RESOLVE。
当前 timestamp query pool 使用普通 TIMESTAMP heap，仅支持 Direct / Compute，Copy queue 上的查询操作由
校验层拒绝；若将来支持 copy timestamp，需要单独的 COPY_QUEUE_TIMESTAMP heap 契约。

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
view 固定 depth/stencil 各 aspect 的写权限：`DepthRead` 全只读，`DepthWrite` 全可写，
`DepthReadStencilWrite` 与 `DepthWriteStencilRead` 仅用于同时包含两种 aspects 的格式。
barrier 描述实际 Before/After 状态，创建或绑定 view、Begin/End pass 不转换状态。`TextureState` 的两个同名混合状态
只允许覆盖双 aspects，附加 ShaderRead 仅为只读 aspect 增加 shader 访问；单 aspect 的 DepthRead/DepthWrite 只指所选 aspect。

`IndependentDepthStencilTransitions` 仅表示独立转换能力，不表示混合权限能力。Vulkan Enabled
`separateDepthStencilLayouts` 时按 aspect 生成 barrier，并为 RenderPass2 使用独立 stencil layout 结构；
关闭时仍以 Vulkan 1.1 混合组合 layout 支持两种混合权限，但 DS image 的 barrier 必须明确覆盖双 aspects、给出组合 Before/After。
单 aspect 的独立转换明确拒绝，绝不扩大范围或猜测另一 aspect 状态；纯 depth 格式不受双 aspect 限制。
D3D12 使用原有 plane subresource transitions，把混合状态拆到各 plane，未变化的 plane 不生成 transition。
规则见 [Vulkan barrier 规范](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageMemoryBarrier.html)。

Vulkan 单 aspect SRV 在独立路径使用对应 depth/stencil read-only layout，组合路径使用 DEPTH_STENCIL_READ_ONLY_OPTIMAL；
后者依照[单 aspect 布局匹配规则](https://docs.vulkan.org/spec/latest/chapters/resources.html#resources-image-layouts)
与混合附件 layout 匹配。无需公共 descriptor layout 字段，Begin 不改写 descriptor。
Vulkan 同时采样只读附件的保证限于 fragment shader 读取当前 fragment 对应的样本；任意坐标、其他 shader 阶段
的同时访问不在本路径契约内。无 shader reflection 时由调用方保证这种访问方式，见
[只读附件采样规则](https://docs.vulkan.org/spec/latest/chapters/renderpass.html#renderpass-feedbackloop)。该后端限制不施加于 D3D12。
DepthRead 的 Vulkan access mask 同时覆盖 attachment read/write：传统只读附件的 Store 操作仍使用
depth/stencil write access，离开只读 pass 的 barrier 必须等待该操作；这不会允许 PSO 写入只读附件。
该要求见 [VkAttachmentStoreOp](https://docs.vulkan.org/refpages/latest/refpages/source/VkAttachmentStoreOp.html)。
`NormalizeSubresourceRange` 同时拒绝格式不具备的 aspect、零 count、越界和加法溢出。

`BarrierTextureDescriptor::Range` 是纹理 barrier 范围的唯一来源，默认 `SubresourceRange::AllSub()`
覆盖整张纹理；count 为 `SubresourceRange::All` 时表示从对应 base 到末尾。两个后端使用归一化后的
范围，D3D12 transition 按 plane/mip/layer 展开；D3D12 UAV
barrier 仍作用于整资源。

Buffer barrier 携带 `BufferRange`，Vulkan 映射 offset/size，D3D12 legacy barrier 保守转换整 buffer。
Texture/Buffer barrier 都携带 BeforeStages/AfterStages；Vulkan 的 UNKNOWN shader 阶段只覆盖实际
queue family 支持的阶段，专用 compute family 只映射 Compute。显式阶段与非 shader 阶段仍须匹配
family；校验层拒绝不支持的阶段以及专用 transfer family 上的 shader/UAV barrier。Vulkan 只收窄 shader
stage，保留 transfer、host、attachment 阶段。ShaderRead 对应普通 shader resource，不附带未公开的
input-attachment access。

D3D12 ShaderRead 的 UNKNOWN stages 在 Compute command list 上只生成 NON_PIXEL_SHADER_RESOURCE；
Direct 默认覆盖 pixel / non-pixel，显式 stages 可收窄范围。校验层拒绝队列不支持的状态、显式 shader stages、
Copy queue UAV barrier，以及写状态与其他读写状态混用；转换不自动增加队列之间的 GPU 同步。

`PushDebugGroup` / `PopDebugGroup` 必须平衡，分别映射 D3D12 command-list Unicode event 与
Vulkan debug-utils label；Vulkan 未启用 debug-utils 时不录标签。

启用 `RADRAY_ENABLE_PROFILER` 时这对调用同时是 GPU 时间戳 zone 的边界：每个 Direct/Compute queue
在构造时创建一个 Tracy GPU context（Copy queue 不采样）；Vulkan 还要求实际 family 的
`timestampValidBits` 非零，否则不创建 context，保留普通 debug label。命令缓冲持有已打开 zone 的栈，`End()` 关闭
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

buffer offset limits 公开 constant、storage 与 texel buffer 对齐：D3D12 分别报告 256、4 与 1
（typed view 仍需按格式元素对齐），Vulkan 分别来自 `minUniformBufferOffsetAlignment`、
`minStorageBufferOffsetAlignment` 与 `minTexelBufferOffsetAlignment`，typed buffer 不套用 storage 对齐。
Vulkan 的 `MaxStorageBufferRange` 来自 `maxStorageBufferRange`；校验普通与动态 storage descriptor 的
最终 range，包括 `BufferRange::All()` 展开的剩余长度。该字段为零表示未报告限制，不限制 buffer 分配大小。
`UavWriteStages` 按 shader
stage 报告 storage write 能力：Compute 必有；D3D12 Pixel 必有并在 Feature Level 11_1 以上加入
Vertex，Vulkan 只在实际启用 `fragmentStoresAndAtomics` / `vertexPipelineStoresAndAtomics` 时加入对应
阶段。Vulkan 特性语义以
[`VkPhysicalDeviceFeatures`](https://docs.vulkan.org/spec/latest/chapters/features.html) 为准；上层必须在
录制前拒绝缺失阶段，不能静默降级。

D3D12 通过 `D3D12DeviceDescriptor::QueueCounts` 在 device 创建阶段建 queue，默认仅一条
Direct queue；`GetCommandQueue` 只查询已创建的 slot，不再隐式增加 queue。Vulkan 仍使用
`VulkanDeviceDescriptor::Queues`，Dedicated 仅对确实来自专用 family 的 queue 为真。
Vulkan Direct 必须同时支持 graphics 和 compute；Copy 接受显式 transfer 或 graphics/compute 隐含的 transfer 能力。
每项请求按 family 顺序使用剩余 queue 容量，同一 family 可分配多个 queue，所有请求共享容量计数；
数量不足或没有请求任何 queue 时创建失败。各 family 的 priority 数组一次性分配，在 `vkCreateDevice` 返回前保持地址稳定。
D3D12 无法报告专用物理引擎，Dedicated 为 false。buffer size 报告 backend 的资源大小上限，
不保证当前预算下的 allocation 一定成功。

`QueryTextureSupport` 的查询条件为 dimension + format + usage，返回 sample mask、filter/blend
事实及该组合的 extent/layer/mip/resource-size 上限。每次查询直接读取 native device/physical-device
能力，device 创建时不预热，也不缓存查询结果。D3D12 查询 format support 与
MSAA quality；Vulkan 查询相同 image flags/usage 的 image format properties。`Resource` 只映射
sampled usage，RHI 没有 input attachment 使用接口。

`ValidateTextureDescriptor` 共享 CPU 规则与 backend support facts；开启 RHI 校验时，`CreateTexture`
先校验再分配。非法 dimension/format/usage、零尺寸、超 mip/extent、MSAA+mips、MSAA+UAV、
非 Device texture memory 或不支持的 allocation hint 返回包含字段的 reason。
validation 不替换 format，也不降低 sample count。资源不足/native create 失败仍返回空。
`TextureViewDescriptor::Format` 是调用方请求的 view 格式，不要求等于 texture 的逻辑格式；公共 RHI
不规定两后端格式兼容范围的交集，也不自动转换格式。`QueryTextureSupport` 仍查询纹理创建能力，
不代表所有重解释 view 都可用。纹理描述不提供允许格式集合。

开启 RHI 校验时，通用层检查 owner、枚举、维度、范围、aspect 与声明用途，再以解包后的描述调用
所属后端的 `ValidateTextureView`。后端判定格式兼容及对应用途支持，失败记录原因并返回空。
关闭校验时仍由调用方保证合法性，不增加常驻校验包装或全局状态。

view 维度与层范围也在 RHI 校验层按后端检查：Vulkan 的 1D/2D 非数组 view 可选择数组纹理的任意合法
单层，Cube view 可选择 Cube/CubeArray 纹理中任意合法的连续六层；Cube/CubeArray view 要求目标
创建维度为 Cube 或 CubeArray，以保证原生 cube-compatible 标志。D3D12 当前的非数组及 Cube view
仍要求起始层为零，非零起始层使用对应 array view。3D view 的数组起始层必须为零，层数为一。

- Vulkan 自建 image 保留 mutable flag，格式重解释遵循 Vulkan 格式兼容类；depth/stencil 不与同字节数
  的 color 格式混为一类。非 mutable image（包括当前 swapchain image）要求原生格式一致。
  每个 view 用 `VkImageViewUsageCreateInfo` 将有效 usage 收窄为自身的 sampled、storage、color 或
  depth/stencil attachment 用途，并检查该格式的 optimal-tiling feature。sRGB sampled view 不继承
  同一 image 上其他 view 使用的 storage usage。Image/View 保存实际创建 flags、usage 和归一化子资源范围。
- D3D12 自建资源保留现有 typeless 映射，按实际资源格式所属家族及 view 类型检查 DXGI format support；
  fully typed 资源遵循其限制。保留深度/模板 SRV 的 plane 映射，以及 swapchain UNORM 存储到 sRGB RTV
  的既有路径。例如 RGBA8_UNORM image 的 R32_UINT sampled view 可属于 Vulkan 同一兼容类，却不属于
  D3D12 同一 typeless 家族；同一请求允许有不同后端结果。

本路径不接入 `VkImageFormatListCreateInfo`；它可能帮助部分实现优化 mutable image，但不据此宣称已有
性能问题或保证收益。原生规则见 [Vulkan view usage](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewUsageCreateInfo.html)
与 [image format list](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_image_format_list.html)。

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

每次创建或重建先用 `vkGetPhysicalDeviceSurfaceSupportKHR` 检查指定 present queue 的实际 family。
surface 未固定 extent 时将请求尺寸限制在 min/max 范围内；固定时使用 `currentExtent`。
零 extent（例如最小化）不调用 `vkCreateSwapchainKHR`：首次创建返回空值，重建返回 false，
保留尚未退休的旧链并让 Acquire 返回 RequireRecreate，待窗口恢复后重试。

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

初始化与能力选择遵从 [Vulkan API 与 Feature 策略](#vulkan-api-与-feature-策略)。
单 subpass VkRenderPass 使用 Vulkan 1.2 核心 RenderPass2 创建与 begin/end 接口；附件操作、layout
和 PSO 兼容语义不变。Framebuffer 在 device 初始化时按 Enabled 固定为 imageless 或传统模式，
不新增公共模式开关；独立 depth/stencil layouts 同样读取 Enabled，不启用 dynamic rendering 或 synchronization2。

公共 RHI 没有 RenderPass/Framebuffer owner、对应 Device 工厂或 PSO 的兼容 pass 指针。RenderPassBeginDescriptor 直接描述有序 color views、可选 depth/stencil view、load/store、clear、extent/layers、render area、name 与 UAV writes。开启 RHI 校验时，ValidateRenderScope 在任何 native Begin 之前校验 view/device、格式角色、usage、sample、mip/layer、extent、重复重叠与只读操作。

PSO 的 AttachmentSignature 从现有 ColorTargets、DepthStencil、MultiSample 字段派生，只含有序格式、depth/stencil 格式及共同 samples。load/store、clear、尺寸、area 和 name 不进入 PSO 兼容身份。绑定时检查当前 scope 的签名；resolve 也不进入光栅化签名，同一 PSO 可用于兼容的有/无 resolve pass。只读 aspect 必须 Load + Store，拒绝 Clear/Discard，PSO 按 depth/stencil 分别检查写入。pass 没有重复的 ReadOnly 字段；DSV flags、原生 render pass flags 和布局从 view 权限推导。

`CommandBuffer::BeginRenderPass(desc)` 经可选校验后获取原生对象并开始录制，无独立准备阶段或 token。
未处于 recording、已有 active encoder、附件校验或原生对象获取失败时返回 `nullptr`，不录制 clear/Begin，
不改变 active encoder，调用方可以修正描述后重试。描述及其 span 只借用到调用返回；附件 owner 仍须存活至命令真实完成。
调用方在 BeginRenderPass 前录制附件 barrier，失败时仍负责处理这些已录制的状态转换。
D3D12 首次 Begin 时缓存该命令的 `ID3D12GraphicsCommandList4`；附件描述调用时被原生快照，resolve subresource 参数数组由 encoder 保持到 EndRenderPass。
局部 area（含 pass 尺寸小于 mip）的 clear 在所有校验完成后用带 rect 的 Clear，Load/Discard 保守映射为 preserve；
pass 只覆盖 view 的部分 layers 时使用局部 RTV/DSV，避免清除其余层。Vulkan 使用 renderArea，
只有 pass 和 framebuffer 均获取成功后才加入命令的去重引用。

VulkanRenderPassCache 由 DeviceVulkan 独占，借用 device。execution key 包含格式、samples、load/store、view 权限、resolve 槽位与模式；depth-only 格式规范化无 stencil 的操作。PSO 创建临时借用代表 pass，成功后不长期保留；framebuffer 也不保留创建 pass 的 owner。

实际附件顺序固定为颜色源、可选 DS 源、按颜色槽顺序排列的 resolve 目标、可选 DS resolve 目标。
传统 framebuffer key 包含所有源/目标的有序稳定 view ID、实际附件格式、光栅化签名及 extent/layers。Imageless key 不含 view ID，
改为相同的有序格式/光栅化签名、framebuffer extent/layers 和各附件实际 image flags、view 有效 usage、所选 mip 尺寸及
view 层数；两者均不依赖某个 load/store 版本的 pass 指针。Imageless 创建时各附件的 `pViewFormats`
只声明该槽位实际使用的 view 格式，不是纹理创建时的允许格式集合；Begin 通过
`VkRenderPassAttachmentBeginInfo` 提供实际 views。同约束的新 view 可复用旧 framebuffer。
Swapchain image 的 flags/usage 来自实际创建参数，其逻辑 usage 不再宣称未创建的 sampled 用途。
预算耗尽或原生创建失败仍返回空，不静默切换 framebuffer 模式。

每条 command 在 epoch 内对 pass 和 framebuffer 去重持有 intrusive use。End、Return、CPU Submit 和取消通知都不解除引用；真实完成以后 native allocator Reset 才使录制失效并释放引用。缓存零引用条目进入 LRU，引用数表示使用而非缓存常驻。默认总预算 RP=256/FB=1024，闲置预算 RP=128/FB=256；压力只淘汰闲置条目，live 条目不可驱逐，到顶时 BeginRenderPass 返回空。缓存清理不等待 GPU idle。

view ID 在该 device 内单调且不复用。两种模式均按 command epoch 去重持有附件 view ID，缓存以独立
使用计数跟踪这些引用；仅在 pass/framebuffer 获取成功后提交。End/Submit 不释放，原生命令池 Reset
或 command buffer 释放后才解除，已提交命令必须先真实完成。view 销毁先检查独立使用计数，传统模式
再按局部 reverse index 清理相关闲置 FB，imageless FB 不随 view 销毁失效。mutex 串行化
acquire/release/invalidate/trim 和附件使用计数；缓存不持有业务 texture/view 或 device shared owner。
调用方继续持有附件 owner，校验包装保留 command → view → texture 依赖，不新增隐式 GPU 等待。

### 附件 resolve 与内容保留

颜色附件的可空 `ResolveTarget` 只提供非整数格式 Average（含设备支持的归一化、浮点、sRGB）。
DS 附件的可选 `DepthStencilResolveDescriptor` 携带完整 DS 目标 view 与 DepthMode/StencilMode：None、SampleZero、Average、Min、Max；
至少一个模式非 None，缺失 aspect 必须为 None。`QueryAttachmentResolveSupport(format, samples)` 返回颜色 Average、
DS 模式集合及 IndependentResolve/IndependentResolveNone 组合约束；调用方选择模式，后端不替换或模拟。
源/目标 view 格式、mip 尺寸和 view 层数必须相同，源 MSAA、目标单采样；目标为 RenderTarget 或完整可写 DepthWrite view，
不得与当前其他附件非法重叠。resolve 覆盖 pass render area、同坐标目标，无独立目标偏移。

附件 resolve 与 `ResolveTexture` 独立命令不同：源保持附件状态，目标由调用方准备 `AttachmentResolveDestination`，
不能用独立命令的 ResolveDestination 替代。End 不转换为 ShaderRead。DS 目标覆盖的两种 aspects 都必须显式准备目标状态；
None 保留内容，不恢复其此前资源状态。

| 对象 | pass 完成后的内容保证 |
|---|---|
| 源 Store | 保留所选 aspect 的全部 MSAA samples |
| 源 Discard | render area 内不保证，但 resolve 必须先读取有效源；后端可以保留更多 |
| resolve 目标 | 保存 render area 内结果，无目标 Discard 选项 |
| render area 外 | 保留源和目标原内容；调用方先初始化，不能用 Undefined 放弃内容 |
| 目标 None aspect | 整个所选子资源内容不变 |
| 源未 resolve aspect | 继续遵守其自身 Load/Store |

这些是结束后的值保证，不允许 pass 内通过别名并发访问这些区域。
Vulkan 颜色使用 pResolveAttachments，DS 使用 VkSubpassDescriptionDepthStencilResolve，按设备 modes、
independentResolve 与 independentResolveNone 检查；目标使用 Load/Store 保留既有内容、initial/final layout 不变。
颜色与 DS resolve 都按 COLOR_ATTACHMENT_OUTPUT 和 color-attachment read/write 同步，DS 源状态保守包含 resolve 的读取阶段；
只读附件 Load/Store 仍有原生访问，不等于无写 access。参见
[Render pass resolve](https://docs.vulkan.org/spec/latest/chapters/renderpass.html)。

D3D12 使用 ending access RESOLVE，为实际处理的 plane/layer 持有 subresource/rectangle 数组；Store 设置 PreserveResolveSource，
局部区域 Discard 也保守保留源。resolve 的 Format 按 plane 复用 SRV 的类型化格式映射：颜色保持 view 格式，
depth/stencil 不直接沿用完整 DSV 格式。例如 `D32_FLOAT_S8_UINT` 的两个 plane 分别使用
`R32_FLOAT_X8X24_TYPELESS`、`X32_TYPELESS_G8X24_UINT`。能力查询要求 RenderPassesValid、ProgrammableSamplePositionsTier、格式/MSAA 支持，
depth 报告 Average/Min/Max，stencil 报告 Min/Max，无 SampleZero；实际格式支持决定可用性。能力不足明确拒绝，
不降级独立命令或 shader 模拟。原生规则见 [ResolveSubresourceRegion](https://microsoft.github.io/DirectX-Specs/d3d/ProgrammableSamplePositions.html#resolvesubresourceregion)。
resolve 目标与源一起参与两种 framebuffer cache、失效索引和 command 去重引用；End、EndRenderPass、Submit 不释放引用，
Reset/释放前已提交工作必须真实完成，失败的 Begin 不留下 pins。不新增隐式 GPU 等待。

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
| `AttachmentOperations` | 混合 DS 权限与同 fragment SRV、PSO 拒绝、局部颜色/DS resolve、逐 sample 源保留、区域外/未 resolve aspect、sampler 普通/immutable 读回与能力限制 |
| `ExplicitBindingLayoutTest` | 双后端真实 dispatch/读回、不同 DX/VK 坐标、set 空洞、数组、各类 buffer、texture、普通/固定 sampler、动态偏移、部分及多块 push、组/参数集共享 |
| `ExplicitLayoutOwnershipD3D12` | 通过原生 debug 消息观察独立创建与最终释放，不依赖句柄比较 |
| `ExplicitLayoutOwnershipVulkan` | 测试侧函数表 probe 观察独立创建、私有空洞、幂等释放和部分失败清理 |
| `DescriptorDirtyRangesD3D12Test` | 描述符 dirty range 合并与未初始化空洞 |
| `D3D12DiagnosticsTest`、`D3D12DiagnosticsSmoke` | callback/基础队列、DXGI 原生错误、采集完整性 smoke |
| `VulkanSwapChainRetirementTest`、`VulkanSwapChainNativeTest`、`VulkanSwapChainWsiTest` | 保留的交换链回收与 WSI 验证 |

render 测试只使用版本控制的原始 stage 字节码和手写布局；artifact/contract 测试归 shader 模块。
测试侧 probe/原生诊断负责观测，生产对象不添加测试计数。
后端不可用与初始化失败的判定见[GPU 测试规则](../guide/build-test.md#测试)；必测后端缺失或跳过不得算作通过。
