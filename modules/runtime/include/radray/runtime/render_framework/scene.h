#pragma once

#include <span>

#include <radray/types.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>

namespace radray {

class LightComponent;
class PrimitiveComponent;

class Scene {
public:
    Scene() = default;
    Scene(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene& operator=(Scene&&) = delete;
    ~Scene() noexcept;

    PrimitiveSceneProxy* AddPrimitive(PrimitiveComponent* component);
    void RemovePrimitive(PrimitiveSceneProxy* proxy) noexcept;
    LightSceneProxy* AddLight(LightComponent* component);
    void RemoveLight(LightSceneProxy* proxy) noexcept;

    std::span<const unique_ptr<PrimitiveSceneProxy>> Primitives() const noexcept { return _primitiveProxies; }
    std::span<const unique_ptr<LightSceneProxy>> Lights() const noexcept { return _lightProxies; }

private:
    vector<unique_ptr<PrimitiveSceneProxy>> _primitiveProxies;
    vector<unique_ptr<LightSceneProxy>> _lightProxies;
};

}  // namespace radray
