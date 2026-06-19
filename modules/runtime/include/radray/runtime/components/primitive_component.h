#pragma once

#include <radray/runtime/components/scene_component.h>

namespace radray {

class PrimitiveSceneProxy;
class Scene;
class PrimitiveComponent;

template <>
struct RuntimeTypeTrait<PrimitiveComponent> {
    static constexpr RuntimeTypeId value{0x84b38cf1, 0x5e55, 0x4a71, 0xa8, 0xd6, 0x23, 0x12, 0x45, 0x2d, 0x6e, 0x91};
};

/// 可渲染的 SceneComponent。
/// OnRegister 时创建 PrimitiveSceneProxy 并注册到 Scene。
/// OnUnregister 时从 Scene 移除。
/// 对应 UE5 的 UPrimitiveComponent。
class PrimitiveComponent : public SceneComponent {
public:
    PrimitiveComponent() noexcept = default;
    ~PrimitiveComponent() noexcept override;

    RuntimeTypeId GetTypeId() const noexcept override;

    void OnRegister() override;
    void OnUnregister() override;

    /// 派生类实现：创建对应的 SceneProxy
    virtual unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() { return nullptr; }

    PrimitiveSceneProxy* GetSceneProxy() const noexcept { return _sceneProxy; }

protected:
    void OnTransformChanged() override;

    /// 重建 SceneProxy：移除旧代理(若有)、CreateSceneProxy 重建并注册。
    /// 供资产异步就绪后由组件调用(如 StaticMeshComponent::TickComponent)。
    /// 未注册或 CreateSceneProxy 返回 nullptr 时不做任何事。
    void RecreateSceneProxy();

private:
    Scene* GetScene() const noexcept;

    /// non-owning, Scene 持有 unique_ptr
    PrimitiveSceneProxy* _sceneProxy{nullptr};
};

}  // namespace radray
