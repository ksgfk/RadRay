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
│   │   └── static_mesh_component.h
│   └── renderer/                ← 渲染侧对象
│       ├── scene.h
│       └── primitive_scene_proxy.h
├── src/
│   ├── game_framework/
│   ├── components/
│   └── renderer/
```

## 类体系

```
ActorComponent                    ← 最基础组件，无 Transform，无空间概念
  └── SceneComponent              ← 引入 Transform + 父子 Attach 层级
        └── PrimitiveComponent    ← 有可渲染几何体，向 Scene 注册 Proxy
              └── StaticMeshComponent ← 持有 shared_ptr<MeshResource>

Actor
├── RootComponent: Nullable<SceneComponent*>  ← 纯逻辑 Actor 可以没有
├── OwnedComponents: vector<unique_ptr<ActorComponent>>
└── GetWorld() → Nullable<World*>

World
├── Scene (unique_ptr)               ← 渲染侧注册表
└── Actors (vector<unique_ptr<Actor>>)
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
        // SceneRenderer 从 scene->GetPrimitives() 收集 DrawCommands
        // for proxy in scene->GetPrimitives():
        //     commands += proxy->GetMeshDrawCommands()
        // sort & submit
    }
};
```
