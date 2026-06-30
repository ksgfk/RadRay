#include <radray/runtime/render_framework/scene.h>

#include <algorithm>

#include <radray/runtime/components/light_component.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>
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

LightSceneProxy* Scene::AddLight(LightComponent* component) {
    if (component == nullptr) {
        return nullptr;
    }

    unique_ptr<LightSceneProxy> proxy = component->CreateSceneProxy();
    if (proxy == nullptr) {
        return nullptr;
    }

    LightSceneProxy* raw = proxy.get();
    _lightProxies.push_back(std::move(proxy));
    return raw;
}

void Scene::RemoveLight(LightSceneProxy* proxy) noexcept {
    if (proxy == nullptr) {
        return;
    }

    auto it = std::find_if(_lightProxies.begin(), _lightProxies.end(),
                           [proxy](const unique_ptr<LightSceneProxy>& candidate) {
                               return candidate.get() == proxy;
                           });
    if (it != _lightProxies.end()) {
        _lightProxies.erase(it);
    }
}

}  // namespace radray
