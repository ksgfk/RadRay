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
        │     └── StaticMeshComponent ← 持有 AssetRef<StaticMesh> + AssetRef<Material>
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
  └── StaticMeshSceneProxy         ← 持有 AssetRef<StaticMesh/Material>，产出 MeshBatchElement
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
  │
  └─ actor->OnSpawned()
       // Actor 子类可覆写，做初始化后逻辑
```

## SceneProxy 资源生命周期 (数据驱动上传)

PrimitiveComponent::OnRegister 只创建 Proxy 并交付资产引用 + 世界变换快照，
**不触碰任何 GPU 资源**。GPU 上传由渲染系统在每帧帧顶数据驱动完成:遍历 Scene 的
Proxy，对处于 Pending 态的调用 UpdateResources。组件层完全无感知。

```
ResourceState (PrimitiveSceneProxy):
  Pending    ← 初态。GPU 资源未就绪，需在帧顶 UpdateResources 上传/构建。
  Uploading  ← (预留) 异步传输队列上传中，跨帧未就绪。当前单队列模式不进入此态。
  Ready      ← 资源就绪，可参与可见性收集与绘制。
```

```
[游戏线程] StaticMeshComponent 注册
  └─ PrimitiveComponent::OnRegister
       └─ CreateSceneProxy() → StaticMeshSceneProxy(mesh, material)  // 初态 Pending
          scene->AddPrimitive(proxy)

[渲染线程] 每帧帧顶 (RenderPass 之前，裸 CommandBuffer)
  └─ SceneRenderer::PrepareResources(cmdBuffer, scene, uploader)
       └─ for proxy in scene->GetPrimitives():
            if proxy.GetResourceState() == Pending:
              proxy->UpdateResources(cmdBuffer, uploader)
                // StaticMeshSceneProxy:
                //   若 mesh 无 RenderData → uploader.UploadMesh 录制 copy → SetRenderMesh
                //     (共享 mesh 按 HasRenderData 去重，只上传一次)
                //   构建 MeshBatchElement + 顶点布局 → 转 Ready

[渲染线程] 同帧 RenderPass 内
  └─ SceneRenderer::Render → InitViews 只收 IsRenderable() (== Ready) 的 Proxy
       // 上传 copy 与本帧绘制同一提交 → 同帧即就绪;未就绪的 Proxy 跳过，下帧再试。
```

关键约束:buffer copy 不能在 RenderPass 内录制，故 PrepareResources 必须在任何
BeginRenderPass 之前、以裸 CommandBuffer 调用。线程时序与现有上传链路一致(渲染线程
帧顶读写 Proxy 状态)，由 runner 的信号量 + retire 锁保证。

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

```cpp
class MyApp : public Application {
    unique_ptr<World> _world;

    AppUpdateResult Update(const AppUpdateContext& ctx) override {
        _world->Tick(ctx.DeltaTime.count());
        return {.ShouldExit = false};
    }

    void Render(const AppRenderContext& ctx) override {
        Scene* scene = _world->GetScene();
        // 帧顶(RenderPass 之前):数据驱动上传 Pending Proxy 的 GPU 资源
        // sceneRenderer.PrepareResources(cmdBuffer, *scene, ctx.GetUploader());
        //
        // RenderPass 内: InitViews 收集 Ready Proxy → BasePass 构建/排序/录制
        // sceneRenderer.Render(encoder, *scene, sceneView, processorConfig);
    }
};
```
