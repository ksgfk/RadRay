> - 适用: 改渲染管线、场景表示、Application 生命周期或服务装配
> - 权威: 本文是 runtime 层「除资产系统与 GPU 帧管理之外」部分的唯一说明。那两块见 `architecture/asset-system.md` 与 `architecture/frame-and-gpu.md`
> - 锚点: `modules/runtime/include/radray/runtime/render_framework/render_pipeline.h`, `modules/runtime/include/radray/runtime/forward_pipeline/forward_pipeline.h`, `modules/runtime/include/radray/runtime/material.h`, `modules/runtime/include/radray/runtime/shader_program.h`, `modules/runtime/include/radray/runtime/render_framework/mesh_draw.h`, `modules/runtime/include/radray/runtime/components/static_mesh_component.h`, `modules/runtime/src/render_system.cpp`, `examples/example_lambert_sphere/example_lambert_sphere.cpp`

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
  ├─ RenderSystem      "怎么画"：RenderPassRegistry 与 RenderPipeline
  ├─ AssetDatabase     可选 JSON 身份库与 importer              → asset-database.md
  ├─ AssetManager      资产生命周期                       → asset-system.md
  └─ World             Actor / Component / Scene
```

`RenderSystem` 拥有"怎么画"，**不拥有帧时序**——那是 `GpuSystem` 与 runner 的事。

## 渲染管线

```text
Game thread:   flight 可写 → 清上一帧 retained refs → AssetManager::Pump
               → ApplicationScheduler::Pump → OnUpdate → World::Tick → PrepareFrame
Render thread: acquire targets → initial state → RenderTarget → pipeline.Render
               → 对未写目标 fallback clear → Present
```

`RenderPipeline` 只提供 `PrepareFrame` 与 `Render` 两个入口。前者在 game thread 写当前 flight 的
pipeline 私有输入，后者在 render thread 消费该输入。runner 既有的 slot semaphore / fence 保证
flight 复用互斥，不增加 packet、sequence 或另一套同步协议。
`RenderPipelineContext` 只有 `AppFrameContext& Frame` 与 target span；不携带 Application、Scene、
Camera 或其他渲染模型。普通函数安排具体管线内部步骤，不提供通用 camera/pass/event 阶段链。

`RenderSystem` 取得窗口目标后统一转换到 `RenderTarget`，调用 pipeline，并清除仍未写入的目标，
最后转换到 `Present`。pipeline 必须在 RenderTarget 状态使用 backbuffer，并在成功录制后设置
`ContentDrawn`。没有 pipeline 时也走 fallback clear；没有取得 presentation target 时不执行
pipeline。Application 不再提供独立的 view 内容录制钩子，自定义绘制由注入的 pipeline 完成。

`SetPipeline` 是应用装配入口，应在 runner 启动前或 GPU idle 后调用；当前没有运行时替换协议。
`ForwardPipeline` 在构造时借用 Scene 与 Camera，只在 `PrepareFrame` 访问它们，因此这些 source
必须活过最后一次 PrepareFrame。已准备的帧不依赖 source、proxy 或 Material 的后续寿命。

`Material` 是一个 concrete program 的 CPU authoring state，包含自己负责的一个 parameter group、
数值 bytes、texture refs/subview、sampler descriptor、固定功能状态和 RenderQueue。
`Material::Create(program, "ForwardMaterial")` 通过 cbuffer declaration anchor 取得真实 group；
只为该组分配数值 storage，typed setters 拒绝其他 group。参数身份是
`ForwardMaterial.BaseColor` 这种 declaration-qualified 完整路径；全 program 唯一的叶名可作简写，
重名叶子必须使用完整路径，不猜 group 0。
`BuildRenderData` 复制 CPU 值，输出 raw TextureAsset pointer，并把 texture refs 追加到宿主的
retained vector；这个过程不创建 RHI set / SRV，也不维护 flight 或 descriptor 版本。

`ShaderProgram` 继续拥有 artifact、layout、shader、参数索引与 PSO map。PSO key 由 material state、
geometry vertex layout/topology、pass attachment facts 组成。ShaderJit、artifact/program cache 和
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
`MeshDrawList` 是 Forward 专用的 game-thread collector；PrepareFrame 主动遍历 `Scene::Primitives()`；queue 小于 2500 的 item 先按 program/material
聚簇，queue 大于等于 2500 的 item 按 view depth 从远到近排序，同 key 保持收集顺序。

### 内置 ForwardPipeline

每个 flight 的 `ForwardFrameInput` 保存相机 view/eye/透视参数、材质值快照、逐 section geometry/
index range/transform/material index，以及光源类型/参数/radius。它不含 Scene、proxy、CameraComponent、
Material、AssetManager 或 StreamingAssetRef。Geometry/TextureAsset 由 RenderSystem 的 per-flight
refs 保活；该 vector 仅在 game thread 清理和追加，flight 复用时清理后由既有 Pump 零引用回收。

Forward 私有 resolver 按 `ForwardView`、`ForwardMaterial`、`ForwardObject` 找到 cbuffer，读取
当前 target 的真实 group，并要求三组不同、三个 buffer dynamic。active `AlbedoTexture` 和
`LinearSampler` 必须属于 material group。失败按 program 负缓存并跳过 draw。DXIL spaces 与
SPIR-V sets 可以不同；CPU 不硬编码 0/1/2，也不建立 remap table。

每个 flight 自持 DynamicCBufferArena 和 frame-local parameter sets。开始复用时先销毁旧 sets，
再 Reset arena。view/object set 按 program + backing buffer 复用；material set 按当前 snapshot
index + backing bindings 复用。不同 backing target 创建不同 set，已发布的 set 在本帧内不再写。
PSO、数值上传和资源 binding 都在绘制前准备；draw loop 只 bind PSO/set/VB/IB 和 DrawIndexed。

每个窗口的 D32 depth attachment 由 Forward 维护；尺寸/sample count 改变时先清除引用旧 view 的
framebuffer，再重建。viewport 使用 MakeViewport；只有它在 Vulkan 下使用负 height。光照从帧
快照投影到 view bytes，点光按距已复制相机位置由近到远截断，超过上限只记录一次 warning。

## Application 与 runner

**启动**：`Run(desc)` = `InitializeRuntime` → `OnInit` → `StartLoop`。

`InitializeRuntime` 走 `ServiceRegistry` 的三阶段：

1. 创建 5 个核心服务（`WindowManager`、`GpuSystem`、`RenderSystem`、`AssetManager`、`World`）；
   `desc.AssetRoot` 非空时另开可选 `AssetDatabase`。交叉引用推迟到 phase 2。
2. `Add` 全部服务后 `Wire()`。
   数据库不进 registry；`Wire()` 后手工调用 `AssetManager::SetAssetSource`。
3. `Initialize()`，触发 `RenderSystem::OnInitialize`（建 `RenderPassRegistry`）。

之后建主窗口并挂 swapchain。

正常 runner 走 `Shutdown`；若初始化或游戏钩子的异常绕过正常路径，`Application` 析构仍调用同一
幂等内部 teardown（不再调用游戏侧虚钩子），先断开窗口的非 owning 引用，再按下述服务顺序回收。

**关停顺序是固定的**，理由见 `architecture/asset-system.md`：

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
在那里恢复就等于在渲染线程跑资产析构。见
[ADR-0009](../adr/0009-deferred-destroy-hands-over-suspension.md)。

Windows 下还有 `Win32ModalLoopVBlankRenderer`：模态循环（拖窗口、菜单）期间用 DXGI
`WaitForVBlank` + 一个消息窗口驱动渲染线程补帧，避免界面冻住。

## ServiceRegistry

非侵入、无单例、trait 驱动的三阶段装配。

```cpp
template <> struct ServiceTraits<AssetManager> {
    static constexpr auto Inject = std::tuple{&AssetManager::SetWaitFrameProcessor};
};
```

`Inject` 的元素是 **setter 成员函数指针**（形参类型自动抽出），
或 `As<Source>(setter)` 显式指定 resolve 来源类型（派生 → 基类上行转换时用）。

三阶段：

| 阶段 | API | 做什么 |
|---|---|---|
| 1 | `Add(T*)` | 非拥有登记。自动按 `RuntimeTypeTrait<T>::Bases` 为每个基类登记 resolve 别名（在 typed 上下文里 `static_cast` 修正多继承偏移） |
| 2 | `Wire()` | 展开每个服务的 `Inject`，按 `RuntimeTypeId` resolve 并调 setter。**缺依赖直接 `RADRAY_ABORT`** |
| 3 | `Initialize()` | 按登记序调用可选的 `OnInitialize()` |

**分三阶段的理由**：装配发生时全部实例已存在，所以互相持有引用天然可解。
`WindowManager` ↔ `GpuSystem` 就是一个双向环。

当前的装配关系：

| 服务 | Inject | OnInitialize |
|---|---|---|
| `WindowManager` | `SetGpuSystem`, `SetRenderSystem` | — |
| `GpuSystem` | `SetWindowManager` | — |
| `AssetManager` | `SetWaitFrameProcessor`（经 `Bases = IWaitFrameProcessor` 解析到 `GpuSystem`） | — |
| `RenderSystem` | — | 有（建 `RenderPassRegistry`） |
| `World` | — | — |

`AssetDatabase` 是可选依赖：`ServiceRegistry::Wire` 对缺失依赖会 abort，所以它不登记为服务，
也不出现在 `ServiceTraits<AssetManager>::Inject`。只有 `AssetRoot` 非空且 `Open` 成功时才手工注入。

## 现状陷阱

- **默认 pipeline 为空**：`RenderSystem::_pipeline` 只有在应用调用 `SetPipeline` 后才接线；
  `example_lambert_sphere` 的 pipeline 注入是一个显式样例路径。
- **第一期 forward 没有视锥剔除、instancing、shadow、post process 或 RT pool**；draw list 每相机
  每帧重建，depth attachment 是 pipeline 自有的最小子集。
- **group 数字来自当前 target metadata**：具体 pipeline 按 declaration 解释职责，Material 只认识 anchor 选中的一组。
- **`PrimitiveComponent` 基类仍返回空 proxy**：可绘制路径由 `StaticMeshComponent` 的派生实现提供。
- **JIT 不是 runtime 的可用性前提**：关闭 JIT 后 program 源码请求失败；未来 AOT consumer 仍可
  直接构造 `ShaderProgram`。
- **`radray_add_radray_gtest_case` 已定义但无人使用。**
