> - 适用: Application、World/Component、shader cache 与服务装配
> - 权威: 本文描述当前保留的 runtime 宿主；GPU 帧与上传见 frame-and-gpu，旧渲染框架见临时快照
> - 锚点: `modules/runtime/include/radray/runtime/application.h`, `modules/runtime/src/application.cpp`, `modules/runtime/include/radray/runtime/render_system.h`, `modules/runtime/src/render_system.cpp`, `modules/runtime/include/radray/runtime/game_framework/world.h`, `modules/runtime/include/radray/runtime/components/`, `modules/runtime/include/radray/runtime/shader_program.h`, `modules/runtime/include/radray/runtime/service_registry.h`

# Runtime 宿主、组件与 shader

旧 `render_framework`、Forward 与 ImGui 渲染适配已移除，删除前设计见
[临时快照](../temp/render-framework-design.md)。当前没有内置渲染管线、RenderGraph、Scene/proxy、
renderer list、output registry 或时域历史系统。

## Application 与 runner

`Run(desc)` 创建 WindowManager、GpuSystem、RenderSystem、AssetManager、World 和可选 AssetDatabase，
通过 ServiceRegistry 注入后建主窗口与 swapchain，调用 OnInit，再进入 StartLoop。
Application 继续负责资产 Pump、ApplicationScheduler、World tick 以及固定关停顺序。

SingleThreadRunner 顺序执行 update/record/submit；ThreadedRunner 保留 game/render 两线程、
可写/ready slot semaphore 与 fence 退休协议。窗口模态循环使用既有 Win32 vblank 机制。
GPU/flight、上传与生命周期等待没有交给新的渲染框架，详见[帧与 GPU](frame-and-gpu.md)。

`Application::Render(AppFrameContext&)` 是录制入口，默认不录制、不 acquire 窗口，也不清屏。
应用可覆盖它，使用当前 flight 的上下文录制 RHI 命令；Begin/End/Submit/Present 由 runner/GpuSystem 驱动。
需要呈现时调用 `AcquireWindow`，按 backbuffer 的真实状态录制 barrier，并在提交后发布状态。
写 flip backbuffer 的工作使用 `GetCommandBufferForTexture`；共享 command buffer 不写 flip backbuffer。
调用方负责 GPU owner、asset refs 与 per-flight 数据寿命，默认宿主不再自动保留渲染快照。

ApplicationExtension 及其安装入口、回调槽和销毁接线已移除。

## RenderSystem 基础服务

RenderSystem 仅持有 ShaderProgramCache 与 RHI RenderPassRegistry，借用 Application 配置和 GpuSystem 的 device。
`GetOrCreateShaderProgram` 保留源码请求与预编译 artifact 两个入口，source invalidation 和失败缓存行为不变。
GPU idle 后清理 shader/program，再清理 render pass/framebuffer cache。窗口仍借用该 registry，
销毁 backbuffer view 前清理关联 framebuffer；RenderSystem 销毁前先断开窗口的借用指针。

窗口变更所需的 `SetRenderIdle` / `SetRenderIdleWaiter` / `EnsureRenderIdle` 由 WindowManager 承接，
保留原来的 application-thread 限制与 runner 排空协议，不依赖已删除的 output registry。

## World 与组件

World 管理 Actor 所有权、spawn/destroy、注册/注销与 tick，不再创建或拥有 Scene。
Actor/SceneComponent 保留 RTTI 查询、父子关系、世界变换和变换传播。CameraComponent 保留 LH 视图/投影计算。
PrimitiveComponent 作为数据组件基类；StaticMeshComponent 保留 mesh asset 引用。
Light/Directional/Point/Spot 组件保留类型、颜色、强度、半径、阴影与角度设置及原有数值验证。
组件不再创建、更新或销毁 SceneProxy，也不提供旧 render-state 接口。

## Shader program 与参数

ShaderProgram 保留 artifact、native pipeline layout、stage shaders、真实 entry name、参数布局与 group recipe。
`GetStage(shader::ShaderStage)` 返回借用的 RHI ShaderEntry；不存在的 stage 返回空，借用内容随 program 销毁而失效。
调用方通过 RHI device 创建和持有 graphics/compute PSO；ShaderProgram 不再提供 PSO 创建入口或缓存。
ShaderProgramCache 的 artifact/program 复用与源码失效机制保留，详见 [Shader pipeline](shader-pipeline.md)。

ShaderParameterLayout/Storage 继续提供 canonical 名称解析、类型树和按名数值写入。
Material、MaterialTechnique、material_state、render_queue 与 cbuffer_view 已移除；StaticMeshComponent
不再保存已删除的 Material 指针，参数存储也不保留其专用整块复制入口。

GpuMesh 只保存 GPU buffers、vertex/index views 与 topology，不缓存顶点输入布局。
ResourceUploader 仍在分配和录制前校验单顶点流、stride、attribute 范围和语义唯一性，顶点流固定绑定到 slot 0。
primitive_vertex_layout 及其 resolver 已移除；PSO 调用方提供 RHI VertexInputState，由 RHI 对照 artifact 校验。

## ServiceRegistry

`ServiceRegistry<Entries...>` 是非拥有的静态装配器。系统头文件只包含轻量的
`service_traits.h` 并声明自己的契约；Application 只列出服务集合。`service_registry.h` 中的
`detail` 命名空间在编译期匹配提供者、检查签名并计算稳定拓扑序，装配器按类型序列展开直接调用。
实例里只有固定 tuple 的对象指针、执行进度与状态，没有 RTTI 索引、函数指针表或运行时图。

```cpp
template <> struct ServiceTraits<AssetManager> {
    using Dependencies = TypeList<Required<IWaitFrameProcessor>, Optional<IAssetSource>>;
    static void Inject(AssetManager& self, IWaitFrameProcessor& frames,
                       Nullable<IAssetSource*> source) noexcept;
    static void Unwire(AssetManager& self) noexcept;
};
```

`ServiceTraits<T>::Provides = TypeList<Interfaces...>` 显式暴露接口；具体类型 T 自动可查。
接口必须能从 T 公开、无歧义地转换，转换在已知类型下使用 `static_cast`，保留多继承指针调整。
同一实例的多个接口只共享一份生命周期。重复具体类型、重复导出、多个提供者、缺失必需依赖、
非法钩子签名与生命周期环都会阻止 registry 实例化。`kValidServiceRegistry<...>` 可用于
静态检查组合，`kInitializationOrder` 是编译期槽位索引数组；无依赖节点按集合声明顺序打破平局。

依赖列表的顺序也是 `Inject(T&, args...)` 的参数顺序：

| 声明 | 注入参数 | 存在性 | 生命周期顺序 |
|---|---|---|---|
| `Required<T>` | `T&` | 必须是非空槽位 | 提供者先初始化、后 Shutdown |
| `Optional<T>` | `Nullable<T*>` | 可缺类型或实例 | 存在该类型时建立顺序 |
| `Link<T>` | `T&` | 必须是非空槽位 | 只接引用，不建立启动边 |
| `OptionalLink<T>` | `Nullable<T*>` | 可缺类型或实例 | 只接引用，不建立启动边 |

所有对象先存在，再调用注入函数，因此引用环合法。需要对方已初始化的能力必须声明为
`Required`/`Optional`，不能用 `Link` 隐藏真实的生命周期环。`const T` 依赖注入 const 视图。

普通槽位的构造参数是 `T&`；`OptionalService<T>` 接受 `Nullable<T*>`，绑定时复制存在状态，
之后不可替换。必需依赖不能由可选槽位满足，即使调用方本次传入非空对象也一样。
图按所有可能存在的槽位静态校验；空槽位只跳过该对象的钩子，不重新排序。
已存在的可选服务初始化失败仍然失败，不会悄悄变成缺席。

`Get<T>()` 返回 `T&`，要求编译期存在非可选提供者。`Resolve<T>()` 返回 `Nullable<T*>`，
未导出的类型或空可选槽位返回空；查询直接访问固定槽位。registry 的 const 不改变借用对象的
可变性，需要只读视图时显式查询 `const T`。查询表示对象身份，不表示已经初始化。

静态钩子契约：

- `Inject(T&, args...) noexcept` 只连接引用；有依赖时必须提供。没有依赖也可以提供该钩子。
  参数必须精确匹配依赖列表，不能通过按值复制服务或其他隐式转换接受依赖。
- `Initialize(T&) -> ServiceStatus` 显式初始化；没有钩子的服务按已就绪处理。
- 声明 Initialize 时必须提供 `Shutdown(T&) noexcept`，并支持部分初始化；只提供 Shutdown 也合法。
- `Unwire(T&) noexcept` 可选，在所有 Shutdown 之后解除引用。Shutdown 期间对象与引用仍有效，
  操作已启动能力必须遵守依赖顺序。绑定对象必须活过整个调用。
- `Name` 可选，必须引用静态存储期字符串，用来标识运行时失败；不存在时 `ServiceStatus::Service`
  为空，`Message` 仍保留服务报告的原因。服务自身同名成员不会自动变成钩子。

`Initialize()` 仅允许从 Ready 调用一次：先全部 Inject，再按编译期拓扑顺序启动。进入每个
Initialize 前记录进度，失败时先 Shutdown 当前部分初始化的服务，再逆序处理此前服务，
最后按注入的逆序 Unwire 全部已接线对象。错误包含 `Code/Message/Service`；失败后状态为 Failed。
作用域守卫也在栈展开时执行相同清理，但不捕获、转换异常。注入/清理钩子不得抛异常。

正常 `Shutdown()` 使用相同的反向展开，幂等；生命周期调用中的重入返回 false，重复 Initialize
返回 InvalidState。Stopped/Failed 不允许重新启动。生命周期操作由调用方串行化；只读查询不修改
registry，但对象的线程安全仍由对象自己保证。

registry 析构不调用钩子、不释放借用对象。owner 显式选择 Shutdown 时机，负责先使 GPU、任务与
引用持有者静默。Application 当前只用局部 registry 完成启动事务，正常运行后的对象析构仍由
上文固定 teardown 执行；`RenderSystem::OnShutdown` 与析构共享幂等资源清理。

当前系统契约：

| 服务 | Provides | Dependencies | 生命周期 |
|---|---|---|---|
| `WindowManager` | — | `Link<GpuSystem>`, `Link<RenderSystem>` | Unwire |
| `GpuSystem` | `IWaitFrameProcessor` | `Required<WindowManager>` | Unwire；设备已由构造创建 |
| `AssetManager` | — | `Required<IWaitFrameProcessor>`, `Optional<IAssetSource>` | Unwire |
| `AssetDatabase` | `IAssetSource` | — | 打开成功后作为可选实例绑定 |
| `RenderSystem` | — | `Required<GpuSystem>` | Initialize / Shutdown / Unwire |
| `World` | — | — | 当前由 Application 构造与析构 |
