> - 适用: Application、World/Component、shader cache 与服务装配
> - 权威: 本文描述当前保留的 runtime 宿主；GPU 帧与上传见 frame-and-gpu，旧渲染框架见临时快照
> - 锚点: `modules/runtime/include/radray/runtime/application.h`, `modules/runtime/src/application.cpp`, `modules/runtime/include/radray/runtime/render_system.h`, `modules/runtime/src/render_system.cpp`, `modules/runtime/include/radray/runtime/game_framework/world.h`, `modules/runtime/include/radray/runtime/components/`, `modules/runtime/include/radray/runtime/shader_program.h`

# Runtime 宿主、组件与 shader

旧 `render_framework`、Forward 与 ImGui 渲染适配已移除，删除前设计见
[临时快照](../temp/render-framework-design.md)。当前没有内置渲染管线、RenderGraph、Scene/proxy、
renderer list、output registry 或时域历史系统。

## Application 与 runner

`Run(desc)` 创建 WindowManager、GpuSystem、RenderSystem、AssetManager、World 和可选 AssetDatabase，
由 Application 直接连接系统依赖并初始化 RenderSystem，再建主窗口与 swapchain，调用 OnInit，进入 StartLoop。
Application 继续负责资产 Pump、ApplicationScheduler、World tick 以及固定关停顺序。
两种 runner 都先取得可写 flight、处理完成批次，再记录帧开始时间、派发窗口事件并执行 Update；
时间口径和模态循环中的准备帧复用见[帧与 GPU](frame-and-gpu.md#帧序)。

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

## Application 直接装配

底层系统的依赖关系集中在 `Application::InitializeRuntime`，由显式构造和 setter 调用建立。
系统只保存所需的借用引用；新增依赖时直接修改这里的接线和 `DestroyRuntime` 的拆除顺序。

| 消费者 | 借用的对象或接口 | 接线方式 |
|---|---|---|
| `WindowManager` | `GpuSystem`、`RenderSystem` | `SetGpuSystem`、`SetRenderSystem` |
| `GpuSystem` | `WindowManager` | `SetWindowManager` |
| `RenderSystem` | `Application`、`GpuSystem` | 构造参数、`SetGpuSystem` |
| `AssetManager` | `IWaitFrameProcessor`、可选 `IAssetSource` | `SetWaitFrameProcessor`、`SetAssetSource` |
| `World` | `Application` | 构造参数 |

Application 先构造 WindowManager 和 GpuSystem（包含 device），再创建 RenderSystem、AssetManager、
可选 AssetDatabase 与 World。默认 importer 只保留登记与明确失败的加载入口，不借用上传调度器。全部对象就位后直接接线：GpuSystem 提供帧等待接口，AssetDatabase
提供可选资产来源；未配置资产根或数据库打开失败时，资产来源为空。
WindowManager 与 GpuSystem 的双向引用在启动渲染线程前建立。

接线完成后直接调用 `RenderSystem::OnInitialize(error)`，创建 shader/program 与 render-pass caches。
初始化失败时记录错误并调用 `DestroyRuntime`，返回启动失败；窗口或 swapchain 创建失败使用同一清理路径。
RenderSystem 的析构接受部分初始化状态，通过 `OnShutdown` 幂等释放缓存。

正常关停先停止 runner、等待 GPU idle、消费完成消息并取消应用调度任务，再由 `DestroyRuntime`
按固定顺序释放对象。WindowManager 借用的 RenderSystem 引用在后者销毁前清空；AssetManager
销毁并收束加载协程后才销毁 AssetDatabase；交换链释放后才断开窗口与 GPU 的双向引用并销毁 device。
借用引用的提供者必须活过消费者的清理，完整时序见[帧与 GPU](frame-and-gpu.md#关停顺序)。
