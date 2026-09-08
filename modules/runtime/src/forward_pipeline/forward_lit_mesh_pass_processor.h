#pragma once

#include "forward_bindings.h"
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>
#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray::forward_detail {

/// One processor serves renderer lists built from a single RenderSceneSnapshot within one frame:
/// material and primitive preparations are keyed by snapshot indices.
class ForwardLitMeshPassProcessor final : public MeshPassProcessor {
public:
    ForwardLitMeshPassProcessor(FrameDrawResources& resources, ForwardBindingCache& bindings, bool& lightOverflowWarned,
                                Nullable<const RenderPipelineContext*> temporal = nullptr)
        : _resources(resources), _bindings(bindings), _lightOverflowWarned(lightOverflowWarned), _temporal(temporal) {}
    void AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                      const MeshBatch& batch, MeshPassDrawListContext& out) override;

    // Call before reusing this processor for a list whose view differs from the previous one.
    // Drops view-group preparations and object preparations that depend on the view (motion vectors);
    // material groups and view-independent object groups are kept.
    void ResetView() noexcept;

private:
    static constexpr uint32_t kNoSlot = std::numeric_limits<uint32_t>::max();
    // Snapshot index -> slot in a dense group array; kNoSlot means not yet prepared.
    struct SlotTable {
        vector<uint32_t> Slots;
        vector<std::optional<PreparedShaderGroup>> Groups;
        std::optional<PreparedShaderGroup>* Find(uint32_t index) noexcept {
            return index < Slots.size() && Slots[index] != kNoSlot ? &Groups[Slots[index]] : nullptr;
        }
        std::optional<PreparedShaderGroup>& Insert(uint32_t index) {
            if (index >= Slots.size()) Slots.resize(size_t{index} + 1, kNoSlot);
            Slots[index] = static_cast<uint32_t>(Groups.size());
            return Groups.emplace_back();
        }
        void Clear() noexcept {
            std::fill(Slots.begin(), Slots.end(), kNoSlot);
            Groups.clear();
        }
    };
    struct ProgramState {
        ProgramState(ShaderProgram* program, const ForwardProgramBindings* binding);
        ShaderProgram* Program;
        const ForwardProgramBindings* Binding;
        const ShaderParameterLayout* Layout;
        bool ViewPrepared{false};
        std::optional<PreparedShaderGroup> View;
        SlotTable Materials;
        ShaderParameterStorage ObjectValues;
        const ShaderParameterInfo* LocalToWorld{nullptr};
        const ShaderParameterInfo* NormalToWorld{nullptr};
        const ShaderParameterInfo* PreviousLocalToWorld{nullptr};
        const ShaderParameterInfo* MotionValid{nullptr};
        SlotTable Objects;
        bool ViewDependent() const noexcept { return PreviousLocalToWorld != nullptr; }
    };
    Nullable<ProgramState*> ResolveProgram(ShaderProgram* program);

    FrameDrawResources& _resources;
    ForwardBindingCache& _bindings;
    bool& _lightOverflowWarned;
    Nullable<const RenderPipelineContext*> _temporal;
    // Consecutive batches usually share a program; the last resolution short-circuits the map lookup.
    unordered_map<ShaderProgram*, unique_ptr<ProgramState>> _programs;
    ShaderProgram* _lastProgram{nullptr};
    ProgramState* _lastState{nullptr};
};

}  // namespace radray::forward_detail
