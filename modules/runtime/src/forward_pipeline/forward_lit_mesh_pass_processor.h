#pragma once

#include "forward_bindings.h"
#include <algorithm>
#include <limits>
#include <radray/runtime/forward_pipeline/gen_forward_cbuffers.h>
#include <radray/runtime/render_framework/cbuffer_view.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>
#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray::forward_detail {

/// One processor serves renderer lists built from a single RenderSceneSnapshot within one frame:
/// material and primitive preparations are keyed by snapshot indices.
class ForwardLitMeshPassProcessor final : public MeshPassProcessor {
public:
    ForwardLitMeshPassProcessor(FrameDrawResources& resources, ForwardBindingCache& bindings, bool& lightOverflowWarned,
                                const PackedCBufferTable& objects, Nullable<const RenderPipelineContext*> temporal = nullptr)
        : _resources(resources), _bindings(bindings), _lightOverflowWarned(lightOverflowWarned), _objects(objects), _temporal(temporal) {}
    void AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                      const MeshBatch& batch, MeshPassDrawListContext& out) override;
    void PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                       const DrawRecord& record, MeshPassDrawListContext& out) override;

    // Call before reusing this processor for a list whose view differs from the previous one.
    // Drops view-group preparations. Temporal object slices are view-dependent and cleared;
    // materials and pass-aware static templates stay for the rest of the frame.
    void ResetView() noexcept;
    uint64_t DuplicateSameFramePreparations() const noexcept { return _duplicatePreparations; }

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
    struct CommandTemplate {
        MeshDrawDescription Description;
        PreparedShaderGroup Material;
        RenderPrimitiveIndex Primitive{0};
    };
    struct ProgramState {
        ProgramState(ShaderProgram* program, const ForwardProgramBindings* binding);
        ShaderProgram* Program;
        const ForwardProgramBindings* Binding;
        const ShaderParameterLayout* Layout;
        bool ViewPrepared{false};
        std::optional<PreparedShaderGroup> View;
        SlotTable Materials;
        SlotTable Objects;
        unordered_map<uint64_t, CommandTemplate> Templates;
    };
    Nullable<ProgramState*> ResolveProgram(ShaderProgram* program);
    void PrepareCommand(const RendererListDesc& desc, const RenderSceneSnapshot& scene, const MeshBatch& batch,
                         const MaterialPassRenderData& pass, RenderQueue queue, bool mirrored,
                         MeshBatchIndex batchIndex, uint32_t passIndex, bool reuseCommand, MeshPassDrawListContext& out);

    FrameDrawResources& _resources;
    ForwardBindingCache& _bindings;
    bool& _lightOverflowWarned;
    // Object rows frozen at PrepareFrame, indexed by snapshot primitive. Only the motion fields are
    // view dependent, so temporal object slices are cleared on ResetView. Pass-aware command templates stay.
    const PackedCBufferTable& _objects;
    Nullable<const RenderPipelineContext*> _temporal;
    // Consecutive batches usually share a program; the last resolution short-circuits the map lookup.
    unordered_map<ShaderProgram*, unique_ptr<ProgramState>> _programs;
    ShaderProgram* _lastProgram{nullptr};
    ProgramState* _lastState{nullptr};
    Forward_ViewData _viewScratch{};
    Forward_ObjectData _objectScratch{};
    uint64_t _duplicatePreparations{0};
};

}  // namespace radray::forward_detail
