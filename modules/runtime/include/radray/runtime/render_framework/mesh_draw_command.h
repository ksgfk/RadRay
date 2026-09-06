#pragma once

#include <radray/runtime/material_state.h>
#include <radray/runtime/render_framework/mesh_batch.h>
#include <radray/runtime/render_framework/render_types.h>

namespace radray {

class ShaderProgram;
struct GraphicsPassState;
struct RendererList;
class RenderGraphRasterContext;
class RendererListPassBindings;

struct PreparedShaderGroup {
    uint32_t Group{0};
    Nullable<render::ShaderParameterSet*> Set{nullptr};
    vector<render::ShaderParameterDynamicOffset> DynamicOffsets;
};
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
    vector<PreparedShaderGroup> Groups;
    DrawSortData SortData;
};
struct DrawExecutionStats {
    uint64_t Commands{0}, Draws{0}, PsoFailure{0}, BindingFailure{0}, Skipped{0};
    bool Succeeded() const noexcept { return PsoFailure == 0 && BindingFailure == 0 && Skipped == 0; }
};

bool ValidateMeshGeometry(const GpuMesh::DrawData& geometry, uint32_t firstIndex, uint32_t indexCount) noexcept;
bool ValidateMeshDrawCommand(const MeshDrawCommand& command) noexcept;
bool FinalizeMeshDrawCommand(MeshDrawCommand& command) noexcept;
void SubmitRendererList(const RendererList& list, RenderGraphRasterContext& ctx, const GraphicsPassState& passState, DrawExecutionStats& stats);
void SubmitRendererList(const RendererList& list, RenderGraphRasterContext& ctx, const GraphicsPassState& passState,
                        const RendererListPassBindings& bindings, DrawExecutionStats& stats);

}  // namespace radray
