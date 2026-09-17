# RadRay：World → 增量包 → 持久 Scene 实施计划

基线：`refactor/render-framework-reset`，`da7731eb54eabcf48ba21c57ae6b129b32490df1`。
状态：设计与实施任务，不是已经实现或已经通过测试的代码。
范围：先实现 World 到 Scene 的可靠增量更新，再以最小绘制验证。所有代码仍在 `radrayruntime`。

本计划替代旧研究文档中的“通用增量数据库、不可变页、每 flight 完整 CPU Scene 快照、发布日志追赶”实现方向。保留的思想只有：只处理发生变化的数据；不同功能自己决定更新粒度；不能混淆 CPU 更新与 GPU 执行结果。

## 1. 固定的设计决定

1. World/Component 只在 GT 上修改；只登记失效，不在 setter 中编译绘制数据。
2. 一份持久 `Scene` 由渲染侧拥有，按发布顺序应用增量。单线程模式执行相同阶段。
3. 每个 flight 保存增量包、视图输入、GT 持有的本帧资产引用、必要的 GPU 反馈，不保存完整 CPU Scene 副本。
4. Scene 在 Update 阶段可写，Prepare/Record 阶段只读；本帧全部 CPU 读取任务结束后才能应用下一帧更新。
5. `Scene::Apply` 不读取 World/Component，不 acquire 窗口，不录制 GPU 命令。
6. 普通变换走局部更新；复杂低频结构变化可重建对应 proxy 的类型专属数据，保留逻辑 ID。
7. 第一版只接入已有 StaticMesh 与 Light。Camera 先生成当帧 ViewRequest，不建设相机历史系统。
8. 不引入 RenderGraph、ECS、通用依赖图、SourceId 投影层、通用事务、自动反射、MPSC 队列和新任务调度器。
9. 不重写 Application/GpuSystem/WindowManager/AssetManager；Application/runner 只增加必要的固定生命周期接线。

UE 的参考范围是持久渲染表示、组件分类标脏、帧末更新和类型专属发送接口，不是照搬 UE 整套 renderer。[E1][E2]

## 2. 当前代码与接点

### 2.1 Application / runner

`Application::Update` 当前执行 AssetManager::Pump、ApplicationScheduler::Pump、OnUpdate、World::Tick，然后返回。新收集阶段应接在 World::Tick 后。[R1]

ThreadedRunner 在 `discard` 时完全跳过 `_app->Render(frameCtx)`。因此消费 Scene 增量不能只放在 Application::Render/OnRender 内。必须在两种 runner 的固定帧序中、跳帧判断外调用。[R1]

flight 的复用仍依赖现有 semaphore 与 GPU retirement；不提前归还 writable slot，不把 CPU 读完当成 GPU 完成。[R1][R7]

### 2.2 World / Component

已有 `PrimitiveComponent : SceneComponent`，不再新建平行 PrimitiveComponent。`LightComponent` 直接继承 SceneComponent，不强迫 Light 变成 drawable primitive。[R3][R4]

Actor 的注册/注销路径集中；中央注销路径必须保证移除待更新队列中的组件指针，再允许组件销毁。不能只靠每个派生 OnUnregister 自觉调用基类。[R2]

StaticMeshComponent::SetStaticMesh 当前只有比较和赋值，新增标脏。SceneComponent 当前递归通知子树，GetWorldMatrix 递归计算父链；本计划会单独测量这项成本，不把它归零的效果说成已经实现。[R5][R6]

### 2.3 资产与真实绘制的限制

StreamingAssetRef 的拷贝、移动、析构、状态查询和 Get 都限制在 AssetManager 所属线程。资产内部不可变数据可以在明确保活条件下被借用，但 ref 本身不交给 RT。[R8]

StaticMesh 已拥有 CPU mesh、sections、bounds 与 GpuMesh；GetRenderMesh 返回值只在该资产存活时稳定。当前内置 mesh GPU 上传加载路径尚未恢复，真实测试应先由 fixture 提供构造完整的 StaticMesh，再单独恢复 importer；不要误以为补上 Scene 就能直接加载并绘制模型。[R9]

## 3. 新增对象控制在最小范围

### 3.1 公共调度状态

在 ActorComponent 内增加少量仅 GT 使用的调度状态：dirty flags、待更新数组中的索引。只有主动参与渲染同步的组件使用它。

公共入口建议：

```cpp
// 拟议接口，非可直接编译的补丁。
void MarkRenderStateDirty();      // 创建或结构变化
void MarkRenderTransformDirty();  // 变换
void MarkRenderDynamicDataDirty();// 小型类型专属参数
```

ActorComponent 提供默认空的收集/渲染生命周期扩展点。PrimitiveComponent 与 LightComponent 管理自己的渲染身份；StaticMeshComponent 实现 mesh 专属收集。通过 Actor 的中央注册/注销流程调用必要的渲染生命周期函数，避免注销安全依赖派生类调用基类。

不为每种未来能力预留 flag。实例、蒙皮、粒子真正实现时，再在各功能内部加入自己的变化集合。

### 3.2 World 的待更新集合

使用 `vector<ActorComponent*>`，每个已排队组件记录 QueueIndex。第一次标脏入队，后续只 OR flags。注销采用 swap-remove，更新被移动组件的 QueueIndex。

这些指针只在 GT 上使用。它们不进入增量包，也不跨越组件注销。

已发送创建的对象在注销时追加仅含渲染 ID 的删除项；未发送创建就注销，直接取消，不产生 Scene 对象。

### 3.3 渲染身份

PrimitiveId / LightId 使用类型化的 `{Index, Generation}`。GT 分配身份，RT 校验后在自己的存储中创建/更新/删除。同一个对象的结构重建不改变 ID。

这不是另加 SourceId 注册层；组件直接保存自己对应的渲染 ID。初版仅一 World 对一 Scene；将来支持多个 World 时需加 Scene 路由，不把裸 Index 当全局身份。

### 3.4 增量包

采用具名类型化数组，示例：

```cpp
struct SceneUpdateBatch {
    uint64_t UpdateSequence;
    vector<PrimitiveId> RemovePrimitives;
    vector<LightId> RemoveLights;
    vector<StaticMeshStateUpdate> MeshStates; // create / replace，含最终变换
    vector<PrimitiveTransformUpdate> Transforms;
    vector<LightStateUpdate> Lights;          // 小型 light 参数，含最终变换
};
```

所有名称均为拟议名称。数组种类随着真实 feature 增加，不建设字符串注册表或通用消息解析器。增加一种类型时，只增加它的 payload/收集/Apply 和 batch 入口，不改已有类型算法。

包内不得包含组件指针、读取 World 的 lambda、指向 World 可变容器的 span。可以包含拥有数据的块，以及由本帧 GT 引用保活的不可变资产渲染视图。后者必须显式写清借用契约。

普通 transform 是固定大小数据，不为它分配一个 command 对象或 std::function。数组预热后复用容量。

### 3.5 Scene 与 proxy

`RenderSystem` 拥有一份 Scene。Scene 中 primitive 与 light 分开存储；不要让 light 带 mesh/material 字段。

StaticMeshProxy 只拥有该实例独有的渲染状态与必要缓存。Geometry 的不可变数据优先复用现有 StaticMesh/GpuMesh，不每帧复制 mesh/sections，不为同一资产重复构建几何输入布局。

首版可用简单 slot table 保存唯一拥有的 proxy，并维护活跃索引列表，避免遍历大量历史空槽。多态调用仅出现在变化处理等必要位置；绘制热点是否进一步分为连续数组由 profile 决定。

不建立 Proxy → RenderObject → SceneRecord → DrawStore 多套含义重叠的完整数据。

## 4. 固定帧序

```text
GT 获得 writable flight
  → 处理 GPU completion / 清理该 flight 的旧引用
  → AssetManager::Pump
  → ApplicationScheduler::Pump
  → OnUpdate
  → World::Tick
  → World::FlushRenderUpdates(batch)
  → 捕获 ViewRequest
  → 保留本帧所有可能使用的唯一资产引用
  → 封口并发布 ready slot

RT 消费 ready slot
  → 处理该 flight 上一次 GPU 上传反馈
  → Scene::Apply(batch)                  必须执行
  → 若允许绘制：PrepareViews / Record    可以跳过
  → GpuSystem 提交与 retirement

GT 收到 completion
  → 只处理该 flight 的 GT 资源与反馈
  → 不修改 RT Scene / RT dirty arrays
```

建议的少量内部接点：

```cpp
RenderSystem::BeginFrameUpdateGT(flight);
RenderSystem::PrepareFrameGT(world, updateContext);
Application::ConsumeRenderUpdates(flight); // runner 无条件调用
RenderSystem::OnFlightCompletedGT(completion);
RenderSystem::AbandonUnpublishedFrameGT(flight);
```

“无条件”指每个已发布包都必须消费一次，包括窗口最小化、模态循环丢绘制帧和退出前已发布包。未发布就终止的包走 GT abandon，不虚构 GPU completion。

CPU 更新顺序号用于断言遗漏/重复；它不是每对象版本系统，也不是 CPU Scene 快照 epoch。

## 5. 收集合并规则

| 本轮情况 | 输出 |
|---|---|
| 注册后多次修改 | 一个带最终状态的 create |
| 已存在对象只移动多次 | 一个最终 transform |
| StateDirty 与 TransformDirty 同时存在 | 一个带最终变换的 state update |
| 同一个 Light 改颜色、强度并移动 | 一个小型完整 light update |
| 注册后尚未发送就注销 | 无输出 |
| 已发送创建后修改再注销 | 删除；不再读取源 |
| mesh A→B→A | 可以检查最终资源身份并消除无意义结构重建 |
| Loading→Ready | mark state，正常进入同一管道 |

`StateDirty` 仅覆盖该类型定义为“全状态”的字段；未来局部参数组不得机械覆盖其他字段。对小型 Light，整包更新本来就是选择的粒度。

第一版在 GT 收集期间禁止 World 修改。构造 payload 成功后才清 dirty；不引入通用回滚事务。资产加载失败属于显式不可绘制/空资源状态，不保留已经失去保活保障的旧资源作为隐式 fallback。真正无法继续的分配/内部构建失败，按工程错误策略终止或放弃未发布帧，不静默漏同步。

## 6. 类型更新契约

### 6.1 StaticMesh

- 创建：建立持久 proxy；引用或准备 mesh 的不可变渲染数据，建立该实例必要的 section/draw 描述。
- Transform：更新矩阵与 world bounds，标记对象 GPU 参数更新；不得解析材质、复制 mesh sections 或重建整个 proxy。
- 更换 mesh：同一 PrimitiveId 下重建类型专属几何关联；首版允许对象级重建，不追求 section diff。
- 删除：从可见候选/活跃索引等结构移除，销毁本对象 CPU 状态。共享几何不得随单个实例删除。
- 尚未 Ready：显式不可绘制；Ready 后使用组件最新 transform，不能恢复成请求加载时的位置。

普通 transform 更新不能默认忽略负 scale 导致的 winding/cull state 变化。数据实现若依赖此项，就要更新对应分类/状态，而不是宣称变换永远只有一个矩阵写入。

### 6.2 Light

复用公共 dirty 队列、批次交付和身份协议，但使用独立 LightSceneData。第一版颜色、强度、范围、角度、阴影开关与 transform 合并成一个小记录整体更新。

扩大影响半径需要更新影响范围/bounds；不影响任何 StaticMesh 几何数据。尚未实现的阴影缓存不提前造出来。

### 6.3 Camera / Material / 后续类型

Camera 本轮先生成 ViewRequest 值，不让渲染侧读取 CameraComponent，也不为了每帧视图参数建设 CameraProxy 历史系统。

Material 当前分支已移除，本轮不恢复完整材质框架。以后加入时：共享参数记录由材质拥有者更新；普通参数变化不广播“全部 mesh StateDirty”；technique/layout 的真实结构依赖由该功能显式处理。

SkinnedMesh、InstancedMesh、Particle 暂不实现，仅保留通过类型专属 batch 增加功能的扩展方式。

## 7. 资产保活：必须与增量更新分开

只把本轮 dirty 对象的资产放进 flight 引用列表是错误的。静止对象仍然会被绘制，GT 也可能在 RT 使用上一帧时销毁或改绑它。

首版采用：

- GT 按已发布的渲染绑定增量维护 `AssetId → 使用计数`；它是非拥有索引，不是第二份场景数据。
- 只在新增、改绑、删除时变更计数；每个组件保留必要的上次发送资源 ID，避免扫描全部组件重算关系。
- 每帧封口时遍历唯一被使用的资产 ID，通过现有 `AssetManager::Find` 获取本 flight 的 refs，不发起 Load。[R8]
- 用于渲染的 Ready 对象必须与 payload 的不可变渲染视图相符。第一版不支持同一 AssetId 的无通知热替换；真正支持热替换时增加资源代次与显式 rebind。
- 资产的使用者自身继续持有引用。计数索引非零但 Find 失败是契约错误，不允许仍使用旧指针。
- 本帧所有 refs 在 GT 获取，在 flight 完成后于 GT 释放。RT 不拷贝、不移动、不析构 StreamingAssetRef。
- Scene 中长期保存的借用视图只能在正确的 Apply 边界之后、且被当前帧保活覆盖时读取；remove/rebind 的 CPU 清理不得解引用已退休的旧资产。

这一步具有 O(U) 的每帧保活成本，U 是唯一使用资产数，必须独立统计。零变化只承诺不提取/编译场景，不声称整帧完全零遍历。

Ready 通知复用 `co_await ref` 或现有 GT 加载完成路径。组件的等待任务必须可取消，注销先取消；回调校验仍是同一组件注册身份和同一请求。不要调用共享 ref.Cancel() 去取消其他对象也在使用的加载。测试必须覆盖 Ready 与销毁/换 mesh 的交错。[R8]

## 8. GPU 局部更新，不恢复 CPU Scene 多副本

第一版可用每 flight 独立的持久 object buffer。Scene 只保存一份 CPU 参数；每个物理 GPU buffer 独立保存待更新记录 ID。

当对象参数改变，向对应 flight 的 pending key 集合登记。默认 flight 很少，O(F×D) 的登记成本可明确计量；后续需要再优化，不全场景扫描版本。

上传状态建议保持简单：

```text
PendingKeys：尚未录制的更新
AttemptKeys：本次确实录制上传的 key
```

录制时，把实际覆盖的 key 从 Pending 转入 Attempt；以后发生的新变化仍进入 Pending。GPU 完成反馈成功时，只丢弃匹配提交的 Attempt，不清除新 Pending。失败/应用命令被丢弃时，把 Attempt 合并回 Pending。重试读取 Scene 当前值，不重放已释放的旧 payload。

要匹配 FrameSerial 与 BufferGeneration。buffer 扩容/更换时全量初始化该新物理 buffer 是合法结构更新；不能把它记作正常静止帧的增量行为。[R7]

所有 GPU dirty 与 Attempt 集合由 RT 操作。GT completion 只把反馈放进该 flight 的交接字段；RT 在下一次安全消费时处理。禁止 GT 回调直接清 RT Scene 里的脏集合。

本轮只补最小对象参数路径与上传丢弃测试；不建设统一 GPUScene、bindless 或 GPU-driven renderer。

## 9. 七个实施提交

### M0 — 固定帧交付与最小观察量

改动：application.h/.cpp、render_system.h/.cpp；新增 scene_update.h 和空 Scene。

建立 per-flight payload 和单份 Scene。PrepareFrameGT 放在 World::Tick 后；ConsumeRenderUpdates 放在两种 runner 的绘制跳过判断外；补 unpublished abandon 和 shutdown 清理。

先用测试手工写入一个计数值，不接 mesh/GPU。记录 Produced/ConsumedSequence、AppliedBatchCount。

验收：F=1/2/3 均按序消费每个已发布包一次；强制连续跳过绘制仍保持 Scene 最终值；未发布退出不等待不存在的 completion；不更改 GpuSystem fence/slot 语义。

### M1 — 组件标脏、合并与生命周期

改动：actor_component、actor、world、primitive_component；新增 test_scene_updates.cpp。

实现 dirty+QueueIndex、GT vector 队列、O(1) swap-remove、渲染身份、create/remove 归并。Actor 中央注销路径保证先断队列再销毁。

验收：同组件修改 100 次只入队/收集一次；销毁排队组件无 UAF；创建后立即销毁无 Scene 对象；已发送创建后立刻删除能跨帧按序处理；结构更新不更换 ID。

### M2 — StaticMesh 的 CPU 持久数据路径

改动：static_mesh_component；新增 scene.h/.cpp、static_mesh_proxy.h/.cpp、test_static_mesh_scene.cpp。

先使用不引用真实 GPU 资源的 fixture：Create/Transform/Replace/Remove 四条路径。包包含最终变换，Scene 按类型更新；同一批 mesh+transform 使用两者最终值更新 bounds。

验收：一个对象动、其他静止，只有这个对象的 transform/bounds 更新；移动不重建 mesh 描述；多个 View 不触发额外 Scene Apply 或 mesh rebuild；负 scale 与新 local bounds 正确。

此提交不得宣称真实资产保活已完成。

### M3 — 真实资产与 flight 保活

改动：RenderSystem 的 GT frame refs、StaticMesh 收集、Ready 等待与注销；新增 test_scene_assets.cpp。

复用现有 AssetManager::Find/co_await，完成非拥有唯一资产使用索引、本 flight refs 和清理。此阶段先用带析构计数、线程断言的测试资产验证 CPU 借用生命周期；M5 再用现有上传设施构造完整 StaticMesh/GpuMesh。

验收：静止对象在多个 flight 中安全绘制/读取；GT 删组件或换资产不让旧 flight 的资源提前析构；Ready 之后使用最终 transform；同 asset 多对象去重保活；退出和加载取消均无泄漏、无 RT ref 操作。

### M4 — Light 作为第二种更新粒度

改动：现有 Light/Point/Spot 等 setter、LightSceneData、scene_update、test_light_scene.cpp。

采用小型完整 light 参数更新；复用调度，不复制 mesh 同步逻辑，不建设统一万能 PrimitiveData。

验收：100 次颜色/强度/变换修改合成一个 light update；影响范围正确；StaticMeshRebuildCount 不变；现有 spot_light 数值测试继续通过。

### M5 — 最小真实绘制与 GPU 参数局部更新

改动：对象 GPU 参数存储、上传反馈；测试 fixture 或一个极小示例使用既有 OnRender/RHI。新增 test_scene_gpu.cpp，复用现有测试设备工具。

用现有上传设施创建一个三角形或立方体的完整 StaticMesh/GpuMesh，并通过 AddReady 交给 AssetManager。固定 shader、一到三个 view 即可，不把恢复模型 importer 作为前置依赖。静态几何/PSO 准备不在每个 View 重做。没有现存 Forward 可供直接修改，不为了验证创建完整 Forward 框架或恢复 RenderGraph。[R10]

验收：各 flight GPU readback 与 Scene 对象参数一致；只移动一次后轮换不回退；丢弃含上传的绘制帧后重试正确；旧 GPU buffer 在最后使用完成前不释放。

### M6 — 性能门槛、回归与文档

新增 scene benchmark 与 counters，更新 docs/architecture/render-framework.md、frame-and-gpu.md、runtime tests CMake。

使用独立的全量构建参考实现作为测试 oracle；只在测试中执行。默认生产路径关闭昂贵全量校验。身份有效性、基本边界和所有权保证不是可选的安全措施，不通过关闭它们换取表面性能。

为普通 setter 加精确相等短路，确保一个 flush 每个 dirty primitive 最多采集一次最终矩阵。暂不改变 OnTransformChanged 的游戏语义。单独计量递归通知/父链矩阵成本；如果它成为瓶颈，再以专门测试驱动变换缓存与失效合并，不把这部分成本藏进“队列已经去重”。

验收：满足后面的操作次数断言；报告 GT、RT、保活、GPU 四类成本；所有旧 runtime 测试继续通过；没有新增通用数据库/依赖图。

## 10. 详细验收用例

下表为需要实现的测试，不是本次已经运行的结果。

| ID | Setup | Action | Assertions |
|---|---|---|---|
| T01 | 一个已发布 primitive | 同帧设置位置100次 | 入队1、收集1、最终transform正确、结构重建0 |
| T02 | 未注册 primitive | 设置属性后注册，再改属性 | 只create1次，使用最终属性 |
| T03 | 注册但未flush | 注销/销毁 | 无create/remove、无悬空队列项 |
| T04 | create已封入上一批、RT未消费 | GT删除源并发布下一批 | RT先create后remove，最终不存在 |
| T05 | 三个组件均在dirty队列 | 删除中间组件 | swap-remove后另一组件QueueIndex正确，剩余两项更新一次 |
| T06 | 已存在primitive | 同帧替换mesh并移动 | 一个state包，bounds使用新mesh和新矩阵 |
| T07 | 旧id已删除 | 复用index的新generation，再注入旧更新 | 新对象不被污染；诊断旧更新 |
| T08 | Scene一份、F=1/2/3 | 只移动一次，再连续消费空包 | 不回退，CPU更新一次 |
| T09 | 连续三个已发布包 | 丢弃中间帧绘制 | 三个包均应用一次；最终Scene正确 |
| T10 | 封口但未发布的frame | 立即退出 | refs在GT释放，不虚构completion |
| T11 | RT持有旧frame，GT仍可更新下一帧 | GT移除最后一个组件引用 | 旧frame引用保活到完成 |
| T12 | 静止mesh已经Ready | 多帧无dirty | 仍有本frame资产保护；Scene rebuild为0 |
| T13 | 一千实例共用同一mesh | 封口一帧 | 唯一资产保活一次；不复制一千份mesh资产数据 |
| T14 | 资产Loading，组件移动 | 加载转Ready | 使用最新位置；无需全场景轮询 |
| T15 | 等待旧资产Ready | 换mesh或删除组件，再让旧请求结束 | 不复活旧绑定、无UAF、不取消其他使用者加载 |
| T16 | point/spot light已存在 | 改参数并移动100次 | 一个light更新；范围/方向正确；mesh重建0 |
| T17 | 普通层级、父子节点 | 父节点移动，子节点也移动 | 最终世界矩阵正确，渲染收集不重复 |
| T18 | 层级中节点已dirty | 重挂接、删除父节点、读取矩阵 | 游戏侧读取最新值，通知/同步无遗漏 |
| T19 | primitive正scale | 切换负scale | bounds与真实依赖的winding/cull分类正确 |
| T20 | 对象GPU镜像已同步 | 只改1个对象后轮换F槽 | 每物理buffer最终正确；CPU几何不重建 |
| T21 | 上传已录制 | 提交丢弃应用命令 | Attempt回到Pending，下一有效帧readback正确 |
| T22 | old Attempt在途，RT又收到新值 | 旧Attempt成功或失败 | 新Pending不被错误清除，重试使用最新值 |
| T23 | GPU参数buffer容量不足 | 扩容并继续更新 | 新buffer完整初始化，旧资源延迟回收 |
| T24 | 一个Scene | 同帧1个view与3个view | SceneApply次数相同，结构构建次数相同；视图工作允许增加 |
| T25 | 随机create/update/remove/replace | 按序增量应用并与全量参考比较 | 每个发布边界渲染语义相同；测试不依赖GPU |
| T26 | 加载、已发布frame、未发布frame同时存在 | 关停 | 无等待死锁，无资产线程违规，所有CPU/GPU owner安全释放 |

## 11. 性能验收

### 11.1 计数器

```text
GT:
  DirtyMarkCalls
  UniqueDirtyComponents
  GatheredComponents
  CapturedBytes
  CaptureAllocations
  UniquePinnedAssets
  AssetPinTime
  TransformNotifyVisits / WorldMatrixEvaluations
RT:
  AppliedBatches
  AppliedTransforms
  MeshStateBuilds
  LightUpdates
  SceneApplyTime
GPU:
  ObjectUploadBytes / Ranges
  UploadRetryCount
  ObjectBufferReallocations
```

### 11.2 工作负载

N=10,000/100,000；D=0/1/1%/100%；View=1/3；Flight=1/2/3；另有深层父子结构、共享mesh和大量唯一资源两类测试。

固定随机种子与硬件/编译设置；Release分别记录校验开/关。预热完成后采样，报告中位数和P95，以及实际执行次数/分配/字节。测试 oracle 的耗时不得混入生产路径测量。

### 11.3 硬断言与不作的承诺

- 已加载且无时间驱动数据的稳定帧：GatheredComponents=0，MeshStateBuilds=0，LightUpdates=0。
- 单个普通transform更新：GatheredComponents=1、AppliedTransforms=1，MeshStateBuilds=0。
- 一个对象一帧设置100次：该对象Gather=1；dirty标记调用可为100，不伪装为没有调用。
- 1 view→3 views：不增加SceneApply和类型结构重建。
- scene规模增加、dirty数固定：变化收集不新增全场景遍历；随机存储访问和保活开销允许反映真实成本。
- 预留容量后，只有少量固定大小transform更新不发生逐对象堆分配。
- D=N时比较GT+RT总成本与全量参考，不只报告GT变快。若细分管理开销成为负担，允许该类型使用批量连续全量更新路径。
- 不预先承诺毫秒数或加速倍数。本次没有执行C++基准。

## 12. 文件清单

已有文件修改：

```text
modules/runtime/include/radray/runtime/application.h
modules/runtime/src/application.cpp
modules/runtime/include/radray/runtime/render_system.h
modules/runtime/src/render_system.cpp
modules/runtime/include/radray/runtime/game_framework/world.h
modules/runtime/src/game_framework/world.cpp
modules/runtime/src/game_framework/actor.cpp
modules/runtime/include/radray/runtime/components/actor_component.h
modules/runtime/include/radray/runtime/components/primitive_component.h
modules/runtime/src/components/primitive_component.cpp
modules/runtime/include/radray/runtime/components/static_mesh_component.h
modules/runtime/src/components/static_mesh_component.cpp
modules/runtime/include/radray/runtime/components/light_component.h
modules/runtime/src/components/light_component.cpp
modules/runtime/src/components/point_light_component.cpp
modules/runtime/src/components/spot_light_component.cpp
modules/runtime/src/components/scene_component.cpp
modules/runtime/tests/CMakeLists.txt
```

新增位置建议（具体文件名在实现时可调整）：

```text
modules/runtime/include/radray/runtime/scene.h
modules/runtime/include/radray/runtime/scene_update.h
modules/runtime/src/scene.cpp
modules/runtime/src/static_mesh_proxy.h
modules/runtime/src/static_mesh_proxy.cpp
modules/runtime/src/scene_gpu_data.h
modules/runtime/src/scene_gpu_data.cpp
modules/runtime/tests/test_scene_updates.cpp
modules/runtime/tests/test_static_mesh_scene.cpp
modules/runtime/tests/test_scene_assets.cpp
modules/runtime/tests/test_light_scene.cpp
modules/runtime/tests/test_scene_gpu.cpp
modules/runtime/tests/bench_scene_updates.cpp
```

Scene内部proxy与GPU镜像细节先放src，不把所有内部类型提升成公共API。typed payload的头文件按需要拆分，避免任何渲染功能都必须包含完整World/Component定义。

当前runtime的tests已通过CMake登记；新增目标沿用 `radray_add_test`、既有GPU测试工具和 `radray_runtime_tests`，不要另造测试运行器。[R11]

## 13. 最终完成标准

本轮完成后，应能回答并自动验证：

“哪个组件变了、只发送了什么、RT修改了什么、为什么没有重建无关数据、某个flight为什么还必须保活某个资产、丢弃绘制后下一帧为什么仍然正确。”

静止场景的保活、视图选择和命令录制仍可能有成本；本轮消除的是无变化时的重复提取与重复结构构建。

先完成 M0–M3 再扩展渲染功能。不要为未知未来类型补齐空接口，也不要通过重新恢复整份每flight CPU Scene来回避所有权问题。

## 参考来源

以下仓库链接固定到审查提交；UE链接是本次读取的Epic官方文档，不代表逐行审核UE源码。

- [R1 Application与runner](https://github.com/ksgfk/RadRay/blob/da7731eb54eabcf48ba21c57ae6b129b32490df1/modules/runtime/src/application.cpp)
- [R2 Actor组件生命周期](https://github.com/ksgfk/RadRay/blob/da7731eb54eabcf48ba21c57ae6b129b32490df1/modules/runtime/src/game_framework/actor.cpp)
- [R3 PrimitiveComponent](https://github.com/ksgfk/RadRay/blob/da7731eb54eabcf48ba21c57ae6b129b32490df1/modules/runtime/include/radray/runtime/components/primitive_component.h)
- [R4 LightComponent](https://github.com/ksgfk/RadRay/blob/da7731eb54eabcf48ba21c57ae6b129b32490df1/modules/runtime/include/radray/runtime/components/light_component.h)
- [R5 StaticMeshComponent setter](https://github.com/ksgfk/RadRay/blob/da7731eb54eabcf48ba21c57ae6b129b32490df1/modules/runtime/src/components/static_mesh_component.cpp)
- [R6 SceneComponent变换实现](https://github.com/ksgfk/RadRay/blob/da7731eb54eabcf48ba21c57ae6b129b32490df1/modules/runtime/src/components/scene_component.cpp)
- [R7 GpuSystem与FlightCompletion](https://github.com/ksgfk/RadRay/blob/da7731eb54eabcf48ba21c57ae6b129b32490df1/modules/runtime/include/radray/runtime/gpu_system.h)
- [R8 AssetManager与引用契约](https://github.com/ksgfk/RadRay/blob/da7731eb54eabcf48ba21c57ae6b129b32490df1/modules/runtime/include/radray/runtime/asset_manager.h)
- [R9 StaticMesh资产](https://github.com/ksgfk/RadRay/blob/da7731eb54eabcf48ba21c57ae6b129b32490df1/modules/runtime/include/radray/runtime/static_mesh.h)
- [R10 当前render framework边界](https://github.com/ksgfk/RadRay/blob/da7731eb54eabcf48ba21c57ae6b129b32490df1/docs/architecture/render-framework.md)
- [R11 Runtime测试登记](https://github.com/ksgfk/RadRay/blob/da7731eb54eabcf48ba21c57ae6b129b32490df1/modules/runtime/tests/CMakeLists.txt)
- [E1 UActorComponent渲染更新接口](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UActorComponent)
- [E2 UE Threaded Rendering](https://dev.epicgames.com/documentation/en-us/unreal-engine/threaded-rendering-in-unreal-engine)
- [E3 UE Mesh Drawing Pipeline](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine)
