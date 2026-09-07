#pragma once

#include <span>

#include <radray/types.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>

namespace radray {

/// Game-thread proxy registry. Registration transfers ownership; snapshots retain asset owners separately.
class Scene {
public:
    Scene() = default;
    Scene(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene& operator=(Scene&&) = delete;
    ~Scene() noexcept;

    Nullable<PrimitiveSceneProxy*> AddPrimitive(unique_ptr<PrimitiveSceneProxy> proxy);
    void RemovePrimitive(PrimitiveSceneProxy* proxy) noexcept;
    Nullable<LightSceneProxy*> AddLight(unique_ptr<LightSceneProxy> proxy);
    void RemoveLight(LightSceneProxy* proxy) noexcept;

    std::span<const unique_ptr<PrimitiveSceneProxy>> Primitives() const noexcept { return _primitiveProxies; }
    std::span<const unique_ptr<LightSceneProxy>> Lights() const noexcept { return _lightProxies; }

private:
    vector<unique_ptr<PrimitiveSceneProxy>> _primitiveProxies;
    vector<unique_ptr<LightSceneProxy>> _lightProxies;
};

}  // namespace radray
