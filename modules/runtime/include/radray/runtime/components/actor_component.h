#pragma once

#include <radray/nullable.h>
#include <radray/runtime_type.h>

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

    virtual RuntimeTypeId GetTypeId() const noexcept;

    Nullable<Actor*> GetOwner() const noexcept { return _owner; }
    Nullable<World*> GetWorld() const noexcept;
    bool IsRegistered() const noexcept { return _registered; }

private:
    friend class Actor;

    Nullable<Actor*> _owner{nullptr};
    bool _registered{false};
};

template <>
struct RuntimeTypeTrait<ActorComponent> {
    static constexpr RuntimeTypeId value{0x9a5c2e10, 0x8ed4, 0x4f35, 0xa1, 0xd7, 0x17, 0x63, 0x92, 0x04, 0x2b, 0x8a};
    using Bases = std::tuple<>;
};

}  // namespace radray
