#pragma once

#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/types.h>

namespace radray {

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

private:
    vector<unique_ptr<PrimitiveSceneProxy>> _primitiveProxies;
};

}  // namespace radray
