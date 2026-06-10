#pragma once

#include <span>

#include <radray/types.h>

namespace radray {

class PrimitiveSceneProxy;

/// 渲染侧的世界注册表。
/// PrimitiveComponent::OnRegister 时向 Scene 注册 Proxy。
/// SceneRenderer 从 Scene 收集 Proxy 进行渲染。
/// 对应 UE5 的 FScene。
class Scene {
public:
    Scene() noexcept;
    Scene(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene& operator=(Scene&&) = delete;
    ~Scene() noexcept;

    void AddPrimitive(unique_ptr<PrimitiveSceneProxy> proxy);
    void RemovePrimitive(PrimitiveSceneProxy* proxy);

    /// 当前返回全部。后续加 frustum culling。
    std::span<const unique_ptr<PrimitiveSceneProxy>> GetPrimitives() const noexcept;
    size_t GetPrimitiveCount() const noexcept;

private:
    vector<unique_ptr<PrimitiveSceneProxy>> _primitives;
};

}  // namespace radray
