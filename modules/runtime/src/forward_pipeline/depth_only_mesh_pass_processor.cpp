#include "depth_only_mesh_pass_processor.h"
#include "forward_frame.h"

namespace radray::forward_detail {

void DepthOnlyMeshPassProcessor::ResetView() noexcept {
    _viewValues = {};
    _viewEpoch = _resources.GetEpoch();
}
FrameDrawBindingId DepthOnlyMeshPassProcessor::PrepareBindings(const RendererListDesc& desc, ShaderProgram& program,
                                                               uint32_t primitive, uint32_t viewGroup, uint32_t objectGroup, bool viewFirst) {
    if (primitive >= _objects.RowCount() || _objects.Stride() != sizeof(Forward_ObjectData)) return {};
    if (_viewEpoch != _resources.GetEpoch()) ResetView();
    if (!_viewValues.Source) {
        _viewScratch = {};
        _viewScratch.ViewProj = desc.View->ViewProjection;
        _viewValues = _resources.InternValues(&kForwardViewWireIdentity, AsCBufferBytes(_viewScratch));
    }
    const auto view = _resources.PrepareGroupId(program, viewGroup, _viewValues, 0, AsCBufferBytes(_viewScratch));
    if (!view.IsValid()) return {};
    const auto object = _resources.PrepareGroupId(program, objectGroup, {&_objects, &kForwardObjectWireIdentity, 0}, primitive, _objects.Row(primitive));
    if (!object.IsValid()) return {};
    const array<FrameShaderGroupId, 2> groups = viewFirst ? array{view, object} : array{object, view};
    return _resources.InternBinding(groups);
}

void DepthOnlyMeshPassProcessor::PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                               const DrawRecord& record, MeshPassDrawListContext& out) {
    if (!record.Policy.IsValid()) {
        MeshPassProcessor::PrepareRecord(desc, scene, record, out);
        return;
    }
    if (record.Policy != desc.Policy || record.Material >= scene.Materials.size() ||
        record.PassIndex >= scene.Materials[record.Material].Passes.size() || record.BindingRecipe >= scene.BindingRecipes.size()) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    const auto& pass = scene.Materials[record.Material].Passes[record.PassIndex];
    const auto& recipe = scene.BindingRecipes[record.BindingRecipe];
    if (!pass.Valid || !pass.Program || pass.ParameterGroup || !recipe.Valid || recipe.GroupCount != 2) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    constexpr auto viewRole = static_cast<size_t>(StaticBindingRole::View);
    constexpr auto objectRole = static_cast<size_t>(StaticBindingRole::Object);
    const auto binding = PrepareBindings(desc, *pass.Program.Get(), record.Primitive, recipe.Groups[viewRole], recipe.Groups[objectRole], recipe.GroupOrder[0] == viewRole);
    if (!binding.IsValid()) {
        out.Reject(MeshPassRejectReason::PrepareResourceFailed);
        return;
    }
    out.AddRecord(_resources, binding);
}

void DepthOnlyMeshPassProcessor::AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                              const MeshBatch& batch, MeshPassDrawListContext& out) {
    if (batch.Material >= scene.Materials.size() || batch.Primitive >= scene.Primitives.size()) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    const auto pass = scene.Materials[batch.Material].FindPass(desc.MaterialPassName);
    if (!pass || !pass->Valid || !pass->Program) {
        out.Reject(MeshPassRejectReason::MissingPass);
        return;
    }
    if (!batch.Geometry) {
        out.Reject(MeshPassRejectReason::InvalidGeometry);
        return;
    }
    const auto legacy = _bindings.Resolve(pass->Program.Get());
    if (!legacy || pass->ParameterGroup) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    const auto binding = PrepareBindings(desc, *pass->Program.Get(), batch.Primitive, legacy->ViewGroup, legacy->ObjectGroup, legacy->ViewGroup < legacy->ObjectGroup);
    if (!binding.IsValid()) {
        out.Reject(MeshPassRejectReason::PrepareResourceFailed);
        return;
    }
    MeshDrawCommand command;
    command.Program = pass->Program;
    command.PipelineState = pass->PipelineState;
    command.PipelineState.DepthStencil.DepthTestEnable = true;
    command.PipelineState.DepthStencil.DepthWriteEnable = true;
    if (IsMirroredAffine(scene.Primitives[batch.Primitive].LocalToWorld)) command.PipelineState.Primitive.FaceClockwise = OppositeFrontFace(command.PipelineState.Primitive.FaceClockwise);
    command.Geometry = batch.Geometry;
    command.FirstIndex = batch.FirstIndex;
    command.IndexCount = batch.IndexCount;
    command.VertexOffset = batch.VertexOffset;
    for (const auto group : _resources.GetBinding(binding)) command.Groups.push_back(_resources.GetGroup(group));
    out.AddCommand(std::move(command), &_resources);
}

}  // namespace radray::forward_detail
