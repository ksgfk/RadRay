> - 适用: 改渲染管线、场景表示、Application 生命周期或服务装配
> - 权威: 本文描述场景、Forward 与 Application 装配；workload/graph/history 契约见 `renderer-foundation.md`，资产与 GPU 帧管理见 `asset-system.md`、`frame-and-gpu.md`
> - 锚点: `modules/runtime/include/radray/runtime/render_framework/render_pipeline.h`, `modules/runtime/include/radray/runtime/forward_pipeline/forward_pipeline.h`, `modules/runtime/include/radray/runtime/material.h`, `modules/runtime/include/radray/runtime/shader_program.h`, `modules/runtime/include/radray/runtime/material_technique.h`, `modules/runtime/include/radray/runtime/render_framework/render_scene_snapshot.h`, `modules/runtime/include/radray/runtime/render_framework/renderer_list.h`, `modules/runtime/include/radray/runtime/components/static_mesh_component.h`, `modules/runtime/include/radray/runtime/game_framework/actor.h`, `modules/runtime/include/radray/runtime/service_registry.h`, `modules/runtime/src/application.cpp`, `modules/runtime/src/render_system.cpp`, `examples/example_lambert_sphere/example_lambert_sphere.cpp`

# 渲染框架与 game framework

**先读这条**：runtime 提供内置 `ForwardPipeline`，但不会把它设成隐式默认值。
`RenderSystem::_pipeline` 初始为 null，由应用在 `OnInit` 中通过
`RenderSystem::SetPipeline(unique_ptr<RenderPipeline>)` 显式注入并转移所有权。
`examples/example_lambert_sphere` 用 `AssetManager`、`StaticMeshComponent`、`Material` 和内置
forward pipeline 验证完整的 scene → proxy → draw 路径。

## 分层

```
Application            进程生命周期、runner 选择、帧循环
  ├─ WindowManager     窗口与 swapchain
  ├─ GpuSystem         device、queue、flight、上传        → frame-and-gpu.md
  ├─ RenderSystem      workload/output、graph pools/history、registry 与 pipeline
  ├─ AssetDatabase     可选 JSON 身份库与 importer              → asset-database.md
  ├─ AssetManager      资产生命周期                       → asset-system.md
  └─ World             Actor / Component / Scene
```

`RenderSystem` 拥有"怎么画"，**不拥有帧时序**——那是 `GpuSystem` 与 runner 的事。

## 渲染管线

```text
Game thread:   flight 可写 → 清上一帧 retained refs → AssetManager::Pump
               → ApplicationScheduler::Pump → OnUpdate → World::Tick → PrepareFrame
Render thread: pool/history safe Begin → resolve requested outputs/views → pipeline.Render
               → compile/realize/execute graph → 未写目标 fallback clear → required final states
```

`RenderPipeline` 只提供 `PrepareFrame` 与 `Render` 两个入口。前者在 game thread 写当前 flight 的
pipeline 私有输入，后者在 render thread 消费该输入。runner 既有的 slot semaphore / fence 保证
flight 复用互斥，不增加 packet、sequence 或另一套同步协议。
`RenderPrepareContext` 提供 output 值目录和 workload builder；pipeline 向当前 flight 的 frame plan
写入 view families。`RenderPipelineContext` 提供 resolved families、graph/output/history 操作，
不公开 AppFrameContext、窗口或 command buffer。具体接口与验证见
[Renderer foundation](renderer-foundation.md)。

`RenderSystem` 只取得 plan 请求的目标，graph 按真实初始状态导入；成功写入标记由 executor 产生。
host 对未写 output clear 后转换到各自 required final state。没有 pipeline 时默认请求 active
presentation outputs 并 fallback clear；没有 presentation target 时仍执行自定义 pipeline。
Application 不提供独立的 view 内容录制钩子。

`SetPipeline` 是应用装配入口，应在 runner 启动前或 GPU idle 后调用；当前没有运行时替换协议。
`ForwardPipeline` 在构造时借用 Scene 与 Camera，只在 `PrepareFrame` 访问它们，因此这些 source
必须活过最后一次 PrepareFrame。已准备的帧不依赖 source、proxy 或 Material 的后续寿命。

`MaterialTechnique` 是 game-thread 创建的不可变 pass 表，包含唯一 pass 名、program、material cbuffer
anchor 和默认固定功能状态。`Material::Create(technique)` 借用 technique，后者及其 programs 必须活过
Material 的 authoring/snapshot 调用；program 还必须活过帧执行。primary pass 定义 canonical 数值布局
与资源声明，Material 保存一份数值/纹理/sampler 值及逐 pass 的状态覆盖。布局兼容、资源子集与
metadata 的校验限制见 [Renderer foundation](renderer-foundation.md#材质-technique)。

Material setter 接受 primary cbuffer 内的相对字段路径（如 `BaseColor`）或带 primary declaration 的
完整路径（如 `ForwardMaterial.BaseColor`），拒绝其他 cbuffer。Texture/Sampler 使用声明的 exact name。
`BuildRenderData` 为各 pass 复制独立的数值 bytes、状态和其实际消费的资源；缺资源只使消费它的 pass
无效。资产 owner 追加到宿主 retained vector；snapshot 不创建 RHI set / SRV，也不维护 descriptor 版本。

`ShaderProgram` 继续拥有 artifact、layout、shader、参数索引与 PSO map。PSO key 由 material state、
geometry vertex layout/topology、pass attachment formats/sample count 组成；不包含 render pass 指针、
Load/Store 或 framebuffer 尺寸。ShaderJit、artifact/program cache 和
失败 cache 继续由 RenderSystem 拥有，所有 program 活到 GPU idle 后的 shutdown。

### shader artifact 边界

RHI 只消费 render 层已经验证过的 shader、layout 和 PSO 描述，仍不依赖 compiler client。
开发期源码入口在 `RenderSystem::GetOrCreateShaderProgram(const ShaderProgramRequest&)`：request 显式
拥有逻辑 `SourceName`、结构化 defines、keyword assignments、完整 `CompilePolicy` 与按 target 分开的
layout recipe，discovery 与 compile 由同一个 request 驱动。缓存分两层：compiler artifact 按
source/defines/assignments/policy/target/toolchain 缓存（不含 layout recipe），program 按 artifact 身份
加当前 backend 的 canonical resolved layout hash 缓存。按实际 backend 只编译一个 target；失败按完整
key 记成显式失败记录以避免逐帧重试，同时不会污染其他 key。
`RADRAY_ENABLE_SHADER_JIT=OFF` 时 `RenderSystem` 与 pipeline 仍可构造，program 请求明确返回空。

## 场景表示

数据从 game framework 流到渲染侧的路径：

```
World 构造          → RenderSystem::AllocateScene()
SpawnActor          → RegisterAllComponents → ActorComponent::OnRegister
PrimitiveComponent  → CreateRenderState → Scene::AddPrimitive(CreateSceneProxy())
LightComponent      → CreateRenderState → Scene::AddLight(CreateSceneProxy())
```

**Scene 与 proxy 只在 game thread 使用。proxy 常驻，pipeline 输入每帧复制。** proxy 在组件 `OnRegister` 时创建，
存在 `Scene` 的 `vector<unique_ptr<...>>` 里，`OnUnregister` 时移除。

**任何属性或变换变化走 `MarkRenderStateDirty()` → 整棵 proxy 销毁重建。**
`OnTransformChanged` 也走这条路。Light proxy 的参数在构造函数里一次性从 component 快照。
这个粒度很粗，但它让"proxy 里的数据什么时候会变"有一个确定答案：只在重建时。

`Actor::FindComponent<T>()` 按拥有顺序对实际组件做指针形式 `dynamic_cast`，返回第一个可转换
对象的 `Nullable`；查询目标可以是任意完整类类型，因此支持组件基类、接口、横向转换和虚继承。
const Actor 只返回 const 视图。移除组件时也以 `dynamic_cast<SceneComponent*>` 判断是否需要先从
父节点脱离，不维护平行的场景组件标志或 GUID 查询重载。

### draw call

```cpp
struct MeshDrawArgs {
    const GpuMesh::DrawData* Geometry;  // VB/IB、vertex layout、topology
    uint32_t FirstIndex, IndexCount, VertexOffset;
};
```

**覆写 `GetDrawArgs` 的 proxy 必须自持一份 `StreamingAssetRef<StaticMesh>`。**
`Geometry` 是指向资产内部的裸指针，保命责任在 proxy 自己
并覆写 `CollectAssetReferences`，让 PrepareFrame 复制几何 owner 到宿主 retained refs。

`StaticMeshComponent` 按 section 保存 material，`StaticMeshSceneProxy` 自持 mesh ref、借用的 material 指针与
local-to-world，并把 `StaticMeshSection` 的 `FirstIndex` / `IndexCount` / `VertexOffset` 投影成 draw。
每个 flight 在 PrepareFrame 中构建一次与 view 无关的 `RenderSceneSnapshot`：primitive 保存变换、
世界 AABB、layer mask 和连续 MeshBatch 范围；batch 借用 geometry 并保存 section draw range、primitive
和 material 索引。材质按首次出现去重，所有 pass 的 program 分配帧内整数 ID。光源保存参数和球形界限。
`StaticMeshSceneProxy` 从 mesh asset 提供局部 bounds；自定义 proxy 可以覆盖 layer mask 与禁用视锥剔除标志。
无效 bounds 保守可见；几何和纹理由宿主 per-flight refs 保活。

`GpuMesh::DrawData::VertexBuffers` 保存真实 binding number 与 buffer view，可描述多个顶点流。
现有 mesh uploader 仍生成一个交错流；手工几何可以使用多个或不连续的 binding。PSO resolver 从几何中
选取当前 shader 所需的属性及其 buffer，允许 DepthOnly 复用 Forward 的完整几何；缺失或重复匹配的
必需语义会失败。执行器按连续 binding 段调用底层接口，不重新编号。

### 内置 ForwardPipeline

Forward 在 render thread 对每个 resolved view 调用一次 CPU `Cull`，从同一结果生成 DepthOnly、
Opaque、Transparent 三个 `RendererList`。通用 builder 处理 pass/queue/mask、排序与统计；具体
`ForwardLitMeshPassProcessor` / `DepthOnlyMeshPassProcessor` 解释 shader 契约，准备 per-view/object/material
bytes 与 frame-local sets，输出只借用资源的 `MeshDrawCommand`。render thread 不访问 Scene、proxy、
CameraComponent、Material、AssetManager 或 StreamingAssetRef。

Forward resolver 按 `ForwardView`、`ForwardMaterial`、`ForwardObject` 找到当前 target 的真实 group，
要求三个 cbuffer dynamic 且组号不同；active `AlbedoTexture` / `LinearSampler` 必须属于 material group。
DepthOnly 只接受独立的 dynamic view/object 组，不绑定 material。DXIL spaces 与 SPIR-V sets 可以不同，
CPU 不硬编码 0/1/2。resolver 失败按 program 负缓存；不替换成其他 pass。

每个 flight 的 `FrameDrawResources` 持有 arena 与不可变 sets；renderer lists 必须先清空，才能清 sets
和重置 arena。graph callback 只查询 PSO、绑定已准备的资源并 draw。snapshot/culling/list/执行统计和
精确的资源寿命契约见 [Renderer foundation](renderer-foundation.md#场景快照与剔除)。

Forward 在同一张 graph 中声明可选深度预通道、opaque 和必要的 transparent。一个 family 可以包含
多个不重叠 view，独立保存剔除结果、排序、光照和 offsets，共享 family attachments。view/scissor
经 `MakeViewport` 统一处理 Vulkan Y 翻转；点光按各 view 的距离截断。目标、深度 Load/Store、
视图提交和当前范围见 [Renderer foundation](renderer-foundation.md#forward-范围)。

## Application 与 runner

**启动**：`Run(desc)` = `InitializeRuntime` → `OnInit` → `StartLoop`。

`InitializeRuntime` 创建实例后执行静态服务装配：

1. 创建 5 个核心服务（`WindowManager`、`GpuSystem`、`RenderSystem`、`AssetManager`、`World`）；
   `desc.AssetRoot` 非空时另开可选 `AssetDatabase`。
2. 用固定的 `ServiceRegistry<...>` 集合借用这些实例；数据库占据 `OptionalService<AssetDatabase>` 槽位。
3. `Initialize()` 先展开所有 trait 的注入，再按编译期顺序初始化。`RenderSystem` 的 trait
   显式调用 `OnInitialize` 创建 `RenderPassRegistry`，`AssetManager` 自动取得可选的 `IAssetSource`。

成功后建主窗口并挂 swapchain。trait 初始化失败会先回滚，再由 `Application` 回收对象并让
`Run` 返回 1，不进入游戏钩子和 runner。实例创建仍在 registry 之前：例如 `GpuSystem` 构造中的
设备创建不属于 registry 初始化事务。

正常 runner 走 `Shutdown`；若初始化或游戏钩子的异常绕过正常路径，`Application` 析构仍调用同一
幂等内部 teardown（不再调用游戏侧虚钩子），先断开窗口的非 owning 引用，再按下述服务顺序回收。

**关停顺序是固定的**，理由见 [asset-system](asset-system.md)：

```
WaitAndCleanupCompletedFlights → OnShutdown → scheduler.CancelAll
  → World（先拆 proxy 与 AssetRef）
  → WindowManager::SetRenderSystem(nullptr) → RenderSystem
  → AssetManager（force-unload）
  → AssetDatabase
  → DetachAllSwapChains / SetGpuSystem(nullptr) → GpuSystem → WindowManager
```

### 两个 runner

`desc.Multithreaded` 二选一。

**`SingleThreadRunner`** — 一个循环里顺序做完：

```
DispatchEvents → BeginUpdateForFlight → PumpFrameUploadScheduler → Update
              → BeginFrameRecord → Render → EndFrameRecordAndSubmit → AdvanceFrameIndex
```

**`ThreadedRunner`** — 主线程只做 Update 侧，渲染线程做录制与提交：

| 主线程 | 渲染线程 |
|---|---|
| `DispatchEvents` | 等 `_readySlotsSemaphore` |
| `TickFrame`（Update / World::Tick） | `BeginFrameRecord` → `Render` → `EndFrameRecordAndSubmit` |
| `AdvanceFrameIndex` + release 一个 ready 槽 | `CompleteFlightIfReady` / `RetireRenderedFrames` |

同步用 `_writableSlotsSemaphore`（容量 = flight 数）、原子的 `_renderedFrameCount`
（notify / wait）和 `_retireMutex`。

**帧边界等待的恢复必须回到主线程**：flight 完成是渲染线程观察到的，
在那里恢复就等于在渲染线程跑资产析构。引用计数与 manager 表只在 game thread 访问，
因此渲染线程只标记完成，由主线程泵恢复；机制见[帧与 GPU](frame-and-gpu.md)。

Windows 下还有 `Win32ModalLoopVBlankRenderer`：模态循环（拖窗口、菜单）期间用 DXGI
`WaitForVBlank` + 一个消息窗口驱动渲染线程补帧，避免界面冻住。

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

## 现状陷阱

- **默认 pipeline 为空**：`RenderSystem::_pipeline` 只有在应用调用 `SetPipeline` 后才接线；
  `example_lambert_sphere` 的 pipeline 注入是一个显式样例路径。
- **当前 Forward 使用 snapshot、逐 view 剔除、renderer lists 和 graph pool**；没有自动 instancing、
  shadow 或内置 post process。样例自定义计算与合成 pass 的接入不改变内置 Forward 的范围。
- **group 数字来自当前 target metadata**：具体 pipeline 按 declaration 解释职责，Material 只认识 anchor 选中的一组。
- **`PrimitiveComponent` 基类仍返回空 proxy**：可绘制路径由 `StaticMeshComponent` 的派生实现提供。
- **JIT 不是 runtime 的可用性前提**：关闭 JIT 后 program 源码请求失败；未来 AOT consumer 仍可
  直接构造 `ShaderProgram`。
- **`radray_add_radray_gtest_case` 已定义但无人使用。**
