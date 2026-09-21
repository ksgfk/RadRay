> - 适用: Application、多 World、组件渲染连接、场景交付与共享渲染服务
> - 权威: 本文描述 runtime 宿主与场景边界；GPU 帧与上传见 frame-and-gpu
> - 锚点: `modules/runtime/include/radray/runtime/application.h`, `modules/runtime/src/application.cpp`, `modules/runtime/include/radray/runtime/world_manager.h`, `modules/runtime/src/world_manager.cpp`, `modules/runtime/include/radray/runtime/render_system.h`, `modules/runtime/src/render_system.cpp`, `modules/runtime/include/radray/runtime/game_framework/`, `modules/runtime/include/radray/runtime/components/`, `modules/runtime/include/radray/runtime/render_scene/`

# Runtime 宿主、World 与渲染场景

runtime 采用立即创建、延迟销毁、统一 Tick 轮次与类型化增量交付。每个 Scene 只有一份 RT CPU 数据；
flight 保存更新包和必要 owner。没有内置 Forward/RenderGraph、视图执行 API 或完整场景快照。
旧渲染器设计见[历史快照](../temp/render-framework-design.md)。

## Application 与驱动边界

`Application::Run` 创建服务、初始化窗口并调用 OnInit，不创建默认 World。OnInit 返回后以 bootstrap S1
收束连接和销毁请求；初始数据等首个 writable flight 封包，不伪造发布或 GPU 完成。

| 正常入口 | 责任 |
|---|---|
| S0 `ServiceFrameBoundaryGT` | 完成事实、框架 owner 释放、等待通知、资产结果与 scheduler；runner 的 PrepareFrame 同阶段执行窗口维护与 writable 背压 |
| S1 `FinalizeWorldAndSealGT` | Tick/World CPU 借用结束后，冻结生命周期请求、注销和连接，再 Collect 最终值并 Seal |
| S2 `ApplySceneUpdatesRT` | BeginFrameRecord 后、OnRender 前，有序 Apply；先结束上一轮 CPU Scene 读者 |

输入与 OnUpdate 后，WorldManager 开始全局 TickEpoch。S1 不等待 GPU。RT 跳过绘制仍消费已发布包。
两个 runner 共用这些协议；writable/ready、主队列 fence 仍是实际同步权威，没有第二套提交体系。
普通组件不得自行驱动这些入口或调用 WaitIdle；CPU-only 测试显式调用 World 的
`Tick`、`FinalizeWorldGT`、`CollectRenderUpdates`、`ShutdownWorld`，或对应 WorldManager 驱动。
托管 World 不能自行 Tick/Finalize。析构只做末端资源释放，已注册对象必须先显式 teardown。

## 立即创建与 Tick

`SpawnActor`、`AddComponent` 返回已经存在的对象。顺序为验证目标 → 分配身份/owner → 容器追加 →
同步注册/创建通知；回调前所有权和查询已成立。独占 draft 可以先配置，加入 World 时才获得 Tick 资格。

WorldManager 维护唯一 TickEpoch；独立 World 的显式驱动维护自己的轮次。World/Actor/Component 加入调度域时
记录 `FirstTickEpoch = CurrentTickEpoch + 1`。输入和 OnUpdate 创建者可以参加紧接着的轮次；Tick 中创建者
从下一轮开始，与目标 World/Actor 是否已经遍历无关。暂停不阻止 S1、Ready 或渲染同步，恢复不补跑历史 Tick。

WorldManager 保留 WorldId 快照，因为 SparseSet 的物理 storage 可能移动。Actor/Component 的 owner 数组
采用固定入口长度和按索引取对象地址，回调后不保留数组元素引用。回调只允许追加，既有 owner 不移动/删除。
`Actor::Tick` 是业务 hook；框架 dispatcher 在其返回后仍负责组件 Tick，派生不需要调用基类来驱动组件。
`Actor::Tick` 与 `ActorComponent::TickComponent` 默认不进入调度。`SetTickEnabled(true)` 后才加入 World 的 ticking 列表；
空闲 World 的 Tick 只遍历该列表，不扫描全部 Actor。覆盖 Tick / TickComponent 的派生必须自行启用。
组件启用不等于调用未启用的 `Actor::Tick`。`World::SetTickEnabled` 仍是整 World 暂停。
每个 hook 返回和下一组件派发前都检查 Live/epoch。递归 Tick、提交或跨线程修改会诊断。
公开 span 是只读借用，调用者持有期间不能触发导致数组扩容的创建。

## 身份、注册与延迟销毁

WorldId 在所属 manager 内有效；ActorId 包含 WorldId，ComponentId 包含 ActorId，均含槽位和 generation。
独立 World 的 ActorId 只在该 World 内有效。跨 S1 保存非拥有引用使用身份；裸指针不保证跨 S1 有效。
槽位 generation、TickEpoch 和交付序号耗尽时拒绝回绕。

对象寿命为 Initializing / Live / PendingDestroy / Destroying。组件另有
Unregistered / Registering / Registered / Unregistering，不与渲染连接混成一套状态。
注册前写 Registering，建立渲染关联，再 OnRegister，返回后写 Registered；OnRegister 中 IsRegistered 为 false。
新加兄弟组件由 Add 自己注册，外层固定长度遍历不重复注册。初始化期间自销毁仍完成当前注册序列，
随后停止尚未开始的注册和 OnSpawned；创建函数可能返回仍占有内存的 Pending 对象。
只有实际进入过 OnSpawned 的 Actor 才派发配对 OnDestroyed。

Destroy/Remove 首次返回 Accepted，重复返回 AlreadyPending，无效目标返回 Invalid。请求立即影响 Live 查询
和后续 Tick，但不当场注销、取消任务、解除层级或释放引用。父 Pending 自动使子对象不可调度，不必扫描全部子对象。
Pending 不可取消或复活，且不能向 Pending/Destroying/Stopping owner 新增对象。

S1 先冻结所有 World 的本批销毁、层级和连接请求，再处理父覆盖子、身份失效与受影响 owner 数组的稳定压缩，
最后派发注销/销毁通知。幸存对象顺序保持；没有请求时不扫描 Actor 寻找 pending。有删除时成本包含受影响容器
的线性压缩及请求排序，不能当成 O(删除数)。OnUnregister 时 owner/world 上下文仍有效；清理后才释放内存。

销毁回调可以立即向其他存活 Actor 或存活 World 新增对象。本次通知新提交的销毁/重挂接/连接请求留下一次 S1，
但 Pending 立即生效；新建后立即 Pending 的源不会发布空 primitive。Stopping 阶段禁止新业务创建。
`Clear` 是空闲 manager 的显式批量关闭，之后仍可创建；`Shutdown` 进入不可恢复的 Stopping。

## 层级与渲染连接

未注册 draft 的 AttachTo、DetachFromParent、SetRootComponent 立即生效。AddSceneComponent 的初始 parent 在
对外注册通知前建立，仅新增节点和边。已注册节点改用 RequestReparent / RequestSetRootComponent，S1 前查询仍见旧关系。
KeepLocal 保留局部 TRS；KeepWorld 要求新局部矩阵可以精确表达为 TRS，自身/后代、跨 World、奇异矩阵和 shear
在修改前拒绝，提交时重新验证。删除父组件会解除其他 Actor 的幸存 child，采用 KeepLocal，不误删别人的 owner。
变换 setter 对完全相同的值短路；通知遍历后代并标记本子树缓存脏。TRS 变更经 `NotifyTransformChanged`
统一入口：只做脏标记与渲染脏入队，不做矩阵运算。无子节点时只标脏自身并通知，避免第二次空遍历；
有后代时仍先 `InvalidateWorldSubtree` 再 `NotifySubtree`，保证回调中读取后代世界矩阵不会落到过期缓存。
框架在 Primitive/Light 上自动入队 Transform dirty；普通 SceneComponent 改变换不产生渲染更新。
`OnTransformChanged` 只保留业务副作用。`GetWorld` 在注册时缓存 World 指针，不再经 Actor 跳转。`GetWorldMatrix` 在缓存脏时沿 parent chain 重算
（compose local 一次，链上每个祖先只算一次后缓存），干净时直接返回缓存；返回值始终等价于当场递归求值。
重算走迭代实现：先自下而上收集脏链，再自上而下 compose，链长超过固定窗口时分多轮从顶部收敛，
栈消耗与层级深度无关。不要改回按 parent 递归，Debug 下单帧因 Eigen 乘法展开可达约 10 KB，
几百层即耗尽默认 1 MB 栈。
解除层级会把幸存子树标脏，由懒重建刷新。缓存代价是每个 SceneComponent 64 B 矩阵；该矩阵排在 TRS/入队热数据之后，
避免 Mutate 写下脏标志时连带加载。静态场景因此多付的 Tick 成本由跳过空闲派发回收，不作为保留缓存的理由。
派生 `OnTransformChanged` 走内部 `MarkRenderDirty` 时不再重复做线程检查；公开 `MarkRender*Dirty` 仍
`CheckCanModify`。`World::CheckCanModify` 的拥有线程检查只在 Debug 执行；Release 仍拒绝 Collect 期间的修改。

World/manager 通过 `RequestRenderConnection` 请求连接，显式 `RequestReconnect` 强制新连接。
请求目标与已提交 SceneId 可分别查询，不能把请求 Accepted 当作连接已完成。
WorldRenderBridge 的 Disconnected / Connecting / Connected / Disconnecting 状态独立于游戏注册。
连接回调请求切换只排队，不能 reset 执行中的 bridge。Connecting 中立即创建者接入一次；Disconnecting 中创建
游戏对象仍成功，但不接回正在拆除的 Scene。重连从最终游戏值建立新 SceneId，旧 Scene 可以继续退休。
注销先写 Unregistering，再拆 render state，最后 OnUnregister；不依赖派生 hook 调基类。

## 增量捕获与 RT 数据

| 对象 | 职责 |
|---|---|
| WorldManager / World | GT 游戏对象、身份、统一调度、生命周期请求 |
| WorldRenderBridge | GT 内部适配器、连接、组件 dirty 去重；不进入公共 API |
| SceneWriter | Scene 唯一 GT producer、ShapeId/LightId、几何增量与光源全量捕获、资产 owner |
| RenderScene | 单份 RT 数据、Apply、只读借用与 CPU reader lease |
| RenderSystem | GT/RT 分离的登记表、flight 交付协议、shader/render-pass 服务 |

组件分类 State/Transform/DynamicData dirty；同组件只排队一次。Collect 跳过 Pending，且禁止游戏修改、
生命周期请求、重入封包和所属 Application 的 asset/scheduler Pump。派生 setter 必须先 CheckCanModify。
PrimitiveComponent 的 final 入口负责 ShapeId 创建/注销，CollectPrimitiveUpdates 捕获派生值。
LightComponent 捕获类型化光源值，使用独立的 LightId。两种 ID 是不可隐式互转的强类型，均包含
Index/Generation，只在所属 SceneId 内有效；独立身份池允许 Shape 和 Light 使用相同数值的槽位与代次。

独立工具由 CreateSceneGT/GetSceneWriterGT 获得 writer，遵守同样的绑定和交付契约。
World 独占的 writer 不向外提供；独立 writer 在 DestroySceneGT 后不可继续使用。
Shape 的未发布 Create/Remove 可以相消，已 Seal 的创建只能由后续 Remove 有序退出。
State 覆盖最终 transform，transform-only 更新不重建 mesh。Shape 更新数组复用容量，工作与唯一 dirty 源有关。

SceneWriter 分别维护 Shape 状态、Light 身份池与 LightSceneData 最终值；身份池不存光源参数，
Light 没有逐项 dirty/删除队列。CreateLight 只保留 GT 身份，首次 SetLight 后才进入光源集合。
任一 SetLight 或已有数据的 RemoveLight 将集合标脏，Flush 把完整的四类光源表按值封存到 flight；
未改变的光源也包含在内，但不重新调用其组件捕获。
未捕获便删除的组件不会发布默认光源；独立 writer 在同次封包前 SetLight 再 RemoveLight，允许交付最终空集合。
SceneUpdateBatch 的 LightsChanged=false 表示保留 RT 光源，true 表示全量替换，空列表明确清除所有光源。
无 dirty 时不遍历或复制光源；已 Seal 的列表不受后续修改、删除、身份复用影响。

StaticMesh 在构造时验证 CPU mesh/bounds 并补全默认 sections 一次，之后数据不可变。
StaticMeshDescription 持有 AssetId、bounds 并借用 `span<const StaticMeshSection>` 与 `const GpuMesh`；
这些借用由同一资产 owner 保护，实例不复制 sections。Loading、失败或无效 mesh 产生空几何。
组件 Ready 通知检查 Live、注册、SceneId、ShapeId 和 mesh 请求身份；改绑/注销停止旧等待而不取消共享加载。

RenderScene 按 Shape Remove → Create → Mesh → Transform → Light 全量替换应用。ShapeSlot 只保存几何身份
与稠密下标，不持有几何数据；StaticMeshProxy 按值存放在与 ShapeId 平行的稠密数组，逐 shape 不做堆分配，
移除交换末项填补空位，因此 Apply 可能移动记录。GetStaticMeshes 返回 ShapeId，包含资产未就绪的已登记 Mesh。
StaticMeshProxy 更新 matrix、world bounds、ReverseCulling，支持负缩放和仿射 shear。齐次行与 isfinite 的
逐条契约校验只在 Debug 执行（`RADRAY_IS_DEBUG`）；Release 信任 writer 封包内容，直接写矩阵并计算 AABB 与行列式。
LightSceneData 直接拥有四种类型的 LightTable，GT 最终值、flight 快照与 RT 数据均使用同一紧凑布局。
每张表是 Ids 与 Data 两列，Ids[row] 拥有 Data[row]，两列长度始终相等：

| 表的 Data 元素 | 保存的数据 |
|---|---|
| DirectionalLightData | Common、世界方向 |
| PointLightData | Common、PointLightParameters |
| SpotLightData | Common、PointLightParameters、内外半锥角 |
| RectLightData | Common、世界位置/方向、衰减半径与衰减模式/指数 |

Common 只保存种类无关参数：颜色/强度与影响世界/投影开关。身份在 Ids 列，不嵌在记录里，因此按身份查找只
扫描 8 字节连续的 id 列，不跨步走过整条记录。阴影 bias 归产出它的类型：PointLightParameters 保存世界位置/
方向、衰减参数、光源半径/软半径/长度与阴影 depth/normal bias，Point 的方向仍用于非零长度光源；
DirectionalLightComponent 的 CSM 配置（级联、阴影距离/分辨率、bias、PCF 模式）尚未接入 scene，
方向光记录当前不携带阴影参数。位置使用 Vector3f，不保存恒为 1 的齐次分量。这些是 CPU 数据，不与 GPU buffer
的对齐和 packing 绑定。Rect 仍是预留数据能力，尚无 RectLightComponent 和面积几何参数，不代表已实现完整
面积光渲染。

表的类型决定光源类型，每条记录不再保存 Type 标签、身份或其他类型专属字段。LightData 是只在单灯捕获与
SetLight 调用时使用的 variant，身份由调用方另行传入；不保存 vector<variant>，因此持久数据不按最大光源记录
尺寸占位。组件侧由叶子直接构造自己的类型化记录，不再基类造 variant 再逐层 visit 改写。SetLight 在 id 列
线性查找后原位替换，类型改变时从旧表移除并加入新表；RemoveLight 两列一起交换末项填补空位。类型变化保留
LightId，行号与顺序不表示身份。GetLights 返回只读分类数据及类型化查找；GetLight 仅借用 Common 参数。
LightsChanged 时复制完整分类表并复用容量，RT 不再逐条按 Type 分组。快照中的身份必须有效且唯一，两列长度必须一致。
RT 不维护 Light 槽位映射或代次墓碑；GetLight/ContainsLight 线性扫描少量光源并匹配完整 LightId，
仅在 GT 保留但没有参数的身份在 RT 不可见。旧 ID 更新由 writer 校验拒绝，旧快照由交付序号阻止重复/乱序消费。
RT 或停止后的检查可借用数据，普通借用截止到下一 Apply。并行 CPU 读者在 RT 派发前获取 AcquireRead lease，
在任务结束时释放；Apply 和 Scene 析构等待已有 lease，不等 GPU。RT 必须先停止派发旧 Scene 的新读者。
RenderScene 不可复制/移动；不能把 reader lease 持到依赖下一次 Apply 才能结束的工作中。

## 交付、退出与资产保活

flight 严格经过 Writable → Sealed → Published → Consumed → completion 后 Writable。
Seal 分配单调 UpdateSequence，Publish 验证顺序；Consume 在 Apply 前验证 Published、下一序号与新的 FrameSerial。
即使同 generation 的合法 transform 也不能乱序或重复消费。completion 必须匹配当前 Consumed 包的 FrameSerial。
F 与 backbuffer count 无关；功能测试覆盖 F=1/2/3/8 的单/双线程 runner。

SceneWriter 的 RenderAssetLifetime 在 GT 以 AssetId 去重持有绑定 owner。最后解绑进入候选，封包前重新绑定可以
撤销候选；最终零使用 owner 转入承载改绑/删除的 flight，真实 completion 后释放。没有每帧全资产重新 pin。
多 Scene 各有 owner；RT 仅借用，不复制或析构 StreamingAssetRef。Scene 删除在 RT Apply 后移除 CPU 记录，
GT SceneId 与 writer 保留到删除包完成。Actor 在 S1 析构，不受 GPU 在途影响。

手工 draw 或 raw RHI owner 在 writable 阶段交给 `GpuSystem::RetainForFrameGT`，独立于用户任务取消。
真实完成通过匹配 FrameSerial 释放；未发布 owner 只允许终止模式 abandon。覆盖当前有序主队列，增加独立队列前
需要扩展完成依赖。StaticMesh 已走使用者保活后零引用直接卸载；Texture 的保守迁移边界见[资产系统](asset-system.md)。

关停先进入 Stopping，排空已发布包和 CPU/GPU 使用，消费真实 completion，再 terminal abandon 未发布包。
普通窗口维护 drain 不执行 abandon。终止后禁止恢复发布。显式拆除顺序为
WorldManager → RenderSystem → AssetManager → AssetDatabase → GpuSystem；OnShutdown 也处于 Stopping。
初始化失败使用同一显式 teardown，但没有提交时不等不存在的 fence。

生命周期与变换通知的验收计数由测试 probe 持有；场景更新量直接检查已封存的更新包，资产准备检查
共享描述和实际内容。World、SceneWriter、RenderSystem、StaticMesh 与 AssetManager 不保存专供测试的累计统计。
性能测试自行记录阶段耗时、分配器统计和更新包大小；内部遍历与 owner 搬移次数不由运行时维护。
真实 GPU 验收见 GpuSceneLifetime，CPU/runner 验收见 WorldLifecycle、SceneDelivery、SceneAssets、StaticMeshScene、LightScene。

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
