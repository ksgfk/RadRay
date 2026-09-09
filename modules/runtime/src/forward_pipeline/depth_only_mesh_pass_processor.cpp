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
    const auto& layout = program->GetParameterLayout();
    auto [view, inserted] = _views.try_emplace(program, std::nullopt);
    if (inserted) {
        ShaderParameterStorage values{&layout, binding->ViewGroup};
        const bool wrote = binding->ViewProj ? values.SetMatrix4x4(*binding->ViewProj, desc.View->ViewProjection)
                                             : values.SetMatrix4x4("ForwardView.ViewProj", desc.View->ViewProjection);
        if (wrote) view->second = _resources.PrepareGroup(*program, binding->ViewGroup, values);
    }
    auto [objects, newObjects] = _objects.try_emplace(program, &layout, binding->ObjectGroup);
    auto [prepared, newObject] = objects->second.Groups.try_emplace(batch.Primitive, std::nullopt);
    if (newObject) {
        auto& object = objects->second.Values;
        const bool wrote = binding->LocalToWorld
                                ? object.SetMatrix4x4(*binding->LocalToWorld, scene.Primitives[batch.Primitive].LocalToWorld)
                                : object.SetMatrix4x4("ForwardObject.LocalToWorld", scene.Primitives[batch.Primitive].LocalToWorld);
        if (!wrote) {
            out.Reject(MeshPassRejectReason::InvalidBindings);
            return;
        }
        prepared->second = _resources.PrepareGroup(*program, binding->ObjectGroup, object);
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
    if (!FinalizeMeshDrawCommand(command)) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    out.AddCommand(std::move(command));
}

void DepthOnlyMeshPassProcessor::AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                                const MeshBatch& batch, MeshPassDrawListContext& out) {
    const auto pass = scene.Materials[batch.Material].FindPass(desc.MaterialPassName);
    if (!pass || !pass->Valid || !pass->Program) {
        out.Reject(MeshPassRejectReason::MissingPass);
        return;
    }
    if (!batch.Geometry || !ValidateMeshGeometry(*batch.Geometry.Get(), batch.FirstIndex, batch.IndexCount)) {
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
