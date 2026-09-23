> - 适用: Application、多 World、组件渲染连接、场景交付与共享渲染服务
> - 权威: 本文描述 runtime 宿主与场景边界；GPU 帧与上传见 frame-and-gpu
> - 锚点: `modules/runtime/include/radray/runtime/application.h`, `modules/runtime/src/application.cpp`, `modules/runtime/include/radray/runtime/world_manager.h`, `modules/runtime/src/world_manager.cpp`, `modules/runtime/include/radray/runtime/render_system.h`, `modules/runtime/src/render_system.cpp`, `modules/runtime/include/radray/runtime/game_framework/`, `modules/runtime/include/radray/runtime/components/`, `modules/runtime/include/radray/runtime/render_scene/`

# Runtime 宿主、World 与渲染场景

runtime 采用立即创建、延迟销毁、统一 Tick 轮次与类型化增量交付。每个 Scene 只有一份 RT CPU 数据；
flight 保存更新包和必要 owner。没有内置 Forward/RenderGraph、视图执行 API 或完整场景快照。
旧渲染器设计见[历史快照](../temp/render-framework-design.md)。

## Application 与驱动边界

`Application::Run` 按 `ApplicationRuntimeDescriptor::Systems` 创建所选服务，必要时初始化窗口，然后调用 OnInit；
默认启用 Window、Gpu、Render、World、Asset，但不创建默认 World。各 getter 对未启用的系统返回空。
`RequestExit()` 可在没有窗口时结束循环。OnInit 返回后以 bootstrap S1
收束连接和销毁请求；初始数据等首个 writable flight 封包，不伪造发布或 GPU 完成。

| 正常入口 | 责任 |
|---|---|
| S0 `ServiceFrameBoundaryGT` | 完成事实、框架 owner 释放、等待通知、资产结果与 scheduler；runner 的 PrepareFrame 同阶段执行窗口维护与 writable 背压 |
| S1 `FinalizeWorldAndSealGT` | Tick/World CPU 借用结束后，冻结生命周期请求、注销和连接，再 Collect 最终值并 Seal |
| S2 `ApplySceneUpdatesRT` | BeginFrameRecord 后、OnRender 前，有序 Apply；先结束上一轮 CPU Scene 读者 |

输入与 OnUpdate 后，WorldManager 开始全局 TickEpoch。S1 不等待 GPU。RT 跳过绘制仍消费已发布包。
GPU 的单线程和双线程 runner 共用这些协议；ready 交接、CPU 提交完成计数与主队列 fence 是实际同步权威。
不启用 Gpu 时使用单线程 CPU runner：按 `FlightDataCount` 轮转槽位，泵可选资产和 scheduler，
如有 RenderSystem 则在 Update 后同步发布、消费场景包，并于下一帧边界或退出时完成退休。
该模式不调用 OnRender 或 OnRenderFrameComplete；多线程模式要求 Gpu。
普通组件不得自行驱动这些入口或调用 WaitIdle；未使用 Application runner 的 CPU 测试显式调用 World 的
`Tick`、`FinalizeWorldGT`、`CollectRenderUpdates`、`ShutdownWorld`，或对应 WorldManager 驱动。
托管 World 不能自行 Tick/Finalize。析构只做末端资源释放，已注册对象必须先显式 teardown。

## 立即创建与 Tick

`SpawnActor`、`AddComponent` 返回已经存在的对象。顺序为验证目标 → 分配身份/owner → 容器追加 →
同步注册/创建通知；回调前所有权和查询已成立。独占 draft 可以先配置，加入 World 时才获得 Tick 资格。

WorldManager 维护唯一 TickEpoch；独立 World 的显式驱动维护自己的轮次。World/Actor/Component 加入调度域时
记录 `FirstTickEpoch = CurrentTickEpoch + 1`。输入和 OnUpdate 创建者可以参加紧接着的轮次；Tick 中创建者
从下一轮开始，与目标 World/Actor 是否已经遍历无关。暂停不阻止 S1、Ready 或渲染同步，恢复不补跑历史 Tick。

WorldManager、Actor/Component 的调度数组均固定入口长度，按索引逐项取 ID 或对象地址，
回调后不保留数组元素引用。Tick 回调只允许追加，既有条目到 S1 才能移除；因此 WorldManager 不复制 WorldId 列表，
SparseSet 或数组扩容也不会使已取得的独立 World/Actor/Component 对象地址失效。
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
WorldManager 在任何回调前移交本批 World owner、失效身份并清空销毁请求，后续回调只追加下一批请求，
无需另存执行中的 WorldId 列表。Actor/Component 的 Destroying 标记在准备阶段写一次，通知阶段不重复遍历标记。

销毁回调可以立即向其他存活 Actor 或存活 World 新增对象。本次通知新提交的销毁/重挂接/连接请求留下一次 S1，
但 Pending 立即生效；新建后立即 Pending 的源不会发布空 primitive。Stopping 阶段禁止新业务创建。
`Clear` 是空闲 manager 的显式批量关闭，之后仍可创建；`Shutdown` 进入不可恢复的 Stopping。

## 层级与渲染连接

未注册 draft 的 AttachTo、DetachFromParent、SetRootComponent 立即生效。AddSceneComponent 的初始 parent 在
对外注册通知前建立，仅新增节点和边。已注册节点改用 RequestReparent / RequestSetRootComponent，S1 前查询仍见旧关系。
KeepLocal 保留局部 TRS；KeepWorld 要求新局部矩阵可以精确表达为 TRS，自身/后代、跨 World、奇异矩阵和 shear
在修改前拒绝，提交时重新验证。删除父组件会解除其他 Actor 的幸存 child，采用 KeepLocal，不误删别人的 owner。
变换 setter 对完全相同的值短路；有变化时立即更新本地 TRS、递增 World 的变换版本，并把直接修改的
节点加入局部更新队列。同一节点通过队列下标去重，写入不检查祖先、不访问后代、不合成矩阵。
World 不保留 local-to-world 矩阵，也不缓存查询结果；`GetWorldTransform` 与 `GetWorldMatrix` 均返回按值
快照，每次从当前节点向根迭代合成局部 TRS 并相乘。查询为 O(深度)，额外空间 O(1)，不要求先 Collect
或派发通知；解除层级后的查询立即看到新关系。该接口适合查询较少的业务。

业务通知独立于渲染局部更新队列。`FinalizeWorldGT` 在处理完生命周期后，从本批直接修改的候选节点中
排除有存活脏祖先的节点，再迭代遍历受影响子树派发 `OnTransformChanged`。候选过滤可能沿父链检查，
成本移到通知阶段；通知必须访问实际受影响的后代。没有 renderer 的 World 同样派发，draft 仍立即通知。
通知批在回调前摘下并推进 epoch，使旧登记整体失效，无需逐节点重置队列下标；
回调再次修改产生下一批通知，但本帧 Collect 读取修改后的最终局部值。
重挂接和解除层级在改变 parent 前移除原队列登记，再重新登记该节点。World 的 child 数组使用下标和
交换末项实现 O(1) 摘链，不保证 child 枚举顺序；父链防环校验和业务通知的遍历成本仍然存在。

连接中的 World 为 SceneComponent 建立独立 TransformId，包含普通空间节点和中间祖先。
Collect 仅读取直接变化节点的最终局部 TRS 与 parent，SceneWriter 合并同次 Seal 前的重复更新。
直接更新数达到 16384 时先按组件地址排序，以改善大批量读取局部值的访问顺序，额外成本为 O(k log k)。
内置 StaticMesh/Light 绑定 TransformId；祖先移动不需要捕获后代渲染组件。自定义 RenderComponent 默认保留
显式世界矩阵捕获，可通过 `UsesSceneTransform` 选择层级绑定；仅存在这类兼容来源时，World 才维护渲染脏根
并遍历捕获其最终值。捕获 epoch 保证重叠子树只输出一次，独立 Collect 不提前派发业务通知。

Pending 来源不执行通知或业务捕获。Scene 的层级依赖保留到实际注销，期间已排队的局部值继续同步，
保证跨 Actor 的存活子节点与即时世界坐标查询一致。注销父节点后，幸存子节点按 KeepLocal 脱离。
新建后立即 Pending 的来源不会发布空 primitive，但可能先发布没有 Shape 的 Transform 节点，下一次 S1
真正注销时删除。未 Seal 的 Transform 创建和删除可以相消。

World 指针在组件加入 World 时缓存；draft Actor 加入时在任何注册回调前给其全部组件写入上下文，
因此子组件先注册时也能观察到尚未注册的祖先变化。

普通 SceneComponent 只负责空间层级与变换。需要渲染连接的组件继承 `RenderComponent`；Primitive 和 Light
均继承它。框架的 final 变换捕获入口合并 Transform 与其他渲染 dirty，派生 `OnTransformChanged` 只处理业务副作用，
不需要调基类。公开 MarkRender* 仍先 CheckCanModify；内部标记只更新 bridge 登记表。
`World::CheckCanModify` 的拥有线程检查只在 Debug 执行；Release 仍拒绝 Collect 期间的修改。

World/manager 通过 `RequestRenderConnection` 请求连接，显式 `RequestReconnect` 强制新连接。
请求目标与已提交 SceneId 可分别查询，不能把请求 Accepted 当作连接已完成。
已连接的 renderer 由 WorldRenderBridge 持有，World 从 bridge 查询，不保存第二份连接指针。
WorldRenderBridge 的 Disconnected / Connecting / Connected / Disconnecting 状态独立于游戏注册。
连接回调请求切换只排队，不能 reset 执行中的 bridge。Connecting 中立即创建者接入一次；Disconnecting 中创建
游戏对象仍成功，但不接回正在拆除的 Scene。重连从最终游戏值建立新 SceneId，旧 Scene 可以继续退休。
注销先写 Unregistering，再拆 render state，最后 OnUnregister；不依赖派生 hook 调基类。

## 增量捕获与 RT 数据

| 对象 | 职责 |
|---|---|
| WorldManager / World | GT 游戏对象、身份、统一调度、生命周期请求 |
| WorldRenderBridge | GT 内部适配器、渲染源登记与 dirty 队列的唯一管理者 |
| SceneWriter | Scene 唯一 GT producer、Shape/Light/Transform 身份、局部变换与几何增量、光源参数快照、资产 owner |
| RenderScene | 单份 RT 数据、Apply、只读借用与 CPU reader lease |
| RenderSystem | GT/RT 分离的登记表、flight 交付协议、shader/render-pass 服务 |

RenderComponent 分类 State/Transform/DynamicData dirty；同来源只排队一次。bridge 维护来源上的登记下标、
dirty 队列下标与 dirty 位，连接身份从 World 查询；普通 SceneComponent 不保存这些字段。
登记表和 dirty 队列直接存来源指针。Collect 先提交局部变换，再处理兼容来源的世界矩阵捕获与其他
State/DynamicData/显式 Transform dirty。内置 StaticMesh 的 State 只更新几何绑定及 TransformId，
空间变化无需重新输出 MeshState。Collect 外 dirty 位为空表示未排队；删除时交换末项并修复下标。
断开连接遍历已登记的来源和 Transform。Collect 跳过 Pending 来源的业务捕获，且禁止游戏修改、
生命周期请求、重入封包和所属 Application 的 asset/scheduler Pump。派生 setter 必须先 CheckCanModify。
World/WorldManager 负责游戏侧 Collect 禁令，RenderSystem 负责封包及 Application 调度禁令；bridge 不另存同一阶段标志。
PrimitiveComponent 的 final 入口负责 ShapeId 创建/注销，CollectPrimitiveUpdates 接收 ShapeCapture，
只输出自身最终的 mesh 状态或 transform（至多一条，也可以不输出以保留 bare shape）。
一般 RenderComponent 接收 SceneCapture，可以登记和捕获多个 ShapeId/LightId；一个身份在同轮 Collect 中
由其来源捕获一次。捕获接口不提供创建/删除身份或修改 World 的操作。
LightComponent 捕获类型化光源值，使用独立的 LightId。ShapeId、LightId、TransformId 是不可隐式互转的强类型，
均包含 Index/Generation，只在所属 SceneId 内有效；三种独立身份池允许使用相同数值的槽位与代次。

独立工具由 CreateSceneGT/GetSceneWriterGT 获得 writer，遵守同样的绑定和交付契约。
World 独占的 writer 不向外提供；独立 writer 在 DestroySceneGT 后不可继续使用。
Shape 在 Seal 前的 Create/Remove 可以相消，已 Seal 的创建只能由后续 Remove 有序退出。
独立 writer 的 State 覆盖最终世界矩阵，transform-only 更新不重建 mesh。绑定层级的 State 与局部变换独立合并。
更新数组复用容量，工作与唯一 dirty 源有关。
SceneWriter 直接构建本批 SceneUpdateBatch。存活 Shape 只保存资产使用身份、是否已发布与是否有 mesh 状态；
不常驻矩阵、mesh 描述或更新下标。World 通过 SceneCapture/ShapeCapture 将去重后的最终值直接追加到变化包；
常规 Shape Flush 只标记创建已发送，不遍历 mesh/显式 transform 记录重组矩阵。Transform 状态另外保存 parent、
创建/局部/重挂接记录的下标，Flush 清理实际发送记录的下标，再把变化包与 flight 的空包交换。

独立 writer 的反复编辑使用单独的 ShapeEdit 槽，保存创建/更新下标与更新种类。SetTransform 覆盖已有记录；
SetStaticMesh 吸收同一身份的 Transform，取消时交换末项并修复下标。World 在同次 Seal 前再次 Collect、
捕获后删除身份，或通过生命周期入口直接编辑 writer 时，也按需建立这些定位信息，继续保证最终值合并；
World 的下一批恢复直接提取。编辑槽按 ShapeId.Index 索引，代次仍由 ShapeState 的身份池验证。
独立 writer 在 CreateShape 时入队创建；World 先保留身份，最终 CaptureShape 才入队，未捕获便删除的源
不会发布默认几何。显式空 mesh 也满足后续 SetTransform 的前置条件。
Shape 的可选资产使用身份对应 RenderAssetLifetime 中一次计数；是否持有使用与 AssetId 是否为空无关。

内置层级变换协议为 `LocalTransformUpdate`：8 B TransformId + 40 B 紧凑 TRS，共 48 B。
创建记录另带 Parent，共 56 B；重挂接只增发 16 B 的身份对；删除为 8 B 身份。TRS 的 quaternion 顺序为
x/y/z/w，不受 Eigen SIMD 对齐配置影响。新节点在首次 Seal 前的局部编辑直接并入创建记录。

RT 的 `SceneTransform` 用整数行索引维护 parent/first-child/next/previous，并分列保存局部 Matrix4f、
世界 Matrix4f 与状态。局部 TRS 只在收到修改时 compose，祖先移动重算后代时复用局部矩阵。
子树入口直接求值，只有后代进入临时栈，独立叶节点无需入栈出栈；父子合成保留 Eigen 矩阵乘法。
物理行在存活期间不移动，删除行由 free-list 复用；重挂接只改整数链接，无须重排整片子树。
连续矩阵列有利于相邻节点访问，但频繁复用后不保证子树连续，也没有后台重排或分页压缩。

Apply 先应用本批全部局部值与拓扑，再计算变换。修改节点按 epoch 去重；当 seed 至少为 256 且占行池
至少 1/8 时，顺序扫描行池重建 seed 顺序，扫描行数有 8k 的上界，避免密集随机访问；稀疏更新不扫描行池。
单个 seed 或所有 seed 均无父节点时
直接遍历。其他情况先按 epoch 标记受影响子树，从 seed 中筛出没有受影响父节点的根，再按父先子后顺序计算。
每个受影响世界矩阵只乘一次，一般路径会额外访问一遍受影响节点的整数元数据；不会扫描全部 Scene。
设直接变化节点为 k，最终受影响节点为 a，稳态标记与求值为 O(k+a)，重挂接本身 O(1)，删除还需访问直接子节点。
临时数组复用容量；空间随节点数及历史容量峰值增长，当前实现不主动收缩。

已评估并回退“冻结 World 后由 RT 直接导入变化 local”和“Scene 全量求值”两项实验。
直接导入省去逐项值包，但输入握手及来源清理会压缩 GT/RT 的重叠，多个重点双线程负载出现性能回退；
全量求值还会扩大 bounds/光源更新范围，在稀疏负载下代价明显增加。因此继续采用按值交付 local 与增量求值，
不保留实验开关，也不新增仅按 dirty 数量/比例切换同步或求值方式的阈值。
测量范围、收益例外与完整数据见 [实验及回退结论](../temp/scene-direct-import-benchmark-2026-09-23.md)。

兼容捕获与独立 writer 仍可发送 `ShapeTransformUpdate`：8 B ShapeId + 列主序 3×4 AffineTransform，共 56 B，
隐含齐次行 (0,0,0,1)，保留负缩放和仿射 shear。RT 为这类 Shape 分配无父节点的变换行。
绑定 TransformId 的 Shape 不接受显式世界矩阵更新，必须修改对应局部节点。非仿射输入在压缩时、
非有限渲染变换在更新 bounds 时做 Debug 校验。

SceneWriter 分别维护 Shape 状态、Light 身份池与 LightSceneData 最终值；Light 槽只存当前表种类和行号，
Light 没有逐项 dirty/删除队列。CreateLight 只保留 GT 身份，首次 SetLight 后才进入光源集合。
任一 SetLight 或已有数据的 RemoveLight 直接设置待提交包的 LightsChanged，不另存 dirty 标志；
Flush 按此标志把完整的四类光源表按值封存到 flight，交换空包后自然恢复未变更状态；
未改变的光源也包含在内，但不重新调用其组件捕获。
未捕获便删除的组件不会发布默认光源；独立 writer 在同次封包前 SetLight 再 RemoveLight，允许交付最终空集合。
SceneUpdateBatch 的 LightsChanged=false 表示保留 RT 光源，true 表示全量替换，空列表明确清除所有光源。
无参数 dirty 时不复制光源快照。内置光源的 Common 带 TransformId，GT/flight 中的位置与方向为占位值，
RT Apply 后的类型化表提供最终世界位置与方向。祖先移动只发送局部变换，无需重新捕获光源参数。
当前 RT 在参数替换或任意变换更新后扫描光源表。参数替换时，同一行的身份及 Transform 不变、矩阵未更新的
光源保留 RT 姿态；其他行重新求值。纯变换帧只为实际受影响的光源求位置与旋转，没有逐节点光源链表。
自定义光源可覆盖 UsesSceneTransform 返回 false，继续由 GetLightPosition/GetLightDirection 捕获显式世界姿态。
已 Seal 的列表不受后续修改、删除、身份复用影响。

StaticMesh 在构造时验证 CPU mesh/bounds 并补全默认 sections 一次，之后数据不可变。
StaticMesh 拥有一份 StaticMeshRenderData，集中存放 GPU mesh、sections 与局部 bounds。
StaticMeshDescription 只保存 AssetId 和指向该渲染数据的只读借用；所有实例与 Scene 共享资产中的同一份数据，
每个实例只保存自己的绑定、矩阵、世界 bounds 与 ReverseCulling。借用由 GT 资产 owner 保护，RT 不参与引用计数。
Loading、失败或无效 mesh 产生空几何，空几何的世界 bounds 退化到变换原点。
组件 Ready 通知检查 Live、注册、SceneId、ShapeId 和 mesh 请求身份；改绑/注销停止旧等待而不取消共享加载。

RenderScene 按 Shape 删除 → Transform 删除/创建/改父/局部值 → Shape 创建/绑定 → 显式世界矩阵
→ 层级求值/bounds → 光源参数与姿态的顺序应用。ShapeSlot 保存身份、稠密 mesh 行号、Transform 行号
以及同一 Transform 上的 Shape 双向链。受影响 Transform 只更新其绑定 Shape 的 bounds；纯几何改绑也会刷新 bounds。
StaticMeshTable 拥有身份、几何绑定、TransformRows、bounds 四个稠密列，bounds 包含 ReverseCulling。
删除一起交换末项并修复 ShapeSlot；实例不逐个堆分配。Matrix4f 只由 SceneTransform 持有，多个 Shape 可以绑定同一节点。
`GetStaticMeshColumns().Transforms` 是整个变换行池，必须以 `TransformRows[meshRow]` 索引，不能用 meshRow
直接索引，也不保证长度等于 Shape 数；`Columns::Get` 已做此映射。剔除仍可只顺序读取稠密 Bounds 列。
GetStaticMeshes 保留身份枚举，包含资产未就绪的已登记 mesh。行号不表示身份，所有列与单项借用在下次 Apply
或销毁时失效，跨线程 CPU 读取需要相同的 reader lease。CPU 列布局不等同于 GPU buffer packing。
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
尺寸占位。组件侧由叶子直接构造自己的类型化记录，不再基类造 variant 再逐层 visit 改写。SceneWriter::SetLight 通过身份槽直接定位类型和行号，O(1) 原位替换；类型改变时从旧表移除并加入新表。
RemoveLight 两列一起交换末项填补空位，并修复被移动身份的行号。类型变化保留
LightId，行号与顺序不表示身份。GetLights 返回只读分类数据及类型化查找；GetLight 仅借用 Common 参数。
LightsChanged 时复制完整分类表并复用容量，RT 不再逐条按 Type 分组。快照中的身份必须有效且唯一，两列长度必须一致。
LightSceneData 自身的 Set/Remove 保留线性查找便利接口，不用于 SceneWriter 的逐灯热更新。
RT 不维护 Light 槽位映射或代次墓碑；GetLight/ContainsLight 线性扫描少量光源并匹配完整 LightId，
仅在 GT 保留但没有参数的身份在 RT 不可见。旧 ID 更新由 writer 校验拒绝，旧快照由交付序号阻止重复/乱序消费。
RT 或停止后的检查可借用数据，普通借用截止到下一 Apply。并行 CPU 读者在 RT 派发前获取 AcquireRead lease，
在任务结束时释放；Apply 和 Scene 析构等待已有 lease，不等 GPU。RT 必须先停止派发旧 Scene 的新读者。
RenderScene 不可复制/移动；不能把 reader lease 持到依赖下一次 Apply 才能结束的工作中。

## 交付、退出与资产保活

flight 严格经过 Writable → Sealed → Published → Consumed → completion 后 Writable。
这些状态直接属于 RenderSystem 的 flight 包；每个入口在修改 payload/owner 前校验一次，处理成功后提交状态，
不另设返回错误码的交付状态机或重复验证层。非法次序仍按不变量违反诊断。
Seal 分配单调 UpdateSequence，Publish 验证顺序；Consume 在 Apply 前验证 Published、下一序号与新的 FrameSerial。
即使同 generation 的合法 transform 也不能乱序或重复消费。completion 必须匹配当前 Consumed 包的 FrameSerial。
F 与 backbuffer count 无关；功能测试覆盖 F=1/2/3/8 的单/双线程 runner。

GT 场景记录按值保存在 SparseSet 中，只有对外借用的 SceneWriter 独立分配以保持地址稳定。
封包直接遍历场景记录，从 writer 取得 SceneId；不维护另一份 ID 列表，场景间不依赖封包中的排列顺序。
writer 的 closing 状态同时表示待封包的 Scene 删除，不再维护另一份 DestroyPending。

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
真实 GPU 验收见 GpuSceneLifetime，CPU/runner 验收见 WorldLifecycle、SceneDelivery、FrameScenarios、SceneAssets、StaticMeshScene、LightScene。

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

Application 依次按选项构造 WindowManager、GpuSystem（包含 device）、RenderSystem、WorldManager、AssetManager 和
可选 AssetDatabase；World 由应用在 `OnInit` 或后续 GT 更新阶段通过 WorldManager 显式创建。
Window 单独启用时只有原生主窗口，Window 与 Gpu 同时启用时再挂接主交换链；Render 单独启用可交付 CPU Scene。
默认 importer 只保留登记与明确失败的加载入口，不借用上传调度器。全部对象就位后直接接线：GpuSystem 提供帧等待接口；无 Gpu 的 AssetManager 使用即时完成的 CPU 帧等待器。AssetDatabase
提供可选资产来源；未配置资产根或数据库打开失败时，资产来源为空。
WindowManager 与 GpuSystem 的双向引用在启动渲染线程前建立。

RenderSystem 与 Gpu 同时启用时才调用 `RenderSystem::OnInitialize()` 创建 shader/program 与 render-pass caches。
`GpuSystem::TryCreate` 逐步检查后端、adapter、验证层、device、队列及 profiler；`Run(desc, startup)`
报告启动状态，未编译后端、无 adapter、缺验证层与真实初始化失败可区分。配置错误在 OnInit 前失败；
初始化失败和窗口或交换链创建失败均走 `DestroyRuntime` 清理路径。
RenderSystem 的析构接受部分初始化状态，通过 `OnShutdown` 幂等释放缓存。

正常关停先关闭窗口协程入口并取消等待者，再停止 runner、等待 GPU idle、消费完成消息并取消应用调度任务，由 `DestroyRuntime`
按固定顺序释放对象。WindowManager 借用的 RenderSystem 引用在后者销毁前清空；AssetManager
销毁并收束加载协程后才销毁 AssetDatabase；交换链释放后才断开窗口与 GPU 的双向引用并销毁 device。
借用引用的提供者必须活过消费者的清理，完整时序见[帧与 GPU](frame-and-gpu.md#关停顺序)。
