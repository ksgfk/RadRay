#pragma once

#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/render/renderer.h>

namespace radray {

namespace srp {
class Scene;
}  // namespace srp

class PrimitiveComponent;

template <>
struct RuntimeTypeTrait<PrimitiveComponent> {
    static constexpr RuntimeTypeId value{0x84b38cf1, 0x5e55, 0x4a71, 0xa8, 0xd6, 0x23, 0x12, 0x45, 0x2d, 0x6e, 0x91};
};

/// 可渲染的 SceneComponent。
/// OnRegister 时构建 srp::Renderer 实体并注册到 srp::Scene。
/// OnUnregister 时从 Scene 移除。
/// 对应 UE5 的 UPrimitiveComponent。
///
/// 组件【拥有】其 srp::Renderer(unique_ptr),Scene 只借用裸指针。
class PrimitiveComponent : public SceneComponent {
public:
    PrimitiveComponent() noexcept = default;
    ~PrimitiveComponent() noexcept override;

    RuntimeTypeId GetTypeId() const noexcept override;

    void OnRegister() override;
    void OnUnregister() override;

    /// 派生类实现:构建本组件对应的 srp::Renderer(可多个,如多 section)。
    virtual vector<unique_ptr<srp::Renderer>> BuildRenderers() { return {}; }

    /// 已构建且已注册到 Scene 的 renderer(借用视图)。
    std::span<const unique_ptr<srp::Renderer>> GetRenderers() const noexcept { return _renderers; }
    bool HasRenderers() const noexcept { return !_renderers.empty(); }

protected:
    void OnTransformChanged() override;

    /// 重建 renderer:移除旧的、BuildRenderers 重建并注册。
    /// 供资产异步就绪后由组件调用(如 StaticMeshComponent::TickComponent)。
    /// 未注册时不做任何事。
    void RecreateRenderers();

private:
    srp::Scene* GetScene() const noexcept;
    void RegisterRenderers();
    void UnregisterRenderers();

    /// 组件拥有 renderer 实体;Scene 仅借用裸指针。
    vector<unique_ptr<srp::Renderer>> _renderers;
};

}  // namespace radray
