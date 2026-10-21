> - 适用: 查阅共享文档中原 runtime 接入与使用约束
> - 权威: 2026-10-21 基线摘录，非当前契约；保留完整段落作为上下文
> - 锚点: `docs/history/2026-10-21-runtime-archive.md`

# 共享文档中的 runtime 说明

状态：已归档。基线：`751aca4e6a70203e6a62f9de67893b33463b94fc`。旧源码、测试名和现状均限定于此基线。

## architecture/render-rhi.md

`Device::Create(desc, RhiValidationOptions)` 在创建真实后端后按固定配置决定是否包装。
`RhiValidationOptions::IsEnabled` 在 Debug（定义 `RADRAY_IS_DEBUG` 且未定义 `NDEBUG`）中默认开启，
Release、RelWithDebInfo、MinSizeRel 默认关闭；
调用方可以显式覆盖。`LogCallback` / `LogUserData` 接收 RHI 误用诊断；未提供回调时写项目错误日志。
回调可能由不同 worker 调用，接收者负责线程安全并存活至 device 和对象销毁；回调不重入同一对象。
Application 的 `GpuOptions::RhiValidation` 经 `GpuSystemDescriptor::RhiValidation` 透传。
该开关独立于 D3D12 debug layer、GBV 和 Vulkan validation / synchronization validation。

失败检查在终止前先收集验证消息，再报告 HRESULT 与设备状态。正常诊断边界覆盖原生对象创建、
view/sampler 创建、descriptor `FlushWrites`、command list Close/Reset、allocator Reset、
ExecuteCommandLists 和 Submit 收尾，以及 Present/ResizeBuffers。Fence 的新完成值和 `WaitFor`
返回时收集 GPU 侧消息，因此 runtime 的 flight 完成轮询也能看到 GBV 诊断；收集不增加 GPU 等待。
[GBV 输出发生在 GPU 执行后](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-d3d12-debug-layer-gpu-based-validation#debug-output)，
提交时的一次收集不能代替完成后的收集。测试应在真实 GPU 完成并收集消息后断言验证错误数。

Fence 查询把 `UINT64_MAX` 视为设备移除，禁止上层将其比较为成功完成。
`Fence::WaitFor(value, timeoutMs)` 是有限 CPU 等待，0 只查询；false 表示超时，不能作为完成。
即使 timeoutMs 为 UINT64_MAX，也钳制到后端可表达的有限时长。调用方串行等待同一 fence。
D3D12 检查事件注册、系统等待结果，并在唤醒后重新核验完成值；设备移除和较早超时的事件
注册也能唤醒事件。Vulkan 使用有限 vkWaitSemaphores。兼容 Wait() / Wait(value) 的期限为
30 秒；超时明确终止，不在 GPU 仍可能使用资源时继续销毁。上层可用 WaitFor/poll 做可取消关闭。
这些检查在检测到错误的边界终止，避免继续发布成功的 flight 完成通知；不提供设备恢复。
D3D12 flip-model 不能对已最小化、已隐藏、已销毁或客户区为 0 的 HWND 提交 GPU 写入，也不能
对其调用 `Present`：驱动会以 `DXGI_ERROR_ACCESS_DENIED` 移除设备。Acquire 在这种 HWND 上
返回 `RetryLater`。runtime 已 acquire 的帧在 submit 时若窗口已不可呈现，只跳过该目标 batch，必要更新和独立离屏工作继续提交；Present
跳过 DXGI Present 并消费 token，返回 RetryLater，不等待 present queue；提交完成仍须实际 fence
证明。正常 Present 使用 [DXGI_PRESENT_DO_NOT_WAIT](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present)，
DXGI_ERROR_WAS_STILL_DRAWING 返回 RetryLater，保留未排入呈现的 readiness；不会当成 device loss。
同一帧有多个 D3D12 flip HWND 时，每个 HWND 使用独立
present command buffer：对该窗 `Execute` 后立刻 `Present`，不在 Present 前等待 graphics queue。
Vulkan 先提交全部应用批次，再逐目标 Present；批次合并与 final signal 见[帧与 GPU](2026-10-21-runtime-frame-and-gpu-snapshot.md#命令分配与有序批次)。
runtime 窗口通过 WindowManager 的协程接口修改 surface，由 runner 在维护阶段统一排空旧 CPU
工作及主队列（含排在帧 fence 之后的 Present）。NativeWindow 的修改前通知只检查阶段，不执行等待。
不得用原始 `ShowWindow` 等 API 绕过此约束，也不要在 Win32 钩子或 `WndProc` 里等待渲染线程。
接口与完成语义见[窗口修改协程](2026-10-21-runtime-frame-and-gpu-snapshot.md#窗口修改协程)。

`CommandBuffer::CopyBufferToTextureRegion` 接受 `BufferToTextureCopyDescriptor`，其中
`BufferTextureCopyRegion` 明确源 byte offset、row pitch 和目标 mip、array layer、X/Y、宽高。
`ValidateBufferTextureCopyRegion` 验证单采样、非压缩 color 2D/2DArray、copy usage、子资源与区域边界、
设备 pitch/placement 对齐及源 buffer 范围。源内容从指定 offset 开始，只上传区域的行；
目标区域外的 texel 保持原值。D3D12 映射到 placed footprint + CopyTextureRegion，Vulkan
映射到 VkBufferImageCopy + vkCmdCopyBufferToImage。不支持的 command buffer 实现返回 false。
Graph 对应 typed Pass 见 [Renderer foundation](2026-10-21-runtime-renderer-foundation-snapshot.md)。

`Device` 之所以是 `shared_ptr`：后端实现对象需要回指 device 拿分配器、描述符堆和函数表，
而 device 的生命周期由上层（例如 `GpuSystem`）管理。RHI 不为每个资源公开共享工厂；上层可以显式共享
一个独占创建的 owner（例如 TextureAsset 的共享 immutable texture），仍须满足全部使用者的完成条件。
native layout 复用归设备缓存，runtime 的 program cache 持有独立的 RHI layout 包装。

D3D12 storage 在创建时持有唯一的 `ID3D12CommandAllocator`，创建失败返回空值；不自动池化或扩容。
同一 allocator 同时最多一条 list 处于 Begin/End 区间，关闭后可以继续顺序录制其他 child，无需先提交或等待 GPU。
开启 RHI 校验时，第二条重叠 Begin 在原生调用前被拒绝，且可以在原录制成功 End 后重试；被拒绝的 End 不释放占用。
Vulkan 一个 storage 持有一个未开启逐 buffer reset 的 command pool，绑定真实 queue family；安全复用时整池 Reset。
同线程交错录制不增加 pool。共享 allocator 的全部操作由调用方串行化；独立 storage 可以由独立 worker 录制。
两端直接暴露各自的录制约束，调用方通过 `GetBackend()` 选择分配策略；runtime 池化见[帧与 GPU](2026-10-21-runtime-frame-and-gpu-snapshot.md#命令分配与有序批次)。
共享录制内存不提供 GPU 数据同步，barrier 和 Submit 数组顺序仍由调用方负责。

`GetShaderParameterValueIdentity` 规范化 buffer 的 All range、base offset、stride/format，texture view
的实际子资源范围和 aspect，以及 sampler 的原生完整状态。hash 只定位候选，调用方须做完整值判等，
并额外登记资源版本和寿命，不能用原生指针长期充当可复用的资源 ID。
Render Framework 的完整组封存、缓存预算和真实完成回收见 [Renderer 绑定存储](2026-10-21-runtime-render-framework-snapshot.md)。

allocator 的原生映射与后端限制见[命令录制存储](#命令录制存储)。
Begin 不结束旧 epoch，重复借出同一命令前必须 Reset。
两个后端的 allocator Reset 都要求全部录制关闭、GPU 使用结束；Vulkan 先 reset native pool，
再清理已结束编码器及去重的 RP/FB pin。runtime 在真实完成处理时逐个 Reset 本轮使用的 storage，
下一次 BeginFrameRecord 不再次重置。

Buffer barrier 携带 `BufferRange`，Vulkan 映射 offset/size，D3D12 legacy barrier 保守转换整 buffer。
Texture/Buffer barrier 都携带 BeforeStages/AfterStages，UNKNOWN 保留保守映射；Vulkan 只收窄 shader
stage，保留 transfer、host、attachment 阶段。ShaderRead 对应普通 shader resource，不附带未公开的
input-attachment access。当前 RasterGraph 的显式状态组织见 [Runtime 宿主](2026-10-21-runtime-render-framework-snapshot.md#对象-gpu-参数与当帧视图)；旧 RenderGraph 已移除。

Vulkan 的 acquire wait 使用 `ALL_COMMANDS`。RHI 不限制 acquired image 的首次访问阶段，
布局转换、transfer readback 与颜色附件访问都必须在 acquire 完成之后；不能只等待颜色/计算阶段。
`RuntimeVulkanSwapChain.TransferFirstAcquirePreservesPresentedPixels` 先复制前次呈现的像素，
再重新清除和呈现，验证这条传统同步路径。

`SetViewport(Viewport)` 在 D3D12 直填 `D3D12_VIEWPORT`，在 Vulkan 直填 `VkViewport`，
**两边都不做 Y 翻转**。所以 `Viewport` 的语义是"原点在左上"，与 D3D 一致。
`Viewport` 与裁剪用的 `Rect` 定义在 `modules/core/include/radray/viewport.h`。
`PrimitiveTopology` 定义在 `rhi.h` 的枚举区，成员名和 `int32_t` 底层类型保持稳定；runtime 的
`MeshPrimitive` 使用同一枚举。

## architecture/core-facilities.md

专用的：`json.h`（yyjson）、`xml.h`（pugixml）、`binary_io.h`（小端读写）、`file.h`、`environment.h`、
`dynamic_library.h`、`guid.h`、`text_encoding.h`、`runtime_type.h`、
`allocator.h`（GPU 子分配器，与堆无关）、`memory.h`、`sparse_set.h`、`channel.h`、
`intrusive_ptr.h`、`structured_buffer.h`、`image_data.h`、`viewport.h`、
`triangle_mesh.h`、`wavefront_obj.h`、`camera_control.h`、`platform/win32_headers.h`。

**`GetCoroutineStopToken` 存在的理由**：手写 awaitable 的 `await_suspend` 只拿到
`coroutine_handle`，而 `coroutine_handle<>` 已经把 promise 的 env 擦除了，
于是取不到 stop token。这个函数探测 promise 里有没有 `get_stop_token`，
没有就返回一个 `stop_requested()` 恒为 false 的默认 token（= 不可取消）。
`AssetWaitAwaitable::await_suspend` 为此把自己模板化，见 [asset-system](2026-10-21-runtime-asset-system-snapshot.md)。

`ManualCoroutineScheduler` 的记录不能搬动——记录里存着回指调度器的指针（stop callback）。
帧等待表因此放在 `FrameTimeline::_flights` 的 `vector<unique_ptr<Flight>>` 中，保证调度器地址稳定。

记录的 `Sequence` 由所属 ManualCoroutineScheduler 分配，在该实例内单调递增且不回绕。
`DispatchReady` 先按无副作用的谓词冻结就绪记录的地址与序号，再恢复续体；回调取消其他记录或
新建记录时，旧地址不会误派发给新的等待。恢复后仍存活的原记录由调度器摘除。
跨多个等待表派发时，先分别捕获 `GetSequenceBoundary()` 的排他截止，再逐表派发；
ApplicationScheduler、帧边界等待和资产 Ready 通知共用此派发实现。窗口操作仍保留自己的安全点与交付阶段。

`TaskScope` 不可拷贝不可移动，且析构会阻塞。它必须在它所依赖的系统（例如 `GpuSystem`）
之前析构，否则取消时的析构会碰到已死的 device。

- **`scope_guard.h`** — `ScopeGuard` / `MakeScopeGuard`，`Dismiss()` 取消。移动后源自动 dismiss。
- **`allocator.h`** — `BuddyAllocator` / `FirstFitAllocator`，都是**偏移量式子分配器**
  （返回 `Allocation{Offset, ...}`），给描述符堆和 GPU 内存用。**不是堆分配器**，那是 `memory.h`。
- **`runtime_type.h`** — 独立、稳定的 GUID 声明设施。`RuntimeTypeId` 是 `Guid` 的别名；
  需要稳定标识的类型特化 `RuntimeTypeTrait<T>::value`，显式读取 `runtime_type_id_v<T>` 时会先按
  `remove_cvref_t` 归一化并要求 GUID 非空。它不描述继承、不保存对象描述符，也不建立 GUID 与
  RTTI 的全局映射。对象关系查询使用指针形式 `dynamic_cast`，精确动态类型使用 `typeid`，
  底层系统由 [Application 直接装配](2026-10-21-runtime-render-framework-snapshot.md#application-直接装配)，通过构造参数和 setter 连接；
  仅为这些查询不需要声明 GUID。
- **RTTI 是工程编译约定** — `radray_default_compile_flags` 为每个自有 target 以 `PRIVATE`
  方式设置 MSVC/ClangCL 的 `/GR` 或其他前端的 `-frtti`，仅作用于 C++ 编译。新增库、测试、
  工具、样例与 benchmark 都必须调用该函数；`radray_add_test` 已代为调用。core 不通过链接接口
  传播 RTTI 选项，仓库外使用公共对象查询的 consumer 需自行开启 RTTI。把开关放在现有的
  工程编译函数中，可以统一自有目标而不改变第三方配置，也不把工程策略挂在 core 的公共接口上。
- **`intrusive_ptr.h`** — `IntrusivePtr` + `AdoptRef` / `RetainRef`，依赖 ADL 的
  `IntrusivePtrAddRef` / `IntrusivePtrRelease`。
- **`structured_buffer.h`** — CPU 侧反射式结构化缓冲。新代码用 `TrySetValue`（fail-fast），
  `SetValue` 是旧接口。
- **`text_encoding.h`** — UTF-8 ⇄ wchar。Windows 走 `MultiByteToWideChar`，
  其他平台走 `mbsrtowcs`（依赖 locale，行为可能不一致，有 TODO）。
- **`image_data.h`** — `ImageData` + PNG/JPEG 读写，底层 **libpng / IJG libjpeg**（不是 stb），
  由 `RADRAY_ENABLE_LIBPNG` / `RADRAY_ENABLE_LIBJPEG` 门控。另有
  `CompareImageRGBA8` / `ImageDiffRGBA8` 供测试对比。
- **`binary_io.h`** — 固定小端。reader 越界返回 false 且不消费输入。
- **`channel.h`** — `BoundedChannel` / `UnboundedChannel`，`Complete()` 后写入失败；已有消息仍可读完，排空后读取失败。
- **`sparse_set.h`** — 带世代编号的 handle 容器。
- **`guid.h`** — `NewGuid` / `Parse` / `ToString`，有 `format_as` 与 `std::hash` 特化。

| 文件 | 套件名 |
|---|---|
| `test_buddy_alloc.cpp` | `BuddyAllocatorTest` |
| `test_first_fit_alloc.cpp` | `Core_Allocator_FirstFit` |
| `test_wavefront_obj.cpp` | `Core_WaveObjTest` |
| `test_str_convert.cpp` | `Core_Utility` |
| `test_img_rw.cpp` | `PNG`（仅 `RADRAY_ENABLE_LIBPNG`，需 `RADRAY_ASSETS_DIR`） |
| `test_nullable.cpp` | `NullableTest` |
| `test_inline_vector.cpp` | `InlineVectorTest`（内联零分配、元素寿命、溢出复用、别名插入、复制移动与对齐） |
| `test_intrusive_ptr.cpp` | `IntrusivePtr` |
| `test_enum_flags.cpp` | `EnumFlagsTest` |
| `test_sparse_set.cpp` | `SparseSetTest` |
| `test_structured_buffer.cpp` | `StructuredBufferTest` |
| `test_pod_hash.cpp` | `PodHashTest`, `HashCodeTest` |
| `test_runtime_type.cpp` | `RuntimeTypeIdTest` |
| `test_binary_io.cpp` | `BinaryIoTest` |
| `test_json.cpp` | `JsonTest` |
| `test_json_serializer.cpp` | `JsonSerializerTest` |
| `test_json_deserializer.cpp` | `JsonDeserializerTest` |


## guide/shader-authoring.md

`[RootSignature(...)]` 是可选的跨 target **RootSignature policy**。作者一旦在任一相关 entry
写了它，graphics stages 必须解析为一致且覆盖全部 active resources 的合法 policy；额外的 D3
stable-superset ranges/parameters/static samplers 可以保留。compiler 在 target lanes 之前解析一次
policy：DXIL lane 发布 standard serialized Root Signature，SPIR-V lane 按同一 HLSL declaration
identity 发布 Vulkan layout records。runtime 不解析 DXIL blob 来重建 Vulkan layout。

不要在 pass 中手写 backend attribute、创建 numbered binding wrapper，或维护 sidecar metadata。
HLSL declaration name 是 runtime lookup identity；render layout 会为当前 artifact 生成不透明
`BindingHandle`。descriptor与push declaration都通过`FindBinding(name)`取得handle；handle的group、
binding、namespace或内部index都不是authoring ABI。

group 的语义属于具体调用方，不是 shaderlib 全局规则；runtime 不硬编码 view/material/object 组号。
Renderer 的模板按输入来源强制分为 Scene/Frame、View/Pass、Material、Sampler 四个域；一个实际组不能混域。
例如 object/instance stream 属于 Scene，view matrix 与 visible list 属于 View，材质常量与纹理属于 Material，sampler 单独分组。
编号及布局仍来自 artifact 和 resolved layout，不建立平行 metadata。增加 view/pass/flight 不应改变静态 material/sampler 的组身份。
旧 mixed-group shader 必须迁移 ABI；完整组复用不会合并不同完整组中的局部 D3D12 table。

SceneDraw 的实例寻址契约由 MaterialTemplateDescriptor 显式声明：VisibleInstanceSlots 是 uint structured buffer，
VisibleBase 映射到反射中的 uint push 字段，shader 用 VisibleBase+SV_InstanceID 读取实际对象 slot，draw 的 FirstInstance 为 0。
按 slot 读取的矩阵 stride 为 64；自定义参数 stride/字段须与 InstanceParameterLayout 完全一致，
像素阶段通过 nointerpolation varying 接收 slot。DirectObject 与自动合批许可独立；详见[实例绘制](2026-10-21-runtime-instancing-snapshot.md)。
数值 buffer 使用具名 struct 加 ConstantBuffer<T>，让 artifact type tree 保留根结构与成员 offset。
旧 Forward ABI 与专用 POD 生成器已移除，历史设计见[历史快照](2026-09-14-render-framework-design.md)。

调用方直接准备并上传常量 buffer 字节，必须保证数据与实际 GPU cbuffer 布局一致。
固定功能状态由调用方写入 RHI pipeline descriptor。布局和参数契约见
[Runtime shader](2026-10-21-runtime-render-framework-snapshot.md#shader-program-与参数)。

GPU runtime pass 由 `RadRayRuntimeShaderJit` 验证；没有可用 GPU 时测试可以 skip，但已创建设备
后的资源、pipeline、提交或 readback 错误必须失败。


## guide/dev-env.md

以上参数用于 `example_scene_sync`，用 `--vulkan` 切换后端；`example_framework_stress` 的验证开关是 `--validation`。
先选择对应 CMake launch target。Windows `PATH` 用分号分隔，并包含已部署 DXC 运行库的目标输出目录。

## guide/cpp-conventions.md

| 类别 | 风格 | 例 |
|---|---|---|
| 类型（class / struct / enum） | PascalCase | `RenderPipeline`, `GpuSystem`, `ScopeGuard` |
| 函数与方法（含自由函数） | PascalCase | `GetWorld`, `BeginFrameRecord`, `PerspectiveLH` |
| 私有成员变量 | `_` 前缀 + camelCase | `_activePasses`, `_renderStateCreated` |
| 公开数据成员（descriptor / POD） | PascalCase | `BufferDescriptor::Usage`, `GpuMesh::DrawData::VertexBuffers` |
| `constexpr` 常量 | `k` 前缀 + PascalCase | `kShaderAssetFormatVersion`, `kMaxPointLights` |
| 宏 | ALL_CAPS | `RADRAY_ABORT`, `RADRAY_FORWARD_OBJECT_GROUP` |
| 命名空间 | 全小写 | `radray`, `radray::render`, `radray::detail` |

文件内辅助函数有两派并存，**以文件为单位自洽**：render/shader 后端用 `static` + `_` 前缀
（`_CreateVkSwapChain`），runtime/core 用匿名 namespace + 无前缀。改哪个文件就跟那个文件。

`.cpp` 内部实现放匿名 namespace（runtime / core）或文件级 `static`（render / shader 后端），
见上文命名一节。

runtime 的普通 owner 使用公有构造 + `make_unique`（例如 `RenderSystem`、`AssetManager`）。
需要报告启动失败的设施使用结果入口：`GpuSystem::TryCreate` / `TryAttachDevice` 填写
`RuntimeStartupResult`，`RenderSystem::OnInitialize` 也可能失败；不能把 runtime 构造一概视为不会失败。

**两段式析构（`Destroy()` + `DestroyImpl()`）集中在 render 后端实现类**，理由见
[RHI 所有权](../architecture/render-rhi.md)。runtime 的 World/Actor/Component 先由显式驱动器注销与 teardown，
再析构成员；析构不恢复业务 hook 或隐式推进 S1。普通资源包装仍使用析构函数与成员声明顺序。

性能改动使用适用的基准或 Tracy 数据验证，不凭直觉声称收益；旧 runtime_profile harness 已移除。

测试源码放 `modules/<module>/tests/test_<topic>.cpp`。同一模块的这些源文件链进一个可执行文件
（`test_radray_core`、`test_radray_render`、`test_radray_runtime`、`test_radray_shader_compiler`），
`gtest_main` 和被测库只链接一次。CTest 仍按每个 `TEST()` 启动进程。只有不能和其它用例共享进程映像的测试另开 exe：
目前是 `test_inline_vector`，它替换全局 `operator new` / `operator delete`。`EXPECT_DEATH` / `EXPECT_EXIT`
已经在子进程里跑断言，不因此再拆可执行文件。

```cmake
radray_add_test(test_radray_runtime SOURCES test_foo.cpp test_bar.cpp LINK_LIBS radrayruntime)
```

| 设施 | 在哪 | 用途 |
|---|---|---|
| `ManualGate` | `runtime/tests/test_asset_slot.cpp` | 手工协程闸门。`async_scope::spawn` 会同步跑完只有 `co_return` 的 task，所以要挂住协程必须有它。写 `co_await gate.Wait()`，直接 `co_await gate` 会拷贝它 |
| `ProbeAsset` + `DestroyGuard` | 同上 | 不碰 GPU 的假资产，计数 `OnUnload` 与析构 |
| `Scoped*`（`ScopedDirectory` 等） | 各测试文件 | 临时目录 / 临时输入 |

## 补充：shaderlib 与 program cache

| 文件 | 内容 |
|---|---|
| `core/math.hlsli` | `RADRAY_PI`、倒数和幂函数等标量数学 |
| `core/frame.hlsli` | 局部着色帧、ONB 和切线对齐 |
| `core/color.hlsli` | linear/sRGB 转换与色调映射 |
| `bsdf/fresnel.hlsli` | 介电 Fresnel 与折射几何辅助量 |
| `bsdf/microfacet.hlsli` | GGX 的 D/G 项和 roughness 转换 |
| `bsdf/principled.hlsli` | Principled BRDF 反射半球求值 |
| `lighting/lights.hlsli` | shader 光源布局与辐照度；不等同于 runtime CPU LightSceneData 的内存布局 |
| `shadow/filtering.hlsli` | 阴影 UV、bias、PCF 原语 |
| `shadow/cascade.hlsli` | 方向光级联阴影数据与求值 |
| `shadow/cube.hlsli` | 点光源 cube shadow 数据与面序 |
| `core/platform.hlsli` | `VK_LOCATION`、`VK_BINDING`、`VK_PUSH_CONSTANT`、`VK_IMAGE_FORMAT` target gate |

共享结构的字段顺序、对齐和矩阵约定是 shader ABI；compiler 为每个 active CBuffer declaration
发布指向当前 target-lane payload root 的 owner。调用方根据这份布局准备实际 GPU 字节；runtime 不再提供通用按名参数存储/setter，
也不能把 CPU 光源表直接 memcpy 成 shader 布局。`shadow/filtering.hlsli` 中的序列化枚举值和 `lights.hlsli` 中的上限
属于 ABI，不能因为重命名或排版而改变。

调用方仍须保证 GPU 已完成使用，PSO/参数集不再借用包装，再释放布局；本缓存不实现延迟销毁。
Device 关闭前所有外部引用必须释放，Debug 检查缓存为空。Vulkan 关闭顺序是 descriptor allocator、
pipeline layout cache、set-layout cache、sampler cache，最后销毁 device。
ShaderProgramCache 仍持有旧 program，因此 source invalidation 本身不会触发 native 布局释放。
