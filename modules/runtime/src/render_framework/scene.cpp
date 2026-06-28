#include <radray/runtime/render_framework/scene.h>

#include <algorithm>

#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>

namespace radray {

Scene::~Scene() noexcept = default;

PrimitiveSceneProxy* Scene::AddPrimitive(PrimitiveComponent* component) {
    if (component == nullptr) {
        return nullptr;
    }

    unique_ptr<PrimitiveSceneProxy> proxy = component->CreateSceneProxy();
    if (proxy == nullptr) {
        return nullptr;
    }

    PrimitiveSceneProxy* raw = proxy.get();
    _primitiveProxies.push_back(std::move(proxy));
    return raw;
}

void Scene::RemovePrimitive(PrimitiveSceneProxy* proxy) noexcept {
    if (proxy == nullptr) {
        return;
    }

    auto it = std::find_if(_primitiveProxies.begin(), _primitiveProxies.end(),
                           [proxy](const unique_ptr<PrimitiveSceneProxy>& candidate) {
                               return candidate.get() == proxy;
                           });
    if (it != _primitiveProxies.end()) {
        _primitiveProxies.erase(it);
    }
}

}  // namespace radray
