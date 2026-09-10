#include "depth_only_mesh_pass_processor.h"
#include <radray/runtime/render_framework/cpu_draw_record.h>

namespace radray::forward_detail {

void DepthOnlyMeshPassProcessor::ResetView() noexcept {
    _views.clear();
}

void DepthOnlyMeshPassProcessor::PrepareCommand(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                                 const MeshBatch& batch, const MaterialPassRenderData& pass,
                                                 bool mirrored, MeshPassDrawListContext& out) {
    auto* program = pass.Program.Get();
    const auto binding = _bindings.Resolve(program);
    if (!binding || pass.ParameterGroup) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    auto [view, inserted] = _views.try_emplace(program, std::nullopt);
    if (inserted) {
        // DepthOnly reads only ViewProj, but shares the full view ABI, so the rest uploads as zero.
        _viewScratch = {};
        _viewScratch.ViewProj = desc.View->ViewProjection;
        view->second = _resources.PrepareGroup(*program, binding->ViewGroup, AsCBufferBytes(_viewScratch));
    }
    auto [prepared, newObject] = _objectGroups[program].try_emplace(batch.Primitive, std::nullopt);
    if (newObject) {
        if (batch.Primitive >= _objects.RowCount()) {
            out.Reject(MeshPassRejectReason::InvalidBindings);
            return;
        }
        prepared->second = _resources.PrepareGroup(*program, binding->ObjectGroup, _objects.Row(batch.Primitive));
    }
    const auto& objectGroup = prepared->second;
    if (!view->second || !objectGroup) {
        out.Reject(MeshPassRejectReason::PrepareResourceFailed);
        return;
    }
    MeshDrawCommand command;
    command.Program = program;
    command.PipelineState = pass.PipelineState;
    command.PipelineState.DepthStencil.DepthTestEnable = true;
    command.PipelineState.DepthStencil.DepthWriteEnable = true;
    if (mirrored)
        command.PipelineState.Primitive.FaceClockwise = OppositeFrontFace(command.PipelineState.Primitive.FaceClockwise);
    command.Geometry = batch.Geometry;
    command.FirstIndex = batch.FirstIndex;
    command.IndexCount = batch.IndexCount;
    command.VertexOffset = batch.VertexOffset;
    command.Groups = {*view->second, *objectGroup};
    FinalizeMeshDrawCommand(command);
    out.AddCommand(std::move(command));
}

void DepthOnlyMeshPassProcessor::AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                                const MeshBatch& batch, MeshPassDrawListContext& out) {
    const auto pass = scene.Materials[batch.Material].FindPass(desc.MaterialPassName);
    if (!pass || !pass->Valid || !pass->Program) {
        out.Reject(MeshPassRejectReason::MissingPass);
        return;
    }
    if (!batch.Geometry) {
        out.Reject(MeshPassRejectReason::InvalidGeometry);
        return;
    }
    PrepareCommand(desc, scene, batch, *pass.Get(), IsMirroredAffine(scene.Primitives[batch.Primitive].LocalToWorld), out);
}

void DepthOnlyMeshPassProcessor::PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                                const DrawRecord& record, MeshPassDrawListContext& out) {
    if (record.Batch >= scene.MeshBatches.size() || record.Material >= scene.Materials.size() ||
        record.PassIndex >= scene.Materials[record.Material].Passes.size()) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    const auto& pass = scene.Materials[record.Material].Passes[record.PassIndex];
    if (!pass.Valid || !pass.Program) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    PrepareCommand(desc, scene, scene.MeshBatches[record.Batch], pass, record.Mirrored, out);
}

}  // namespace radray::forward_detail
