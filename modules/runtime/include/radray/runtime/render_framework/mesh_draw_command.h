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
// Native groups may reference only persistent read-only resources retained through the flight fence.
struct DrawSortData {
    RenderQueue Queue{RenderQueue::Geometry};
    uint32_t ProgramFrameId{0};
    RenderMaterialIndex Material{0};
    float ViewDepth{0};
    RenderPrimitiveIndex Primitive{0};
    MeshBatchIndex Batch{0};
};
/// Borrowed draw payload. Referenced sets are immutable until the flight retires.
struct MeshDrawCommand {
    Nullable<ShaderProgram*> Program{nullptr};
    MaterialPipelineState PipelineState;
    Nullable<const GpuMesh::DrawData*> Geometry{nullptr};
    uint32_t FirstIndex{0}, IndexCount{0};
    int32_t VertexOffset{0};
    InlineVector<PreparedShaderGroup, 3> Groups;
    DrawSortData SortData;
};
struct DrawExecutionStats {
    uint64_t Commands{0}, Draws{0}, PsoFailure{0}, BindingFailure{0}, Skipped{0};
    bool Succeeded() const noexcept { return PsoFailure == 0 && BindingFailure == 0 && Skipped == 0; }
};

/// Prepared during graph setup; native graphics PSOs are realized before any pass is recorded.
/// The list and optional graph bindings remain immutable until graph execution finishes.
struct PreparedRendererList {
    struct Draw {
        const MeshDrawCommand* Command;
        RgGraphicsProgramHandle Program;
        std::span<const RendererListPassBinding> GraphGroups;
    };
    RgPassHandle Pass;
    vector<Draw> Draws;
};

bool ValidateMeshGeometry(const GpuMesh::DrawData& geometry, uint32_t firstIndex, uint32_t indexCount) noexcept;
bool ValidateMeshDrawCommand(const MeshDrawCommand& command) noexcept;
bool FinalizeMeshDrawCommand(MeshDrawCommand& command) noexcept;
std::optional<PreparedRendererList> PrepareRendererList(const RendererList& list, RenderGraphRasterBuilder& builder,
    Nullable<const RendererListPassBindings*> bindings = nullptr);
void SubmitRendererList(const PreparedRendererList& list, RenderGraphRasterContext& ctx, DrawExecutionStats& stats);

}  // namespace radray
