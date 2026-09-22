#pragma once

#include <radray/enum_flags.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/render_scene/scene_capture.h>

namespace radray {

enum class RenderDirtyFlag : uint8_t {
    State = 1,
    Transform = 2,
    DynamicData = 4,
};
template <>
struct is_flags<RenderDirtyFlag> : std::true_type {};
using RenderDirtyFlags = EnumFlags<RenderDirtyFlag>;
inline auto format_as(RenderDirtyFlag value) noexcept { return EnumFlagBitName(value); }

/// A render source may own multiple ShapeId/LightId entries. The World bridge manages its connection and dirty record.
class RenderComponent : public SceneComponent {
public:
    ~RenderComponent() noexcept override;
    void MarkRenderStateDirty();
    void MarkRenderTransformDirty();
    void MarkRenderDynamicDataDirty();

protected:
    void MarkRenderDirty(RenderDirtyFlag flag);
    SceneId GetRenderSceneId() const noexcept;
    virtual void CreateRenderState(SceneWriter& writer) { (void)writer; }
    virtual void DestroyRenderState(SceneWriter& writer) { (void)writer; }
    virtual void CollectRenderUpdates(SceneCapture& capture, RenderDirtyFlags dirty) {
        (void)capture;
        (void)dirty;
    }

private:
    friend class WorldRenderBridge;
    void NotifyWorldTransformChanged(bool notify, bool renderDirty) final;
    uint32_t _renderIndex{std::numeric_limits<uint32_t>::max()};
    uint32_t _renderQueueIndex{0};
    RenderDirtyFlags _renderDirty;
};

template <>
struct RuntimeTypeTrait<RenderComponent> {
    static constexpr RuntimeTypeId value{0x79c2d741, 0x6e16, 0x4119, 0xb4, 0x2a, 0x49, 0x62, 0x25, 0x3d, 0x50, 0x19};
};

}  // namespace radray
