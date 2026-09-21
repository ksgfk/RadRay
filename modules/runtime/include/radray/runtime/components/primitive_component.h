#pragma once

#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/render_scene/scene_writer.h>

namespace radray {

/// GT geometry identity; Scene owns the corresponding render-side shape state.
class PrimitiveComponent : public SceneComponent {
public:
    PrimitiveComponent() noexcept { EnableTransformRenderDirty(); }
    ~PrimitiveComponent() noexcept override;

    ShapeId GetShapeId() const noexcept { return _shapeId; }

protected:
    void OnTransformChanged() override;
    virtual void CollectPrimitiveUpdates(SceneWriter& writer, RenderDirtyFlags dirty) {
        (void)writer;
        (void)dirty;
    }

    virtual void OnRenderStateCreated() {}
    virtual void OnRenderStateDestroyed() {}
    SceneId GetRenderSceneId() const noexcept { return _sceneId; }

private:
    void CreateRenderState(SceneWriter& writer) final;
    void DestroyRenderState(SceneWriter& writer) final;
    void CollectRenderUpdates(SceneWriter& writer, RenderDirtyFlags dirty) final;

    ShapeId _shapeId;
    SceneId _sceneId;
};

template <>
struct RuntimeTypeTrait<PrimitiveComponent> {
    static constexpr RuntimeTypeId value{0xfb11f0d6, 0xc97b, 0x4f3f, 0x98, 0xe3, 0xf5, 0x16, 0x8c, 0xbf, 0x0f, 0x42};
};

}  // namespace radray
