#include "forward_lit_mesh_pass_processor.h"
#include "forward_frame.h"

namespace radray::forward_detail {

ForwardLitMeshPassProcessor::ObjectPreparation::ObjectPreparation(const ShaderParameterLayout* layout, uint32_t group)
    : Values(layout, group),
      LocalToWorld(layout->Find("ForwardObject.LocalToWorld")),
      NormalToWorld(layout->Find("ForwardObject.NormalToWorld")),
      PreviousLocalToWorld(layout->Find("ForwardObject.PreviousLocalToWorld")),
      MotionValid(layout->Find("ForwardObject.MotionValid")) {}

void ForwardLitMeshPassProcessor::ResetView() noexcept {
    _views.clear();
    for (auto& [program, objects] : _objects) {
        if (objects.ViewDependent()) objects.Groups.clear();
    }
}

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
        if (FillViewParameters(values, *desc.Culling.Get(), *desc.View.Get(), _lightOverflowWarned,
                               binding->PassGroup.has_value() || desc.MaterialPassName == "DepthNormalsMotion" || desc.MaterialPassName == "ShadowCaster"))
            view->second = _resources.PrepareGroup(*program, binding->ViewGroup, values);
    }
    auto [material, newMaterial] = _materials[program].try_emplace(batch.Material, std::nullopt);
    if (newMaterial) material->second = _resources.PrepareGroup(*program, binding->MaterialGroup, pass->Parameters, pass->Textures, pass->Samplers);
    auto [objects, newObjects] = _objects.try_emplace(program, &layout, binding->ObjectGroup);
    auto [prepared, newObject] = objects->second.Groups.try_emplace(batch.Primitive, std::nullopt);
    if (newObject) {
        auto& preparation = objects->second;
        auto& object = preparation.Values;
        const auto& primitive = scene.Primitives[batch.Primitive];
        if (preparation.LocalToWorld == nullptr || !object.SetMatrix4x4(*preparation.LocalToWorld, primitive.LocalToWorld)) {
            out.Reject(MeshPassRejectReason::InvalidBindings);
            return;
        }
        if (preparation.NormalToWorld != nullptr &&
            !object.SetMatrix4x4(*preparation.NormalToWorld, MakeNormalToWorld(primitive.LocalToWorld))) {
            out.Reject(MeshPassRejectReason::InvalidBindings);
            return;
        }
        if (preparation.PreviousLocalToWorld != nullptr) {
            const auto motion = _temporal ? _temporal->GetPrimitiveMotion(desc.View->StateId, primitive) : PrimitiveMotionData{primitive.LocalToWorld, false};
            if (!object.SetMatrix4x4(*preparation.PreviousLocalToWorld, motion.PreviousLocalToWorld) ||
                preparation.MotionValid == nullptr ||
                !object.SetUInt(*preparation.MotionValid, motion.Valid && desc.View->PreviousViewValid ? 1u : 0u)) {
                out.Reject(MeshPassRejectReason::InvalidBindings);
                return;
            }
        }
        prepared->second = _resources.PrepareGroup(*program, binding->ObjectGroup, object);
    }
    const auto& objectGroup = prepared->second;
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
    command.Groups = {*view->second, *material->second, *objectGroup};
    if (!FinalizeMeshDrawCommand(command)) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    out.AddCommand(std::move(command));
}

}  // namespace radray::forward_detail
