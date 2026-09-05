#include "depth_only_mesh_pass_processor.h"

namespace radray::forward_detail {

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
    auto* program = pass->Program.Get();
    const auto binding = _bindings.Resolve(program);
    if (!binding || pass->ParameterGroup) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    const auto& layout = program->GetParameterLayout();
    auto [view, inserted] = _views.try_emplace(program, std::nullopt);
    if (inserted) {
        ShaderParameterStorage values{&layout, binding->ViewGroup};
        if (values.SetMatrix4x4("ForwardView.ViewProj", desc.View->ViewProjection))
            view->second = _resources.PrepareGroup(*program, binding->ViewGroup, values);
    }
    ShaderParameterStorage object{&layout, binding->ObjectGroup};
    if (!object.SetMatrix4x4("ForwardObject.LocalToWorld", scene.Primitives[batch.Primitive].LocalToWorld)) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    auto objectGroup = _resources.PrepareGroup(*program, binding->ObjectGroup, object);
    if (!view->second || !objectGroup) {
        out.Reject(MeshPassRejectReason::PrepareResourceFailed);
        return;
    }
    MeshDrawCommand command;
    command.Program = program;
    command.PipelineState = pass->PipelineState;
    command.PipelineState.DepthStencil.DepthTestEnable = true;
    command.PipelineState.DepthStencil.DepthWriteEnable = true;
    command.Geometry = batch.Geometry;
    command.FirstIndex = batch.FirstIndex;
    command.IndexCount = batch.IndexCount;
    command.VertexOffset = batch.VertexOffset;
    command.Groups = {*view->second, std::move(*objectGroup)};
    if (!FinalizeMeshDrawCommand(command)) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    out.AddCommand(std::move(command));
}

}  // namespace radray::forward_detail
