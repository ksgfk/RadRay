#pragma once

#include <radray/nullable.h>

namespace radray {

class Actor;
class World;

/// 最基础组件。无空间概念。
/// 对应 UE5 的 UActorComponent。
class ActorComponent {
public:
    ActorComponent() noexcept = default;
    ActorComponent(const ActorComponent&) = delete;
    ActorComponent(ActorComponent&&) = delete;
    ActorComponent& operator=(const ActorComponent&) = delete;
    ActorComponent& operator=(ActorComponent&&) = delete;
    virtual ~ActorComponent() noexcept = default;

    /// 组件注册到 World 时调用（Actor::RegisterAllComponents 触发）
    virtual void OnRegister() {}

    /// 组件从 World 注销时调用（Actor 销毁前）
    virtual void OnUnregister() {}

    /// 每帧逻辑更新
    virtual void TickComponent(float deltaTime) { (void)deltaTime; }

    /// 是否是 SceneComponent（有 Transform）
    virtual bool IsSceneComponent() const noexcept { return false; }

    Nullable<Actor*> GetOwner() const noexcept { return _owner; }
    Nullable<World*> GetWorld() const noexcept;
    bool IsRegistered() const noexcept { return _registered; }

private:
    friend class Actor;

    Nullable<Actor*> _owner{nullptr};
    bool _registered{false};
};

}  // namespace radray
