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
class RendererListPassBindings;
struct RendererListPassBinding;

// Inline capacities cover the common case (one dynamic buffer per group, view/material/object groups)
// so building a command performs no heap allocation; larger counts spill to the heap transparently.
struct PreparedShaderGroup {
    uint32_t Group{0};
    Nullable<render::ShaderParameterSet*> Set{nullptr};
    InlineVector<render::ShaderParameterDynamicOffset, 2> DynamicOffsets;
};
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
    InlineVector<PreparedShaderGroup, 3> Groups;
};
struct DrawExecutionStats {
    uint64_t Commands{0}, Draws{0}, PsoFailure{0}, BindingFailure{0}, Skipped{0};
    bool Succeeded() const noexcept { return PsoFailure == 0 && BindingFailure == 0 && Skipped == 0; }
};

/// Prepared during graph setup; native graphics PSOs are realized before any pass is recorded.
/// The list and optional graph bindings remain immutable until graph execution finishes.
struct PreparedRendererList {
    struct Draw {
        const MeshDrawDescription* Description;
        std::span<const PreparedShaderGroup> Groups;
        RgGraphicsProgramHandle Program;
        std::span<const RendererListPassBinding> GraphGroups;
    };
    RgPassHandle Pass;
    vector<Draw> Draws;
    uint64_t UniqueBufferReads{0};
};

bool ValidateMeshGeometry(const GpuMesh::DrawData& geometry, uint32_t firstIndex, uint32_t indexCount) noexcept;
bool ValidateMeshDrawCommand(const MeshDrawCommand& command) noexcept;
bool FinalizeMeshDrawCommand(MeshDrawCommand& command) noexcept;
std::optional<PreparedRendererList> PrepareRendererList(const RendererList& list, RenderGraphRasterBuilder& builder,
    Nullable<const RendererListPassBindings*> bindings = nullptr);
void SubmitRendererList(const PreparedRendererList& list, RenderGraphRasterContext& ctx, DrawExecutionStats& stats);

}  // namespace radray
