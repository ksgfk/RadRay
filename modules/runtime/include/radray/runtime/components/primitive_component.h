#pragma once

#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/render_framework/scene.h>

namespace radray {

/// GT component identity; Scene owns the corresponding render-side state.
class PrimitiveComponent : public SceneComponent {
public:
    PrimitiveComponent() noexcept = default;
    ~PrimitiveComponent() noexcept override;

    PrimitiveId GetPrimitiveId() const noexcept { return _primitiveId; }

protected:
    void OnTransformChanged() override;
    virtual void CollectPrimitiveUpdates(SceneUpdateBatch& batch, RenderDirtyFlags dirty) {
        (void)batch;
        (void)dirty;
    }

private:
    void CreateRenderState(World& world) final;
    void DestroyRenderState(World& world) final;
    void CollectRenderUpdates(SceneUpdateBatch& batch, RenderDirtyFlags dirty) final;

    PrimitiveId _primitiveId;
    bool _renderStateSent{false};
};

template <>
struct RuntimeTypeTrait<PrimitiveComponent> {
    static constexpr RuntimeTypeId value{0xfb11f0d6, 0xc97b, 0x4f3f, 0x98, 0xe3, 0xf5, 0x16, 0x8c, 0xbf, 0x0f, 0x42};
};

}  // namespace radray
