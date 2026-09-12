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
class FrameDrawResources;

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
    // Optional identity prepared with the immutable geometry. Legacy mutable layouts leave this empty.
    PrimitiveVertexLayoutId LayoutId{};
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
class PreparedRendererList {
public:
    PreparedRendererList(PreparedRendererList&&) noexcept = default;
    PreparedRendererList& operator=(PreparedRendererList&&) noexcept = default;
    PreparedRendererList(const PreparedRendererList&) = delete;
    PreparedRendererList& operator=(const PreparedRendererList&) = delete;

private:
    friend class FrameDrawResources;
    friend std::optional<PreparedRendererList> PrepareRendererList(const RendererList&, RenderGraphPrepareContext&, Nullable<const RendererListPassSets*>);
    friend void RecordRendererList(const PreparedRendererList&, RenderGraphRasterContext&, DrawExecutionStats&);
    friend struct RenderGraphTestDriver;
    friend struct ReadyWorkspaceTestAccess;
    struct Storage;
    struct Workspace;
    PreparedRendererList(RgPassHandle pass, const RendererList* source, shared_ptr<Storage> storage) noexcept;
    struct Draw {
        render::GraphicsPipelineState* Pipeline;
        uint32_t FirstBinding, BindingCount, Geometry;
        uint32_t IndexCount, FirstIndex;
        int32_t VertexOffset;
        bool BindPipeline, BindGeometry;
    };
    struct GroupReference {
        uint32_t Source, Index;
        friend bool operator==(const GroupReference&, const GroupReference&) = default;
    };
    struct Geometry {
        const GpuMesh::DrawData* Source;
        uint32_t FirstRun, RunCount;
    };
    RgPassHandle Pass;
    const RendererList* Source;
    uint64_t SourceRevision;
    Nullable<const FrameDrawResources*> Resources;
    uint64_t ResourceEpoch;
    Nullable<const MeshDrawCommand*> SourceCommands;
    Nullable<const void*> SourceItems;
    size_t SourceCommandCount, SourceItemCount;
    shared_ptr<const Storage> StorageOwner;
    std::span<const Draw> Draws;
    std::span<const GroupReference> Groups;
    std::span<const PreparedShaderGroup> LocalGroups;
    std::span<const Geometry> Geometries;
    std::span<const std::span<const render::VertexBufferBinding>> VertexRuns;
};

bool ValidateMeshGeometry(const GpuMesh::DrawData& geometry, uint32_t firstIndex, uint32_t indexCount) noexcept;
bool ValidateMeshDrawCommand(const MeshDrawCommand& command) noexcept;
bool FinalizeMeshDrawCommand(MeshDrawCommand& command) noexcept;
std::optional<PreparedRendererList> PrepareRendererList(const RendererList& list, RenderGraphPrepareContext& ctx,
                                                        Nullable<const RendererListPassSets*> passSets = nullptr);
void RecordRendererList(const PreparedRendererList& list, RenderGraphRasterContext& ctx, DrawExecutionStats& stats);

}  // namespace radray
