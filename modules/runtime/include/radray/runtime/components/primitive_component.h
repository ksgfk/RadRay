#pragma once

#include <radray/runtime/components/render_component.h>

namespace radray {

/// GT geometry identity; Scene owns the corresponding render-side shape state.
class PrimitiveComponent : public RenderComponent {
public:
    PrimitiveComponent() noexcept = default;
    ~PrimitiveComponent() noexcept override;

    ShapeId GetShapeId() const noexcept { return _shapeId; }

protected:
    virtual void CollectPrimitiveUpdates(ShapeCapture& writer, RenderDirtyFlags dirty) {
        (void)writer;
        (void)dirty;
    }

    virtual void OnRenderStateCreated() {}
    virtual void OnRenderStateDestroyed() {}

private:
    void CreateRenderState(SceneWriter& writer) final;
    void DestroyRenderState(SceneWriter& writer) final;
    void CollectRenderUpdates(SceneCapture& capture, RenderDirtyFlags dirty) final;

    ShapeId _shapeId;
};

template <>
struct RuntimeTypeTrait<PrimitiveComponent> {
    static constexpr RuntimeTypeId value{0xfb11f0d6, 0xc97b, 0x4f3f, 0x98, 0xe3, 0xf5, 0x16, 0x8c, 0xbf, 0x0f, 0x42};
};

}  // namespace radray
