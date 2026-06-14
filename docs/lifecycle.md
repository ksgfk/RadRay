# Actor/Component 生命周期时序

## 文件结构

```
modules/runtime/
├── include/radray/runtime/
│   ├── game_framework/          ← 高层游戏对象
│   │   ├── actor.h
│   │   └── world.h
│   ├── components/              ← 组件层级
│   │   ├── actor_component.h
│   │   ├── scene_component.h
│   │   ├── primitive_component.h
│   │   ├── static_mesh_component.h
│   │   └── camera_component.h
│   └── renderer/                ← 渲染侧对象
│       ├── scene.h
│       ├── primitive_scene_proxy.h
│       ├── static_mesh_scene_proxy.h
│       └── scene_renderer.h
├── src/
│   ├── game_framework/
│   ├── components/
│   └── renderer/
```

## 类体系

```
ActorComponent                    ← 最基础组件，无 Transform，无空间概念
  └── SceneComponent              ← 引入 Transform + 父子 Attach 层级
        ├── PrimitiveComponent    ← 有可渲染几何体，向 Scene 注册 Proxy
        │     └── StaticMeshComponent ← 持有 StreamingAssetRef<StaticMesh/Material>，就绪后建代理
        └── CameraComponent       ← 独立组件，由世界变换 + 投影参数产出 SceneView

Actor
├── RootComponent: Nullable<SceneComponent*>  ← 纯逻辑 Actor 可以没有
├── OwnedComponents: vector<unique_ptr<ActorComponent>>
└── GetWorld() → Nullable<World*>

World
├── Scene (unique_ptr)               ← 渲染侧注册表(对应 UE5 FSceneInterface)
└── Actors (vector<unique_ptr<Actor>>)
```

渲染侧(由 Scene 持有，组件不直接接触):

```
PrimitiveSceneProxy                ← 组件在渲染侧的代表(对应 UE5 FPrimitiveSceneProxy)
  └── StaticMeshSceneProxy         ← 持有 StreamingAssetRef<StaticMesh/Material>(非拥有)，产出 MeshBatchElement
```

## SpawnActor 时序

```
World::SpawnActor<MyActor>(args...)
  │
  ├─ new MyActor(args...)
  │    └─ 构造函数中创建组件:
  │         auto* root = AddComponent<SceneComponent>();
  │         SetRootComponent(root);
  │         auto* mesh = AddComponent<StaticMeshComponent>();
  │         mesh->AttachTo(root);
  │         // 注意: 此时 Actor 尚未加入 World，组件不会立即 Register
  │
  ├─ actor->_world = this
  ├─ actor->RegisterAllComponents()
  │    └─ for comp in _ownedComponents:
  │         comp->_registered = true
  │         comp->OnRegister()
  │              // SceneComponent::OnRegister → 无特殊逻辑
  │              // PrimitiveComponent::OnRegister → CreateSceneProxy() + scene->AddPrimitive(proxy)
  │              //   注意: StaticMeshComponent 此时资产可能尚未加载完成,CreateSceneProxy 返回 nullptr,
  │              //   待 TickComponent 中资产就绪后经 RecreateSceneProxy 补建。
  │
  └─ actor->OnSpawned()
       // Actor 子类可覆写，做初始化后逻辑
```

## SceneProxy 资源生命周期 (异步资产加载)

资产经 AssetManager **纯异步加载**:GPU 上传是资产构造的前置阶段,不再由渲染器轮询。
StaticMeshComponent 持有 StreamingAssetRef(加载状态 + ready 后访问,非拥有、不影响生命周期),每帧 TickComponent
检查就绪,两者(mesh + material)都 ready 后创建 SceneProxy。
SceneProxy 一经创建即 GPU 就绪、可直接绘制——渲染器只消费已就绪资产。

```
AssetState (AssetManager slot):
  Loading    ← 空位已占,加载协程在飞(读盘/GPU 上传),Object 尚未就绪。
  Ready      ← 资产已一次性构造,StreamingAssetRef 可直接访问对象。
  Faulted    ← 加载失败(AssetLoadResult::Failure 或协程异常)。
  Canceled   ← 加载被取消。
```

加载协程由调用方/loader 自行创建,返回统一的 `AssetLoadTask = task<AssetLoadResult>`。
loader 的参数形状不固定;AssetManager 只接受 `AssetLoadRequest`,并用内部 `task<void>` 包装协程承接结果。
阶段(读盘 / GPU 上传 / 构造)是协程内部事务,对 AssetManager 不可见:

```
[游戏线程] LoadStaticMesh(frameUploads, meshResource) → AssetLoadTask
  └─ AssetManager::Load<T>(AssetLoadRequest{id, task})
       └─ Emplace 一个 Loading 空位 + 提交 task 给内部 TaskScope → 返回 StreamingAssetRef<T>

[游戏线程] 每帧 Update 前
  └─ GpuSystem::PumpFrameUploadScheduler()
       // 恢复 GPU fence 已完成的加载协程,让它写入 pending result

[游戏线程] 每帧 AssetManager::Pump()
  └─ 提交 slot pending result → Ready/Faulted/Canceled

[渲染线程] 每帧帧顶 (GpuSystem::BeginFrameRecord,RenderPass 之前,裸 CommandBuffer)
  └─ FrameUploadScheduler::RunUploadPhase(cmdBuffer, uploader, flightIndex)  // 由渲染系统自动驱动
       // 把 cmd/uploader/flight 交给等在上传点的协程并 inline 恢复,
       // 加载协程自行在帧顶录 copy(无 callback),随后 co_await WaitGpu 挂起等 fence

[渲染线程] 该 flight 的 fence 完成后 (GpuSystem::CompleteFlight)
  └─ FrameUploadScheduler::NotifyFlightComplete(flightIndex)  // 由渲染系统自动驱动
       // 标记上传完成 → 下次 PumpFrameUploadScheduler 恢复加载协程

[游戏线程] StaticMeshComponent::TickComponent
  └─ 若 StreamingAssetRef 就绪且尚无 proxy:RecreateSceneProxy()
       └─ CreateSceneProxy() → StaticMeshSceneProxy(mesh, material)  // 构造即建几何、即可绘制
          scene->AddPrimitive(proxy)

[渲染线程] RenderPass 内
  └─ SceneRenderer::Render → InitViews 收 IsRenderable() 的 Proxy → BasePass 录制
```

关键点:
- 协程 op-state 由 `radray::TaskScope` 管理;AssetManager 内部持有 load scope。
  单个加载的取消由 slot 内 `stop_source` 管理,不会停止整个 scope。
- GPU 上传 awaitable 由 GpuSystem 内的 FrameUploadScheduler 管理。
- ResourceUploader 仍归 GpuSystem;GpuSystem 只在帧顶/fence 完成阶段驱动 FrameUploadScheduler。
- AssetManager 不做引用计数,也不做 GC。StreamingAssetRef/StreamingAssetRefAny 均为非拥有弱句柄。
  资产回收由应用层显式 AssetManager::Unload(id) 控制。
- 同 id 重复 Load 复用在飞或已就绪资产(去重)。加载完成的资产留在库中,
  重复 Load 直接命中,不重新加载。
- buffer copy 不能在 RenderPass 内录制,故 FrameUploadScheduler upload phase 在帧顶
  (任何 BeginRenderPass 之前、以裸 CommandBuffer)由 BeginFrameRecord 调用。

## Tick 时序

```
World::Tick(deltaTime)
  └─ for actor in _actors:
       actor->Tick(deltaTime)
         └─ for comp in _ownedComponents:
              comp->TickComponent(deltaTime)
```

## DestroyActor 时序

```
World::DestroyActor(actor)
  │
  ├─ actor->UnregisterAllComponents()   // 逆序
  │    └─ for comp in _ownedComponents (reverse order):
  │         comp->OnUnregister()
  │              // PrimitiveComponent::OnUnregister → scene->RemovePrimitive(proxy)
  │              // SceneComponent::OnUnregister → 无特殊逻辑
  │         comp->_registered = false
  │
  ├─ actor->OnDestroyed()
  │    // Actor 子类可覆写，做销毁前清理
  │
  ├─ actor->_world = nullptr
  └─ erase from _actors
       └─ unique_ptr<Actor> 析构
            └─ ~Actor()
                 └─ ~vector<unique_ptr<ActorComponent>>
                      └─ 各 Component 析构 (SceneComponent::~SceneComponent 会 DetachFromParent)
```

## AddComponent 时序 (Actor 已在 World 中)

```
actor->AddComponent<T>(args...)
  ├─ new T(args...)
  ├─ comp->_owner = this
  ├─ _ownedComponents.push_back(comp)
  └─ if (_world != nullptr):      // Actor 已在 World 中
       comp->_registered = true
       comp->OnRegister()
```

## RemoveComponent 时序

```
actor->RemoveComponent(comp)
  ├─ if (comp->IsRegistered()):
  │    comp->OnUnregister()
  │    comp->_registered = false
  ├─ if (comp == _rootComponent):
  │    _rootComponent = nullptr
  ├─ if (comp->IsSceneComponent()):
  │    comp->DetachFromParent()
  ├─ comp->_owner = nullptr
  └─ erase from _ownedComponents (unique_ptr 析构)
```

## Transform 传播

```
SceneComponent::SetRelativeLocation(loc)
  ├─ _relativeLocation = loc
  └─ OnTransformChanged()    // 虚函数
       // PrimitiveComponent::OnTransformChanged → proxy->SetWorldMatrix(GetWorldMatrix())

SceneComponent::GetWorldMatrix()
  └─ local = ComposeTransform(location, rotation, scale)
     if (parent): return parent->GetWorldMatrix() * local
     else:        return local
     // 每次调用重新计算，无缓存
```

## Attach 层级

```
meshComp->AttachTo(rootComp)
  ├─ DetachFromParent()         // 先从旧 parent 脱离
  ├─ _parent = rootComp
  ├─ rootComp->_children.push_back(this)
  └─ OnTransformChanged()

meshComp->DetachFromParent()
  ├─ parent->_children.erase(this)
  ├─ _parent = nullptr
  └─ OnTransformChanged()
```

## 与 Application 框架的集成

`Application` 已固化引擎帧序与启动/关闭流程。游戏层只重写窄接口:
`OnInit / OnUpdate / OnImGui / OnRenderMainView / OnShutdown`,不再手写
device/window/GpuSystem/ImGui 初始化、帧 Tick 顺序、多 viewport acquire/present、
backbuffer barrier 状态追踪、GPU 计时。

固化的每帧顺序(`Application::Update` 内,游戏线程):
`AssetManager::Pump → OnUpdate → World::Tick → ImGui BeginFrame/OnImGui/EndFrame/ExtractDrawData`。
渲染(`Application::Render`,渲染线程):若挂了 ImGuiSystem 则走
`ImGuiSystem::RenderViewports`(内部 acquire/barrier/RenderPass/present),
主 viewport 背后通过 `OnRenderMainView` 回调画场景;否则走主窗口直渲染路径。

```cpp
class MyApp : public Application {
    void OnInit() override {
        // 运行时已就绪(device/window/gpu/imgui/asset/world):加载资产、Spawn Actor、建相机。
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        // 每帧游戏逻辑(World::Tick 之前;AssetManager::Pump 之后)。
    }

    void OnImGui(const AppUpdateContext& ctx) override {
        // ImGui::NewFrame 与 EndFrame 之间:录制 UI。
    }

    bool OnRenderMainView(AppFrameContext& ctx, const AppFrameTarget& target) override {
        // 主 viewport UI 背后的场景内容。RenderPass 内:
        // InitViews 收集 Ready Proxy → BasePass 构建/排序/录制。
        // sceneRenderer.Render(encoder, *_world->GetScene(), sceneView, processorConfig);
        return true;  // 已向 backbuffer 写入(ImGui 随后 Load 叠加);false 则 ImGui Clear。
    }

    void OnShutdown() override {
        // GPU 已 idle:释放自管 per-flight 资源、置空指向 World 的非 owning 指针。
    }
};

int main(int argc, char* argv[]) {
    ApplicationRuntimeDescriptor desc{};
    desc.Backend = render::RenderBackend::Vulkan;
    desc.WindowTitle = "My App";
    desc.EnableImGui = true;
    MyApp app{};
    return app.Run(desc);
}
```
