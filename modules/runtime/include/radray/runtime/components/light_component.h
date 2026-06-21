#pragma once

#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/renderer/light_scene_proxy.h>

namespace radray {

class Scene;
class LightComponent;

template <>
struct RuntimeTypeTrait<LightComponent> {
    static constexpr RuntimeTypeId value{0x7e32d0c4, 0x9b0f, 0x43a2, 0x96, 0x91, 0x79, 0xdf, 0x3c, 0x31, 0x45, 0xb2};
};

/// Light source component registered into the render Scene as a LightSceneProxy.
class LightComponent : public SceneComponent {
public:
    LightComponent() noexcept = default;
    ~LightComponent() noexcept override;

    RuntimeTypeId GetTypeId() const noexcept override;

    void SetLightType(LightType type) noexcept;
    void SetDirection(const Eigen::Vector3f& direction) noexcept;
    void SetColor(const Eigen::Vector3f& color) noexcept;
    void SetIntensity(float intensity) noexcept;

    LightType GetLightType() const noexcept { return _type; }
    const Eigen::Vector3f& GetDirection() const noexcept { return _direction; }
    const Eigen::Vector3f& GetColor() const noexcept { return _color; }
    float GetIntensity() const noexcept { return _intensity; }

    void OnRegister() override;
    void OnUnregister() override;

protected:
    void OnTransformChanged() override;

private:
    Scene* GetScene() const noexcept;
    void PushParametersToProxy() noexcept;

    LightType _type{LightType::Point};
    Eigen::Vector3f _direction{0.0f, -1.0f, 0.0f};
    Eigen::Vector3f _color{1.0f, 1.0f, 1.0f};
    float _intensity{1.0f};

    /// non-owning, Scene holds the unique_ptr.
    LightSceneProxy* _sceneProxy{nullptr};
};

}  // namespace radray
