#pragma once

#include <radray/runtime/components/scene_component.h>

namespace radray {

class PrimitiveSceneProxy;
class Scene;

/// Base class for components that have a render-side primitive representation.
/// Corresponds to UE5's UPrimitiveComponent.
class PrimitiveComponent : public SceneComponent {
public:
    PrimitiveComponent() noexcept = default;
    ~PrimitiveComponent() noexcept override;

    RuntimeTypeId GetTypeId() const noexcept override;

    void OnRegister() override;
    void OnUnregister() override;

    void MarkRenderStateDirty();
    PrimitiveSceneProxy* GetSceneProxy() const noexcept { return _sceneProxy; }

    virtual bool ShouldCreateRenderState() const;
    virtual unique_ptr<PrimitiveSceneProxy> CreateSceneProxy();

protected:
    void OnTransformChanged() override;

private:
    Scene* GetScene() const noexcept;
    void CreateRenderState();
    void DestroyRenderState() noexcept;

    PrimitiveSceneProxy* _sceneProxy{nullptr};
    bool _renderStateCreated{false};
};

template <>
struct RuntimeTypeTrait<PrimitiveComponent> {
    static constexpr RuntimeTypeId value{0xfb11f0d6, 0xc97b, 0x4f3f, 0x98, 0xe3, 0xf5, 0x16, 0x8c, 0xbf, 0x0f, 0x42};
};

}  // namespace radray
