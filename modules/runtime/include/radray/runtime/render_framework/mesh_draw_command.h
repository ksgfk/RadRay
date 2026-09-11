#pragma once

#include <radray/inline_vector.h>
#include <radray/runtime/material_state.h>
#include <radray/runtime/render_framework/mesh_batch.h>
#include <radray/runtime/render_framework/render_types.h>
#include <radray/runtime/render_framework/render_graph.h>

namespace radray {

class ShaderProgram;
struct GraphicsPassState;
struct RendererList;
class RenderGraphRasterContext;
class RenderGraphPrepareContext;
class RendererListPassSets;

struct DrawSortData {
    RenderQueue Queue{RenderQueue::Geometry};
    uint32_t ProgramFrameId{0};
    RenderMaterialIndex Material{0};
    float ViewDepth{0};
    RenderPrimitiveIndex Primitive{0};
    MeshBatchIndex Batch{0};
};
/// Pipeline and geometry inputs without frame bindings or graph handles. Referenced objects must outlive use.
struct MeshDrawDescription {
    Nullable<ShaderProgram*> Program{nullptr};
    MaterialPipelineState PipelineState;
    Nullable<const GpuMesh::DrawData*> Geometry{nullptr};
    uint32_t FirstIndex{0}, IndexCount{0};
    int32_t VertexOffset{0};
};
/// A description and its frame-local bindings. Groups are immutable until the flight retires.
struct MeshDrawCommand : MeshDrawDescription {
    // Native groups reference only persistent read-only resources retained through the flight fence.
    // The inline capacity covers the common view/material/object triple without heap allocation.
    InlineVector<PreparedShaderGroup, 3> Groups;
};
struct DrawExecutionStats {
    uint64_t Commands{0}, Draws{0}, PsoFailure{0}, BindingFailure{0}, Skipped{0};
    bool Succeeded() const noexcept { return PsoFailure == 0 && BindingFailure == 0 && Skipped == 0; }
};

/// Built during the prepare stage of its pass: pipeline states and parameter sets are already
/// resolved, so recording only binds and draws. Immutable until graph execution finishes.
struct PreparedRendererList {
    struct Draw {
        const MeshDrawDescription* Description;
        std::span<const PreparedShaderGroup> Groups;
        std::span<const PreparedShaderGroup> PassGroups;
        render::GraphicsPipelineState* Pipeline;
    };
    RgPassHandle Pass;
    vector<Draw> Draws;
};

bool ValidateMeshGeometry(const GpuMesh::DrawData& geometry, uint32_t firstIndex, uint32_t indexCount) noexcept;
bool ValidateMeshDrawCommand(const MeshDrawCommand& command) noexcept;
bool FinalizeMeshDrawCommand(MeshDrawCommand& command) noexcept;
std::optional<PreparedRendererList> PrepareRendererList(const RendererList& list, RenderGraphPrepareContext& ctx,
                                                        Nullable<const RendererListPassSets*> passSets = nullptr);
void RecordRendererList(const PreparedRendererList& list, RenderGraphRasterContext& ctx, DrawExecutionStats& stats);

}  // namespace radray
