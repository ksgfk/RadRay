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

`SetPipeline` 是应用侧装配入口：`RenderSystem` 只拥有当前 pipeline，不拥有其借用的
`Scene`/component。`ForwardPipeline` 构造时借用 `Application`、`Scene` 与
`CameraComponent`；example 只做对象装配，不再拥有命令录制实现。

**pass 注册与排序**：`OnAddRenderPasses` 里用 `EnqueuePass` 把 pass 压进 `_activePasses`；
`OnExecutePasses` 用 `std::stable_sort` 按 `RenderPassEvent` 的整数值升序排，
然后逐个 `Setup → Execute → Cleanup`。`stable_sort` 是刻意的：同一 event 上的 pass
保持入队顺序。`OnFinishCamera` 与 `OnEndFrame` 都会 `ClearPasses()`，
所以 pass 队列是**每相机重建**的，不是持久配置。

pass 在成功录制内容后调用受保护的 `MarkContentDrawn()`；框架根据当前 `RenderCamera::Target`
回写对应的 `RenderPipelineTarget::ContentDrawn`。具体 pass 不遍历 `ctx.Targets`，也不直接改目标标志。

`Material` 持有一个 concrete Variant 的 `ShaderProgram*`、type-tree 打包后的数值参数、纹理引用、
sampler 描述、完整固定功能状态基线和 `RenderQueue`。topology 与 `PrimitiveVertexLayout` 来自 geometry；
color/depth format、sample count 与 compatible render pass 来自 pass。`ShaderProgram` 拥有 decoded
artifact、layout、stage shader、扁平参数索引和自己的 PSO map；PSO key 正好组合这三方事实。
`BindingGroupPlan` 则由具体 pipeline 指定 view/material/object group，material 与通用执行器不写
group 数字字面量。

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

**proxy 常驻，不是每帧重建快照，也不是逐字段增量同步。** proxy 在组件 `OnRegister` 时创建，
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
（见 `architecture/asset-system.md` 的引用计数不变量）。

`StaticMeshComponent` 按 section 保存 material，`StaticMeshSceneProxy` 自持 mesh ref、material 快照与
local-to-world，并把 `StaticMeshSection` 的 `FirstIndex` / `IndexCount` / `VertexOffset` 投影成 draw。
`MeshDrawList` 每相机主动遍历 `Scene::Primitives()`；queue 小于 2500 的 item 先按 program/material
聚簇，queue 大于等于 2500 的 item 按 view depth 从远到近排序，同 key 保持收集顺序。

### 内置 ForwardPipeline

`ForwardPipeline::GetBindingGroupPlan()` 固定返回 view/material/object = `0/1/2`。每个 flight 持有
一个 `DynamicCBufferArena`；view/object parameter set 按 program 与 arena backing buffer 常驻，
material 的纹理/sampler set 按 flight 常驻。每帧只把数值 bytes 写进 arena，draw loop 只绑定 set、
下发 dynamic offset、绑定 VB/IB 并调用 `DrawIndexed`，不创建 descriptor。

forward pipeline 为每个窗口维护 D32 depth texture/view，尺寸或 sample count 改变时先从
`RenderPassRegistry` 清除引用旧 view 的 framebuffer，再重建 attachment。viewport 始终由
`MakeViewport` 产生；只有该 helper 在 Vulkan 下使用负 height。光照从 `Scene::Lights()` 读取完整
`LightRenderParameters`，在 pipeline 内投影到 forward view buffer；超过 shader 上限时点光按距相机
由近到远截断，并且只记录一次 warning。

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
- **binding group plan 属于具体 pipeline**：只有 `ForwardPipeline` 可以假定 0/1/2；通用 material、
  collector 和执行器都必须消费 plan。
- **`PrimitiveComponent` 基类仍返回空 proxy**：可绘制路径由 `StaticMeshComponent` 的派生实现提供。
- **JIT 不是 runtime 的可用性前提**：关闭 JIT 后 program 源码请求失败；未来 AOT consumer 仍可
  直接构造 `ShaderProgram`。
- **`radray_add_radray_gtest_case` 已定义但无人使用。**
