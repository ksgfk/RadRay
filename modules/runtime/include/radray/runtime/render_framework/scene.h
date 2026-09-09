#pragma once

#include <span>

#include <radray/types.h>
#include <radray/runtime/render_framework/cpu_draw_record.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>

namespace radray {

/// Game-thread proxy registry and unique long-term CPU render identity.
/// Registration transfers ownership; snapshots retain asset owners separately.
class Scene {
public:
    Scene() = default;
    Scene(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene& operator=(Scene&&) = delete;
    ~Scene() noexcept;

    Nullable<PrimitiveSceneProxy*> AddPrimitive(unique_ptr<PrimitiveSceneProxy> proxy);
    void RemovePrimitive(PrimitiveSceneProxy* proxy) noexcept;
    Nullable<LightSceneProxy*> AddLight(unique_ptr<LightSceneProxy> proxy);
    void RemoveLight(LightSceneProxy* proxy) noexcept;

    SceneObjectId GetPrimitiveId(const PrimitiveSceneProxy* proxy) const noexcept;
    Nullable<PrimitiveSceneProxy*> FindPrimitive(SceneObjectId id) const noexcept;
    SceneObjectId GetLightId(const LightSceneProxy* proxy) const noexcept;
    Nullable<LightSceneProxy*> FindLight(SceneObjectId id) const noexcept;

    std::span<const unique_ptr<PrimitiveSceneProxy>> Primitives() const noexcept { return _primitiveProxies; }
    std::span<const unique_ptr<LightSceneProxy>> Lights() const noexcept { return _lightProxies; }

private:
    struct SlotTable {
        vector<uint32_t> Generation;
        vector<uint32_t> Packed;
        vector<uint32_t> Free;
        uint32_t Allocate();
        void Release(uint32_t slot) noexcept;
    };

    vector<unique_ptr<PrimitiveSceneProxy>> _primitiveProxies;
    vector<SceneObjectId> _primitiveIds;
    unordered_map<const PrimitiveSceneProxy*, SceneObjectId> _primitiveByPointer;
    SlotTable _primitiveSlots;

    vector<unique_ptr<LightSceneProxy>> _lightProxies;
    vector<SceneObjectId> _lightIds;
    unordered_map<const LightSceneProxy*, SceneObjectId> _lightByPointer;
    SlotTable _lightSlots;
};

}  // namespace radray
