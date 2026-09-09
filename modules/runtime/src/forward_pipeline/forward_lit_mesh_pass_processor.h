#pragma once

#include "forward_bindings.h"
#include <algorithm>
#include <limits>
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
    void PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                       const DrawRecord& record, MeshPassDrawListContext& out) override;

    // Call before reusing this processor for a list whose view differs from the previous one.
    // Drops view-group preparations. Object groups and command templates that encode motion
    // (PreviousLocalToWorld with a temporal context) are also dropped; ShadowCaster has no
    // temporal context, so those stay across cascade ResetView.
    void ResetView() noexcept;
    uint64_t DuplicateSameFramePreparations() const noexcept { return _duplicatePreparations; }
    uint64_t ObjectMathComputes() const noexcept { return _objectMathComputes; }

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
        MeshDrawCommand Command;
        uint32_t ViewGroupIndex{0};
    };
    struct TemplateTable {
        vector<uint32_t> Slots;
        vector<CommandTemplate> Items;
        CommandTemplate* Find(uint32_t index) noexcept {
            return index < Slots.size() && Slots[index] != kNoSlot ? &Items[Slots[index]] : nullptr;
        }
        CommandTemplate& Insert(uint32_t index) {
            if (index >= Slots.size()) Slots.resize(size_t{index} + 1, kNoSlot);
            Slots[index] = static_cast<uint32_t>(Items.size());
            return Items.emplace_back();
        }
        void Clear() noexcept {
            std::fill(Slots.begin(), Slots.end(), kNoSlot);
            Items.clear();
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
        TemplateTable Templates;
        bool ViewDependent() const noexcept { return PreviousLocalToWorld != nullptr; }
    };
    Nullable<ProgramState*> ResolveProgram(ShaderProgram* program);
    const Eigen::Matrix4f& CachedNormalToWorld(RenderPrimitiveIndex primitive, const Eigen::Matrix4f& localToWorld);
    void PrepareCommand(const RendererListDesc& desc, const RenderSceneSnapshot& scene, const MeshBatch& batch,
                         const MaterialPassRenderData& pass, RenderQueue queue, bool mirrored,
                         MeshBatchIndex batchIndex, bool reuseCommand, MeshPassDrawListContext& out);

    FrameDrawResources& _resources;
    ForwardBindingCache& _bindings;
    bool& _lightOverflowWarned;
    Nullable<const RenderPipelineContext*> _temporal;
    // Consecutive batches usually share a program; the last resolution short-circuits the map lookup.
    unordered_map<ShaderProgram*, unique_ptr<ProgramState>> _programs;
    ShaderProgram* _lastProgram{nullptr};
    ProgramState* _lastState{nullptr};
    vector<uint8_t> _normalReady;
    vector<Eigen::Matrix4f> _normals;
    uint64_t _duplicatePreparations{0};
    uint64_t _objectMathComputes{0};
};

}  // namespace radray::forward_detail
