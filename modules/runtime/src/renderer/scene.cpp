#include <radray/runtime/renderer/scene.h>

#include <algorithm>

#include <radray/runtime/renderer/primitive_scene_proxy.h>

namespace radray {

Scene::Scene() noexcept = default;

Scene::~Scene() noexcept = default;

void Scene::AddPrimitive(unique_ptr<PrimitiveSceneProxy> proxy) {
    _primitives.push_back(std::move(proxy));
}

void Scene::RemovePrimitive(PrimitiveSceneProxy* proxy) {
    auto it = std::find_if(_primitives.begin(), _primitives.end(),
                           [proxy](const unique_ptr<PrimitiveSceneProxy>& ptr) {
                               return ptr.get() == proxy;
                           });
    if (it != _primitives.end()) {
        _primitives.erase(it);
    }
}

std::span<const unique_ptr<PrimitiveSceneProxy>> Scene::GetPrimitives() const noexcept {
    return {_primitives.data(), _primitives.size()};
}

size_t Scene::GetPrimitiveCount() const noexcept {
    return _primitives.size();
}

}  // namespace radray
