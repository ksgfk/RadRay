#include "forward_lit_mesh_pass_processor.h"
#include "forward_frame.h"

namespace radray::forward_detail {

void ForwardLitMeshPassProcessor::AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
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
    if (!binding || pass->ParameterGroup != binding->MaterialGroup) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    const auto& layout = program->GetParameterLayout();
    auto [view, newView] = _views.try_emplace(program, std::nullopt);
    if (newView) {
        ShaderParameterStorage values{&layout, binding->ViewGroup};
        if (FillViewParameters(values, *desc.Culling.Get(), *desc.View.Get(), _lightOverflowWarned))
            view->second = _resources.PrepareGroup(*program, binding->ViewGroup, values);
    }
    auto [material, newMaterial] = _materials.try_emplace(batch.Material, std::nullopt);
    if (newMaterial) material->second = _resources.PrepareGroup(*program, binding->MaterialGroup, pass->Parameters, pass->Textures, pass->Samplers);
    ShaderParameterStorage object{&layout, binding->ObjectGroup};
    if (!object.SetMatrix4x4("ForwardObject.LocalToWorld", scene.Primitives[batch.Primitive].LocalToWorld)) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    auto objectGroup = _resources.PrepareGroup(*program, binding->ObjectGroup, object);
    if (!view->second || !material->second || !objectGroup) {
        out.Reject(MeshPassRejectReason::PrepareResourceFailed);
        return;
    }
    MeshDrawCommand command;
    command.Program = program;
    command.PipelineState = pass->PipelineState;
    command.PipelineState.DepthStencil.DepthTestEnable = true;
    command.PipelineState.DepthStencil.DepthCompare = render::CompareFunction::LessEqual;
    command.PipelineState.DepthStencil.DepthWriteEnable = RenderQueueRange::Opaque().Contains(scene.Materials[batch.Material].Queue);
    if (!command.PipelineState.DepthStencil.DepthWriteEnable && command.PipelineState.DepthStencil.Stencil) {
        command.PipelineState.DepthStencil.Stencil->WriteMask = 0;
    }
    command.Geometry = batch.Geometry;
    command.FirstIndex = batch.FirstIndex;
    command.IndexCount = batch.IndexCount;
    command.VertexOffset = batch.VertexOffset;
    command.Groups = {*view->second, *material->second, std::move(*objectGroup)};
    if (!FinalizeMeshDrawCommand(command)) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    out.AddCommand(std::move(command));
}

}  // namespace radray::forward_detail
