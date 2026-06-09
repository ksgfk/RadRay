#pragma once

#include <radray/runtime/components/scene_component.h>

namespace radray {

class PrimitiveSceneProxy;
class Scene;

/// 可渲染的 SceneComponent。
/// OnRegister 时创建 PrimitiveSceneProxy 并注册到 Scene。
/// OnUnregister 时从 Scene 移除。
/// 对应 UE5 的 UPrimitiveComponent。
class PrimitiveComponent : public SceneComponent {
public:
    PrimitiveComponent() noexcept = default;
    ~PrimitiveComponent() noexcept override;

    void OnRegister() override;
    void OnUnregister() override;

    /// 派生类实现：创建对应的 SceneProxy
    virtual unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() { return nullptr; }

    PrimitiveSceneProxy* GetSceneProxy() const noexcept { return _sceneProxy; }

protected:
    void OnTransformChanged() override;

private:
    Scene* GetScene() const noexcept;

    /// non-owning, Scene 持有 unique_ptr
    PrimitiveSceneProxy* _sceneProxy{nullptr};
};

}  // namespace radray
