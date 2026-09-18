> - 适用: Application、World/Component、shader cache 与服务装配
> - 权威: 本文描述当前保留的 runtime 宿主；GPU 帧与上传见 frame-and-gpu，旧渲染框架见临时快照
> - 锚点: `modules/runtime/include/radray/runtime/application.h`, `modules/runtime/src/application.cpp`, `modules/runtime/include/radray/runtime/render_system.h`, `modules/runtime/src/render_system.cpp`, `modules/runtime/include/radray/runtime/game_framework/world.h`, `modules/runtime/include/radray/runtime/components/`, `modules/runtime/include/radray/runtime/shader_program.h`

# Runtime 宿主、组件与 shader

旧 `render_framework`、Forward 与 ImGui 渲染适配已移除，删除前设计见
[临时快照](../temp/render-framework-design.md)。当前没有内置渲染管线、RenderGraph、
renderer list、output registry 或时域历史系统。
World → Scene 已建立单份持久 CPU Scene、per-flight 增量包交付、组件标脏与 Primitive 生命周期，
以及 StaticMesh 的 CPU 持久描述、变换、bounds、不可变 GPU 几何借用与 flight 资产保活；绘制尚未接入。

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
通过 `AllocateCommandBuffer` 获取命令，使用 `ReturnCommandBuffers` 按序归还命令组及同步信息。
需要呈现时调用 `AcquireWindow`，按 backbuffer 的真实状态录制 barrier，将目标随命令组移动归还。
命令池寿命、呈现批次及提交契约见[命令分配与有序批次](frame-and-gpu.md#命令分配与有序批次)。
StaticMesh 的资产寿命由下述 flight 保活路径管理；应用自行录制的其他 GPU owner 和 per-flight 数据仍由调用方负责。

ApplicationExtension 及其安装入口、回调槽和销毁接线已移除。

## RenderSystem 基础服务

RenderSystem 持有一份 Scene、与 GPU flight 等量的 SceneUpdateBatch 和 GT 资产引用数组，以及 ShaderProgramCache 与
RHI RenderPassRegistry，借用 Application 配置、AssetManager 和 GpuSystem 的 device。
`GetOrCreateShaderProgram` 保留源码请求与预编译 artifact 两个入口，source invalidation 和失败缓存行为不变。
GPU idle 后清理 shader/program，再清理 render pass/framebuffer cache。窗口仍借用该 registry，
销毁 backbuffer view 前清理关联 framebuffer；RenderSystem 销毁前先断开窗口的借用指针。

### Scene 交付

runner 取得可写 flight、处理旧 completion 后，GT 可以填充当前 batch；`Application::Update`
在 `World::Tick` 之后调用 `PrepareFrameGT`，由其执行 `World::FlushRenderUpdates` 并取得本 flight 的唯一资产引用。runner 确认本帧继续执行后
按原有协议发布 ready slot，RenderSystem 不另设发布接口或序号。

两种 runner 都在 BeginFrameRecord 后、应用 Render 前调用固定的 `Application::ConsumeRenderUpdates`。
`Application::BeginUpdateForFlight` 与 `ConsumeRenderUpdates` 均为 private，仅由作为 friend 的
SingleThreadRunner、ThreadedRunner 驱动，不向应用派生类开放。
ThreadedRunner 的退出与模态 discard 判断只控制绘制，不能绕过 Scene::Apply。Apply 不访问 World、
窗口或 GPU，只按发布顺序更新单份 Scene；下一次 Apply 必须等上一帧全部 CPU Scene 读取结束。
当前 runner 顺序执行这些阶段，尚无额外 Scene 读取任务。

RenderSystem 不维护独立的 flight 状态机；可写、发布和退休时机由 runner/GpuSystem 的现有协议保证。
每个 flight 保存 batch 与 GT 资产引用，不额外复制 FrameSerial。Application 消费真实 GPU completion 后，
RenderSystem 按 FlightIndex 清空对应 batch 和引用并保留数组容量；GpuWorkCompleted=false 不撤销已应用的 CPU 更新。
关停先排空已发布帧和真实 completion，再清理未发布 batch 及其引用，
不等待或伪造它们的 completion。收集会推进 GT 的发送状态，因此 abandon 只用于终止本次 World/Scene，
不能丢包后继续交付同一场景。具体帧序见[帧与 GPU](frame-and-gpu.md)。

Scene 的头文件和实现分别位于 `include/radray/runtime/render_framework/scene.h` 与
`src/render_framework/scene.cpp`（均相对 `modules/runtime/`）；`SceneUpdateBatch` 与 Scene 同在 `scene.h` 中声明。
Scene 访问器仅供 RT 或 RT 停止后的检查，不能在 GT 与 Apply 并发读取。
当前 batch 包含 `RemovePrimitives`、`CreatePrimitives`、`MeshStates` 与 `Transforms`；
没有组件指针、诊断探针、统计计数或额外序号。
Apply 先删除旧身份，再创建新身份，维护槽位的存活状态与代次。`ContainsPrimitive` 查询完整身份；
删除不存在的身份、重复创建或旧代次创建属于契约错误，直接终止。取消未发送的创建会留下代次间隔，允许后续较新代次创建。
身份处理后应用 mesh state，再应用 transform；类型化更新同样校验完整 PrimitiveId。
Light payload 和 GPU 参数上传尚未接入。
`test_scene_delivery` 覆盖 F=1/2/3 的空包与 Primitive 交付、flight 边界及真实 runner 退出排空；
跳过绘制仍检查 Scene 最终身份，测试侧使用既有 GPU 帧号与 completion。

运行期窗口变更统一经 WindowManager 的协程接口发起，由 runner 在受控维护阶段排空渲染并执行，
离开修改阶段后再交付结果；不再由窗口方法传播 idle 状态或反向调用 runner。
接口、句柄寿命和取消规则见[窗口修改协程](frame-and-gpu.md#窗口修改协程)。
主窗口启动和最终拆除由 Application 调用私有同步路径；OnInit/OnShutdown 仍为同步钩子。

## World 与组件

World 管理 Actor 所有权、spawn/destroy、注册/注销与 tick，不再创建或拥有 Scene。
Actor/SceneComponent 保留 RTTI 查询、父子关系、世界变换和变换传播。CameraComponent 保留 LH 视图/投影计算。
PrimitiveComponent 管理 GT 的渲染身份；StaticMeshComponent 保留 mesh asset 引用，改绑时标记 StateDirty。
Light/Directional/Point/Spot 组件保留类型、颜色、强度、半径、阴影与角度设置及原有数值验证。
组件不直接访问 RT Scene，也不恢复旧 SceneProxy 接口。

### 组件标脏、合并与生命周期（M1）

所有 World/组件修改与收集只在 GT 执行。ActorComponent 只负责 owner、注册状态与通用生命周期。
SceneComponent 提供 State、Transform、DynamicData 三种 dirty flag，
以及 `MarkRenderStateDirty`、`MarkRenderTransformDirty`、`MarkRenderDynamicDataDirty`。
未注册时标脏无效；Primitive 注册会标记 State，收集读取届时的最终值。
World 用 `vector<SceneComponent*>` 保存待更新组件；第一次标脏入队并记录索引，后续只 OR flags。
收集成功后清 dirty 并出队；删除采用 O(1) swap-remove，并修正被移动组件的索引。
没有变化时不扫描 Actor 或全部组件。队列指针仅在 GT 使用，不进入 batch。

`FlushRenderUpdates` 要求空 batch；期间不得修改 World/组件或递归收集。内置的结构、变换、mesh 修改与标脏入口
检查此约束；派生类自己的 setter 也应先调用 `CheckCanModify`。收集失败后的半成品 batch 不得发布或重试，
本阶段不提供事务回滚。State 与其他 flags 一起传给类型专属收集函数，后者决定哪些字段被全状态覆盖，
公共调度不机械丢弃 Transform/DynamicData。

渲染 dirty 状态、队列索引与 Create/Destroy/CollectRenderUpdates 接口均位于 SceneComponent；
默认注册和变换通知不入队，由参与渲染的派生类型显式标脏。Light 可直接复用 SceneComponent 调度，
不需要继承 PrimitiveComponent 或取得 PrimitiveId。
Actor 中央注册先设置 registered，仅对 SceneComponent 调用 `CreateRenderState`，最后调用用户 `OnRegister`。
中央注销先清 registered，仅对 SceneComponent 断开待更新队列并调用 `DestroyRenderState`，最后调用用户 `OnUnregister`；
因此安全性不依赖派生 OnUnregister 调用基类，注销回调中的标脏也不会重新排队自身。
Primitive 的三个渲染生命周期/收集实现为 final；派生类型通过 `CollectPrimitiveUpdates` 捕获自己的数据。
Primitive 的 `OnTransformChanged` 标记 TransformDirty，覆写时须调用基类；原有递归子树通知和父链矩阵计算保持不变。

World 分配类型化 `{Index, Generation}` 的 PrimitiveId，组件直接保存身份，一 World 对一 Scene。
注册时取得 ID；结构变化保持 ID；注销立即释放槽位并提升代次，耗尽时终止而不回绕。
未收集创建即注销只取消排队；创建已经写入批次后注销则追加仅含 ID 的删除项，不再读取源组件。
所以即使 RT 尚未消费创建、GT 已销毁或复用组件槽位，也能按批次顺序先创建旧代次、删除，再创建新代次。
Primitive 身份与具体类型数据分开；StaticMesh 的 CPU 路径见下节，Light 的独立身份与记录尚未接入。

`test_scene_updates` 覆盖普通组件生命周期、普通 SceneComponent 不自动入队、非 Primitive 的 Scene 派生类渲染同步、重复标脏合并、最终变换收集、注册前修改、队列中任意位置删除、跨帧创建/删除、
代次复用与旧 ID 拒绝、父子变换/重挂接/父节点删除，以及收集期修改拒绝。操作次数只在测试 fixture 中观察。

### StaticMesh 的 CPU 持久数据（M2）

`StaticMeshComponent::CollectPrimitiveUpdates` 在 StateDirty 时读取当前 mesh 引用，生成一个
`StaticMeshStateUpdate`：PrimitiveId、最终 LocalToWorld，以及拥有数据的 `StaticMeshDescription`。
描述拥有 AssetId、section 范围与 local bounds；`RenderMesh` 借用 Ready 资产内的只读 `GpuMesh`，寿命见下节。
CPU 数据有效但未显式分 section 的资产按每个 primitive 生成完整范围；空引用、Loading、失败或无效 CPU mesh
生成空 Sections，明确表示没有可用几何。AssetId 可保留请求身份，但不代表 Ready 或 GPU 可绘制。

同轮 StateDirty 与 TransformDirty 合成一个带最终变换的 state；只有 TransformDirty 时，
只写一个固定大小的 `PrimitiveTransformUpdate`，不访问 mesh 资产、不验证或复制 sections。
普通变换包预热后复用数组容量。当前 section 元数据在结构变化时复制到 batch，再复制到对应 proxy；
每个实例拥有这份 CPU 描述，静止和移动帧不重复提取，未引入共享几何缓存或另一套资产引用计数。

Scene 的槽位唯一拥有 `src/render_framework/static_mesh_proxy.h/.cpp` 中的 StaticMeshProxy。
Create 首次建立 proxy；Replace 原地替换该对象描述并以新 local bounds 和最终矩阵计算 world bounds，
不改变 PrimitiveId；Transform 只更新矩阵、world bounds 与 ReverseCulling。
world AABB 按仿射矩阵各轴区间计算，支持旋转、非均匀 scale、父子组合产生的 shear 与负 scale；
线性部分行列式为负时 ReverseCulling 为 true，零 scale 不翻转 winding。非法 local bounds、非有限或非仿射矩阵
属于契约错误。此阶段尚无法线/GPU 参数或绘制状态绑定。

删除销毁该实例 CPU proxy，并用 swap-remove 修正稠密 mesh 身份列表；不访问或销毁共享资产。
`GetStaticMeshes` 返回已登记的 mesh 身份，包括几何尚不可用的对象；`GetStaticMesh(id)` 返回只读借用视图，
失效/旧代次 ID 返回空。视图与列表只允许 RT 读取，借用不跨越下一次 Apply 或 Scene 销毁。
多个视图读取同一份 CPU 数据，不触发 Apply、收集或结构重建。

`test_static_mesh_scene` 使用无 GPU 资源的 StaticMesh fixture，覆盖 Create/Transform/Replace/Remove、
单对象局部更新、结构与变换合并、bounds/负 scale、无 section 的默认范围、空/Loading/无效资源状态、
源销毁后的跨线程消费、活跃索引修复、旧代次拒绝，以及 F=1/2/3 的持久状态与批次数组复用。

### 真实资产与 flight 保活（M3）

StaticMeshComponent 保存最后已发送的 Ready 资产 ID；World 用非 owning 的 `AssetId → 使用数/资产身份`
索引记录 Scene 将使用的唯一 mesh。State 收集时增量更新旧/新绑定，注销时移除已发送绑定。
使用数只是索引维护依据，不拥有资产；组件继续持有自己的 StreamingAssetRef。Loading、失败、取消和
无效 CPU mesh 不进入保活索引，描述中的 RenderMesh 为空。

每次 `PrepareFrameGT` 在 Flush 后遍历唯一资产索引，调用 `AssetManager::Find<StaticMesh>` 取得本 flight
的引用。即使 batch 为空，静止对象也得到保护；成本为 O(U)，U 是唯一 Ready mesh 数，不扫描全部组件。
Find 不启动加载；缺失资产、类型不符或 Ready 对象与已发送视图不一致属于契约错误，立即终止。
同一 AssetId 的 Ready 对象不可在保留绑定时静默热替换。

引用只在 GT 创建、移动与释放；RT 的 batch、Scene 和 proxy 只复制借用指针，不操作 StreamingAssetRef。
RT 必须先 Apply 当前 flight 的全部增量，之后才可在该 flight 保活期间读取 RenderMesh。
旧资产可能在下一次 Apply 前已经退休，因此 Remove/Replace/Scene 析构只丢弃旧指针，不解引用它。
CPU 描述可持久保存，RT 停止后的 Scene 检查也不意味着资产借用仍然有效。
已发布 flight 的引用仅在真实 fence completion 被 GT 消费后释放，包括跳过绘制的帧；
未发布 flight 在关停 abandon 时释放，不伪造 completion。

已注册组件通过现有 `co_await mesh` 等待 Loading 请求，每个挂起请求有自己的 TaskScope。
Ready 后校验当前注册身份和 mesh 请求仍相同，再标 StateDirty；下一次 Flush 读取最新变换。
改绑、注销和销毁会取消并收束本组件等待者，不调用 mesh.Cancel，不取消其他组件共享的加载。
加载失败或被调用方取消时保持空几何；没有每帧 Ready 轮询。

`test_scene_assets` 用带 GT 析构检查的资产覆盖静止帧、多个 flight、跨线程改绑/删除、千实例去重、
退休资产后的 Remove/Replace、Ready 最新变换、旧请求与身份复用、共享等待取消、关停与空 ID 绑定。
真实上传和绘制仍由后续阶段验证。

## Shader program 与参数

ShaderProgram 保留 artifact、native pipeline layout、stage shaders、真实 entry name。
`GetStage(shader::ShaderStage)` 返回借用的 RHI ShaderEntry；不存在的 stage 返回空，借用内容随 program 销毁而失效。
调用方通过 RHI device 创建和持有 graphics/compute PSO；ShaderProgram 不再提供 PSO 创建入口或缓存。
ShaderProgramCache 的 artifact/program 复用与源码失效机制保留，详见 [Shader pipeline](shader-pipeline.md)。

常量数据由调用方按实际 GPU 布局准备、上传和绑定。
Material、MaterialTechnique、material_state、render_queue 与 cbuffer_view 已移除；StaticMeshComponent
不再保存已删除的 Material 指针。

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
| `RenderSystem` | `Application`、`GpuSystem`、`AssetManager` | 构造参数、`SetGpuSystem`、`SetAssetManager` |
| `AssetManager` | `IWaitFrameProcessor`、可选 `IAssetSource` | `SetWaitFrameProcessor`、`SetAssetSource` |
| `World` | `Application` | 构造参数 |

Application 先构造 WindowManager 和 GpuSystem（包含 device），再创建 RenderSystem、AssetManager、
可选 AssetDatabase 与 World。默认 importer 只保留登记与明确失败的加载入口，不借用上传调度器。全部对象就位后直接接线：GpuSystem 提供帧等待接口，AssetDatabase
提供可选资产来源；未配置资产根或数据库打开失败时，资产来源为空。
WindowManager 与 GpuSystem 的双向引用在启动渲染线程前建立。

接线完成后直接调用 `RenderSystem::OnInitialize(error)`，创建 shader/program 与 render-pass caches。
初始化失败时记录错误并调用 `DestroyRuntime`，返回启动失败；窗口或 swapchain 创建失败使用同一清理路径。
RenderSystem 的析构接受部分初始化状态，通过 `OnShutdown` 幂等释放缓存。

正常关停先关闭窗口协程入口并取消等待者，再停止 runner、等待 GPU idle、消费完成消息并取消应用调度任务，由 `DestroyRuntime`
按固定顺序释放对象。WindowManager 借用的 RenderSystem 引用在后者销毁前清空；AssetManager
销毁并收束加载协程后才销毁 AssetDatabase；交换链释放后才断开窗口与 GPU 的双向引用并销毁 device。
借用引用的提供者必须活过消费者的清理，完整时序见[帧与 GPU](frame-and-gpu.md#关停顺序)。
