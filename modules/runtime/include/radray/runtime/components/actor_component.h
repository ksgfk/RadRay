#pragma once

#include <radray/nullable.h>
#include <radray/runtime_type.h>
#include <radray/runtime/game_framework/world_id.h>

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

    /// IsRegistered() 为 false；不要求派生类调用基类。
    virtual void OnUnregister() {}

    /// 每帧逻辑更新。默认不调度；覆盖后须 `SetTickEnabled(true)`。
    virtual void TickComponent(float deltaTime) { (void)deltaTime; }

    void SetTickEnabled(bool enabled) noexcept;
    bool IsTickEnabled() const noexcept { return _tickEnabled; }

    Nullable<Actor*> GetOwner() const noexcept { return _owner; }
    Nullable<World*> GetWorld() const noexcept { return _world; }
    bool IsRegistered() const noexcept { return _registration == ComponentRegistration::Registered; }
    ComponentRegistration GetRegistrationState() const noexcept { return _registration; }
    ObjectLifecycle GetLifecycle() const noexcept { return _lifecycle; }
    bool IsLive() const noexcept;
    ComponentId GetId() const noexcept { return _id; }
    uint64_t GetFirstTickEpoch() const noexcept { return _firstTickEpoch; }

protected:
    void CheckCanModify() const noexcept;

private:
    friend class Actor;
    friend class World;

    Nullable<Actor*> _owner{nullptr};
    Nullable<World*> _world{nullptr};
    ComponentRegistration _registration{ComponentRegistration::Unregistered};
    ObjectLifecycle _lifecycle{ObjectLifecycle::Initializing};
    ComponentId _id;
    uint64_t _firstTickEpoch{0};
    bool _tickEnabled{false};
};

template <>
struct RuntimeTypeTrait<ActorComponent> {
    static constexpr RuntimeTypeId value{0x9a5c2e10, 0x8ed4, 0x4f35, 0xa1, 0xd7, 0x17, 0x63, 0x92, 0x04, 0x2b, 0x8a};
};

}  // namespace radray
