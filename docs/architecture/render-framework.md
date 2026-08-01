> - 适用: 改渲染管线、场景表示、Application 生命周期或服务装配
> - 权威: 本文是 runtime 层「除资产系统与 GPU 帧管理之外」部分的唯一说明。那两块见 `architecture/asset-system.md` 与 `architecture/frame-and-gpu.md`
> - 锚点: `modules/runtime/include/radray/runtime/render_framework/render_pipeline.h`, `modules/runtime/include/radray/runtime/service_registry.h`, `modules/runtime/src/application.cpp`, `modules/runtime/src/render_system.cpp`

# 渲染框架与 game framework

**先读这条**：`RenderPipeline` 框架已经建好，但**还没有任何具体管线接上去**。
`RenderSystem::_pipeline` 恒为 null，仓库里没有任何 `RenderPipelinePass` 子类，
`PrimitiveComponent::CreateSceneProxy` 基类返回 nullptr。所以"场景 → proxy → draw call"
这条路目前跑不通。本文描述的是**框架的形状与约定**，不是一条可运行的渲染路径。

## 分层

```
Application            进程生命周期、runner 选择、帧循环
  ├─ WindowManager     窗口与 swapchain
  ├─ GpuSystem         device、queue、flight、上传        → frame-and-gpu.md
  ├─ RenderSystem      "怎么画"：RenderPassRegistry、PSO 缓存、DXC、RenderPipeline
  ├─ AssetManager      资产生命周期                       → asset-system.md
  └─ World             Actor / Component / Scene
```

`RenderSystem` 拥有"怎么画"，**不拥有帧时序**——那是 `GpuSystem` 与 runner 的事。

## 渲染管线

```
RenderSystem::Render
  └─ 逐窗口 AcquireWindow → vector<RenderPipelineTarget>
     └─ pipeline: BeginFrame → BuildCameraList → Render → EndFrame
```

`RenderPipeline::Render` 先 `PrepareTargets`（把每个 target 过渡到 `RenderTarget` 状态，
并查 `RenderViewContent` 是否已画），然后逐相机走固定阶段链：

```
OnSetupCamera → OnSetupCulling → OnSetupLights → OnAddRenderPasses
              → OnExecutePasses → OnFinishCamera
```

最后对 `ContentDrawn == false` 的目标调 `ClearTarget` 兜底清屏——这样不必为"什么都没画"
单独安排一个 clear pass。

**pass 注册与排序**：`OnAddRenderPasses` 里用 `EnqueuePass` 把 pass 压进 `_activePasses`；
`OnExecutePasses` 用 `std::stable_sort` 按 `RenderPassEvent` 的整数值升序排，
然后逐个 `Setup → Execute → Cleanup`。`stable_sort` 是刻意的：同一 event 上的 pass
保持入队顺序。`OnFinishCamera` 与 `OnEndFrame` 都会 `ClearPasses()`，
所以 pass 队列是**每相机重建**的，不是持久配置。

`MaterialRenderState` 是材质对 PSO 状态的三态覆盖（`optional` 字段 = 不覆盖）。
**它当前没有基线来源也没有使用点**——Topology / FrontFace / DepthCompare / target Format
由谁提供基线还没裁决。

### 与 shaderlib 的绑定对应

绑定编号的唯一定义处是 `shaderlib/forward_pipeline/bindings.hlsli`：
`RADRAY_FORWARD_OBJECT_GROUP 0` / `VIEW_GROUP 1` / `MATERIAL_GROUP 2`，
以及展开成 `VK_BINDING(...) register(...)` 的 `RADRAY_FORWARD_*` 宏。

CPU 侧**不引用这些宏**。链条是：

```
HLSL 宏 → DXC 反射 + tools/shader_gen → *.shader.json 的 BindingGroups → CPU 只读 manifest
```

manifest 是 ABI 权威（[ADR-0003](../adr/0003-manifest-is-abi-authority.md)），
所以 group / binding 索引从 `ShaderPassProgram::GetDesc()` 取，
**不要在 C++ 里硬编码 0/1/2**。`test_vertical_slice.cpp` 对此有专门断言。

CPU 侧的常量缓冲镜像结构用 `static_assert(sizeof(...))` + `offsetof` 钉住布局，
和 `view.hlsli` 保持一致。改 HLSL 里的 struct 就要同时改镜像与断言。

## 场景表示

数据从 game framework 流到渲染侧的路径：

```
World 构造          → RenderSystem::AllocateScene()
SpawnActor          → RegisterAllComponents → ActorComponent::OnRegister
PrimitiveComponent  → CreateRenderState → Scene::AddPrimitive(CreateSceneProxy())
LightComponent      → CreateRenderState → Scene::AddLight(CreateSceneProxy())
```

**proxy 常驻，不是每帧重建快照，也不是逐字段增量同步。** proxy 在组件 `OnRegister` 时创建，
存在 `Scene` 的 `vector<unique_ptr<...>>` 里，`OnUnregister` 时移除。

**任何属性或变换变化走 `MarkRenderStateDirty()` → 整棵 proxy 销毁重建。**
`OnTransformChanged` 也走这条路。Light proxy 的参数在构造函数里一次性从 component 快照。
这个粒度很粗，但它让"proxy 里的数据什么时候会变"有一个确定答案：只在重建时。

### draw call

```cpp
struct MeshDrawArgs {
    GpuMesh::DrawData* Geometry;  // VB/IB 视图
    uint32_t FirstIndex, IndexCount, VertexOffset;
};
```

**覆写 `GetDrawArgs` 的 proxy 必须自持一份 `StreamingAssetRef<StaticMesh>`。**
`Geometry` 是指向资产内部的裸指针，保命责任在 proxy 自己
（见 `architecture/asset-system.md` 的引用计数不变量）。

现有 proxy 类型：`PrimitiveSceneProxy`（基类）、`LightSceneProxy` → `PointLightSceneProxy` /
`DirectionalLightSceneProxy`（带 CSM 级联配置）。**没有任何具体的 primitive proxy 子类。**

## Application 与 runner

**启动**：`Run(desc)` = `InitializeRuntime` → `OnInit` → `StartLoop`。

`InitializeRuntime` 走 `ServiceRegistry` 的三阶段：

1. 创建 5 个服务（`WindowManager`、`GpuSystem`、`RenderSystem`、`AssetManager`、`World`）。
   顺序任意——交叉引用推迟到 phase 2。
2. `Add` 全部服务后 `Wire()`。
3. `Initialize()`，触发 `RenderSystem::OnInitialize`（建 `RenderPassRegistry`、PSO 缓存、DXC）。

之后建主窗口并挂 swapchain。

**关停顺序是固定的**，理由见 `architecture/asset-system.md`：

```
WaitAndCleanupCompletedFlights → OnShutdown → scheduler.CancelAll
  → World（先拆 proxy 与 AssetRef）
  → WindowManager::SetRenderSystem(nullptr) → RenderSystem
  → AssetManager（force-unload）
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
| `RenderSystem` | — | 有（建 registry / PSO 缓存 / DXC） |
| `World` | — | — |

## 现状陷阱

- **`RenderPipeline` 未接线**：`RenderSystem::_pipeline` 恒 null，无任何 `RenderPipelinePass`
  子类。`ForwardPipeline` 只存在于 shaderlib 与注释里。
- **`MaterialRenderState` 零使用点**，基线来源未裁决。
- **`RenderQueue` 枚举在 runtime 内无消费者**（只有 `examples/sphere_demo` 用）。
- **无任何具体 primitive proxy**：`PrimitiveComponent::CreateSceneProxy` 基类返回 nullptr，
  所以 `MeshDrawArgs` / `GetDrawArgs` 目前没有消费方。缺的是 material 层与 mesh component。
- **`examples/` 引用了已删除的头文件**（`material_asset.h`、`static_mesh_component.h`、
  `static_mesh_scene_proxy.h`、`standard_material_factory.h` 等）。加上
  `examples/CMakeLists.txt` 里两个 `add_subdirectory` 都被注释掉，所以它们不编译也修不了。
- **`radray_add_radray_gtest_case` 已定义但无人使用。**
