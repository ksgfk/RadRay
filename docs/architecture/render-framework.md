> - 适用: Application、多 World、组件渲染连接、场景交付与共享渲染服务
> - 权威: 本文描述 runtime 宿主与场景边界；GPU 帧与上传见 frame-and-gpu
> - 锚点: `modules/runtime/include/radray/runtime/application.h`, `modules/runtime/src/application.cpp`, `modules/runtime/include/radray/runtime/world_manager.h`, `modules/runtime/src/world_manager.cpp`, `modules/runtime/include/radray/runtime/render_system.h`, `modules/runtime/src/render_system.cpp`, `modules/runtime/include/radray/runtime/game_framework/`, `modules/runtime/include/radray/runtime/components/`, `modules/runtime/include/radray/runtime/render_scene/`

# Runtime 宿主、World 与渲染场景

旧 Forward 与 ImGui 渲染适配已移除，删除前设计见[临时快照](../temp/render-framework-design.md)。
当前支持多个 World、独立 RenderScene、per-flight 增量交付和 StaticMesh 的 CPU 持久描述、变换、bounds、
不可变 GPU 几何借用及资产保活。没有内置绘制管线、RenderGraph、renderer list、output registry 或时域历史系统。

## Application 与 runner

`Run(desc)` 创建 WindowManager、GpuSystem、RenderSystem、WorldManager、AssetManager 和可选 AssetDatabase，
直接连接依赖、初始化服务并创建主窗口和 swapchain，再调用 OnInit。启动不创建默认 World。
应用通过 `GetWorldManager()` 访问 WorldManager；该入口从 OnInit 到 OnShutdown 有效，运行时初始化前和拆除后返回空。
WorldManager 的 `CreateWorld` 返回带代次的 WorldId；`GetWorld(id)` 对无效、旧代次或待销毁身份返回空，
`DestroyWorld` 立即禁止后续调度，实际释放在本轮全部 World Tick 后、渲染收集前执行。
WorldId 与 SceneId 是不同的类型；同一槽位的代次约定不在程序生命周期内回绕。

Application 拥有 WorldManager，只负责服务装配与帧序；WorldManager 拥有 World 集合，
提供 `AttachWorldToRendering` / `DetachWorldFromRendering` 装配连接。
WorldManager 可独立构造，不传宿主和渲染服务时支持纯 CPU 使用；借用的 Application、RenderSystem 必须活过 manager。
也可直接构造 World，并调用 `AttachToRendering` / `DetachFromRendering`；RenderSystem 必须活过连接它的 World。
World 借用不跨越其销毁安全点，WorldId 只在所属 WorldManager 内有效。
World Tick 使用身份快照；Tick 中新建的 World 下一帧开始 Tick，但可在本帧收集初始渲染状态。
Tick 中销毁当前 World 不会释放正在执行的回调对象；标记待销毁的其他 World 不再被本轮调度。
WorldManager::Tick 在全部 Tick 回调返回后清理待销毁 World；CollectRenderUpdates 单独执行，包含暂停的 World。
收集和 World 析构期间不能通过 WorldManager 改变集合或连接关系；Tick、收集和 Clear 不能重入。
Clear 在 manager 空闲时立即隐藏并销毁全部 World、使旧身份失效；此后可创建新 World。
Clear 和 manager 析构仅请求场景退出，资产退休与真实完成通知仍由 RenderSystem 管理，不等待全局 GPU idle。

`SetTickEnabled(false)` 只暂停该 World 的模拟；编辑修改、资产 Ready 通知和场景同步继续执行。
Actor/Component 的所有权、RTTI、空间层级与变换传播仍由 World/Actor 管理。
CameraComponent 提供 LH 视图和投影计算，不自动选择活动相机；未来绘制调用方显式提供
SceneId、相机值快照和输出目标，RT 不读取 CameraComponent。本阶段未增加视图执行 API。

SingleThreadRunner 顺序 update/record/submit；ThreadedRunner 保留 GT/RT 两线程和现有
writable/ready semaphore、fence 退休协议。两者取得可写 flight、处理 completion 后才开始本帧更新。
`Application::Render` 默认不录制、不 acquire 窗口、不清屏；应用的 OnRender 可通过 AppFrameContext
分配和归还命令批次。Begin/End/Submit/Present 仍由 runner/GpuSystem 驱动，详见[帧与 GPU](frame-and-gpu.md)。

## 场景职责与目录

| 对象 | 所有权与职责 |
|---|---|
| Application | 拥有各系统及 WorldManager；装配依赖、驱动帧序与关停 |
| WorldManager | World 集合、身份、Tick 快照、延迟销毁及渲染连接装配 |
| World | Actor/Component 与可选 WorldRenderBridge；不持有 RT 场景、flight 或资产退休列表 |
| WorldRenderBridge | World 内部 GT 适配器；组件渲染生命周期、脏队列与最终值收集 |
| SceneWriter | 每个 Scene 的 GT 身份分配、更新合并、资产绑定及 RenderAssetLifetime |
| RenderScene | 每个 Scene 的单份 RT 持久 CPU 数据；Apply 和只读查询 |
| RenderSystem | GT 写入端登记表、RT 场景登记表、per-flight 交付包和共享 shader/render-pass 缓存 |

`world_manager.h/.cpp` 是顶层 World 管理服务；`game_framework/` 放 World 与同步桥；WorldRenderBridge 的头文件和实现在 `src/game_framework/`，不作为公共入口；`render_scene/` 放 SceneId、PrimitiveId、SceneUpdateBatch、
SceneWriter、RenderScene、StaticMeshProxy 和资产保活。渲染侧不包含 World/组件头文件、不保存组件指针。
PrimitiveId 位于 `scene_id.h`，在一个 SceneId 内有效；跨场景引用必须同时携带 SceneId。
`scene_update.h/.cpp` 定义拥有 CPU 数据的更新协议；`render_scene.h/.cpp` 只负责持久状态和查询。

RenderSystem 的 `CreateSceneGT` 创建一个独立数据生产者的场景；`GetSceneWriterGT` 对旧身份、
正在关闭或由 World 独占的场景返回空。WorldRenderBridge 声明独占写入端后，外部不能销毁该 Scene，
必须通过 World 或 WorldManager 的断开入口释放连接。独立场景通过 `DestroySceneGT` 请求销毁。
SceneWriter 借用只在 GT 使用，不跨越销毁请求；请求后保留的旧借用不能继续写入。

ShaderProgramCache 与 RHI RenderPassRegistry 在所有场景之间共享。窗口借用 registry，
销毁 backbuffer view 前清理关联 framebuffer；RenderSystem 销毁前先断开窗口引用。
GPU idle 后清理 program 和 render-pass/framebuffer cache，设备最后销毁。

## 组件连接、标脏与写入

组件游戏注册与渲染连接是两套生命周期。未连接的 World 正常注册和 Tick，PrimitiveId 无效，
标脏不排队。连接时遍历已有注册组件调用 CreateRenderState，首次收集发送完整状态；
断开时调用 DestroyRenderState、清队列和身份，但不调用游戏 OnRegister/OnUnregister。
重连创建新的 SceneId，从组件当前值重建；旧场景可以同时处于退休阶段。

Actor 中央注册先设置 registered；若已有连接，则建立渲染状态，再调用 OnRegister。
中央注销先清 registered，再销毁渲染状态，最后调用 OnUnregister；安全性不依赖派生回调调用基类。
普通 SceneComponent 不自动排队，渲染派生类型显式标记 State/Transform/DynamicData。
WorldRenderBridge 用 `vector<SceneComponent*>` 排队，首次标脏记录索引，后续 OR flags；
删除用 swap-remove 修正移动元素索引，没有变化时不扫描全部 Actor/Component。

Create/Destroy/CollectRenderUpdates 接口接收 SceneWriter。PrimitiveComponent 的中央实现为 final，
派生类型通过 OnRenderStateCreated/OnRenderStateDestroyed 和 CollectPrimitiveUpdates 扩展。
OnTransformChanged 标记 TransformDirty，派生覆写需要调用基类。
收集期间禁止修改所属 World/组件及递归收集；派生 setter 也应先调用 CheckCanModify。
State 与其他 dirty flags 一起传给派生收集函数，由其决定覆盖字段。本阶段不提供收集失败后的事务回滚。

SceneWriter 使用 SparseSet 管理 Primitive 身份、已发送状态与脏项。它的 CreatePrimitive、
RemovePrimitive、SetStaticMesh、SetTransform 同时供 World 桥与独立工具使用。
未发送创建后立即删除不产生生命周期项；已封口的创建后删除必须在后续包交付删除。
同轮重复写入合并为最终值，State 包含最终变换，单独 Transform 不重新提取 mesh 描述；SetTransform 要求此前设置过 StaticMesh 状态，允许空 mesh 绑定。
Primitive 删除释放槽位并增加代次；完整身份校验拒绝旧 ID，RT 不借用 GT 登记表。

StaticMeshComponent 保留当前 mesh ref；描述提取、最后绑定资产身份与计数统一位于 SceneWriter。
Ready 且 CPU mesh 有效时，描述拥有 AssetId、sections、local bounds，并借用只读 GpuMesh。
无显式 sections 时按 primitive 生成完整范围；空、Loading、失败或无效 CPU mesh 产生空几何。
独立调用方负责在资产 Ready 后再次提交；组件使用已有 co_await 路径自动标脏，不逐帧轮询。
组件等待随渲染连接建立和取消，Ready 回调同时检查 SceneId、PrimitiveId 和资产请求身份；
改绑或断开只取消本组件等待者，不取消共享资产加载。

RenderScene 的 Apply 按删除、创建、mesh state、transform 更新槽位；重复创建或旧代次更新是契约错误。
StaticMeshProxy 持有每实例 CPU 描述。Replace 更新 bounds 和变换但不改变身份；Transform 只更新
矩阵、world AABB 与 ReverseCulling，支持旋转、非均匀缩放、shear 和负 scale；非法 bounds、
非有限或非仿射矩阵属于契约错误。GetStaticMeshes 返回含空几何的登记身份，GetStaticMesh 对旧 ID 返回空。
列表与视图只能在 RT 或 RT 停止后读取，借用不跨越下一次 Apply 或场景销毁。

## 交付、退出与资产保活

GT 帧序为 completion → asset/scheduler Pump → OnUpdate → 各 World Tick → 待销毁 World 清理
→ 各 WorldRenderBridge 收集 → RenderSystem::SealFrameGT → runner 发布。
SealFrameGT 为每个未封口销毁的 Scene 生成带 SceneId 的条目，含 Create/Destroy 标记及 SceneUpdateBatch，
并封口该 flight 的资产退休列表。空场景内容也可有条目；同帧建删按 create → apply → destroy 消费。
同一 flight 封口后不能重复封口，必须等真实 completion 清理；更新数组保留容量供 flight 复用。

GT 与 RT 各有独立登记表；RT 仅从封口包创建、更新、删除 RenderScene。
两个 runner 在 BeginFrameRecord 后、应用 OnRender 前执行 ConsumeRenderUpdates；
退出或模态 discard 只跳过绘制，不跳过已发布场景操作。下一次 Apply 必须等此前全部 CPU 场景读取结束。
RenderSystem 不增加 GPU 提交或第二套发布协议；FrameSerial 和完成通知沿用 runner/GpuSystem。
GetSceneRT 对不存在或旧代次身份返回空，不能在 GT 与 RT Apply 并发查询。

每个 SceneWriter 拥有一个 GT 专用 RenderAssetLifetime，以 AssetId 去重持有当前绑定的 Ready 资产。
计数统计该 Scene 的绑定使用；不同 Scene 各持有独立 owner，底层资产由 AssetManager 共享。
引用创建、复制与释放都在 GT。RemoveUse 降为零时记录退休候选，同轮重新绑定可复用 owner；
SealRetirements 把最终零使用的 owner 放到承载改绑/删除的 flight，真实 completion 后 ReleaseFlight。
RT 只借用不可变视图，删除和替换 CPU 描述不解引用旧资源。

断开 World 后停止生产该连接的更新，已有包继续消费，后续有序包带场景销毁标记。
RT 应用销毁后删除 RenderScene；GT 写入端与剩余 owner 活到该销毁 flight 完成，随后释放并复用 SceneId 槽位。
普通退出不等待全局 GPU idle，其他世界继续工作；重新连接无需等待旧场景退休。
这一保证依赖当前全部资产 GPU 使用由同一有序主队列覆盖，增加异步队列前需要补齐跨队列完成依赖。

关停先排空已发布帧和真实 completion，再于 RT 停止、GPU idle 后 abandon 未发布包。
Abandon 仅用于终止交付，不能丢包后继续同一场景；此后不再读取 RT 的资产借用，
只能检查自持 CPU 元数据。最终拆除顺序为 WorldManager（全部 World）→ RenderSystem → AssetManager → AssetDatabase → GpuSystem。

测试由 SceneUpdates、StaticMeshScene、SceneAssets、WorldManager/WorldScenes 覆盖组件、
独立写入、身份隔离、重连、Ready 与退休；SceneDeliveryRunner/MultiWorldSceneRunner 验证
D3D12/Vulkan、单/双线程、F=1/2/3、跳过绘制和关停排空。真实 mesh 上传和绘制仍未接入。

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
| `RenderSystem` | `Application`、`GpuSystem` | 构造参数、`SetGpuSystem` |
| `AssetManager` | `IWaitFrameProcessor`、可选 `IAssetSource` | `SetWaitFrameProcessor`、`SetAssetSource` |
| `WorldManager` | 可空 `Application`、可空 `RenderSystem` | 构造参数 |
| `World` | 可空 `Application`；连接期间借用 `RenderSystem` 的 SceneWriter | 构造参数与 `WorldRenderBridge` |

Application 先构造 WindowManager 和 GpuSystem（包含 device），再创建 RenderSystem、WorldManager、AssetManager、
可选 AssetDatabase；World 由应用在 `OnInit` 或后续 GT 更新阶段通过 WorldManager 显式创建。默认 importer 只保留登记与明确失败的加载入口，不借用上传调度器。全部对象就位后直接接线：GpuSystem 提供帧等待接口，AssetDatabase
提供可选资产来源；未配置资产根或数据库打开失败时，资产来源为空。
WindowManager 与 GpuSystem 的双向引用在启动渲染线程前建立。

接线完成后直接调用 `RenderSystem::OnInitialize()`，创建 shader/program 与 render-pass caches。
初始化失败时记录错误并调用 `DestroyRuntime`，返回启动失败；窗口或 swapchain 创建失败使用同一清理路径。
RenderSystem 的析构接受部分初始化状态，通过 `OnShutdown` 幂等释放缓存。

正常关停先关闭窗口协程入口并取消等待者，再停止 runner、等待 GPU idle、消费完成消息并取消应用调度任务，由 `DestroyRuntime`
按固定顺序释放对象。WindowManager 借用的 RenderSystem 引用在后者销毁前清空；AssetManager
销毁并收束加载协程后才销毁 AssetDatabase；交换链释放后才断开窗口与 GPU 的双向引用并销毁 device。
借用引用的提供者必须活过消费者的清理，完整时序见[帧与 GPU](frame-and-gpu.md#关停顺序)。
