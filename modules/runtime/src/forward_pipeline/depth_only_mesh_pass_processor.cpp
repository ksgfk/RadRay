#include "depth_only_mesh_pass_processor.h"
#include "forward_frame.h"

namespace radray::forward_detail {

void DepthOnlyMeshPassProcessor::ResetView() noexcept {
    _batchBindings.clear();
    _batchCursor = 0;
    _batchEpoch = 0;
    _viewValues = {};
    _viewDomain = _objectDomain = {};
    if (++_viewRevision == 0) {
        _planGroups.clear();
        ++_viewRevision;
    }
    _viewEpoch = _resources->GetEpoch();
}
FrameDrawBindingId DepthOnlyMeshPassProcessor::PrepareBindings(const RendererListDesc& desc, ShaderProgram& program,
                                                               uint32_t primitive, uint32_t viewGroup, uint32_t objectGroup, bool viewFirst, uint32_t planIndex) {
    if (primitive >= _objects.RowCount() || _objects.Stride() != sizeof(Forward_ObjectData)) return {};
    if (_viewEpoch != _resources->GetEpoch()) ResetView();
    if (!_viewValues.Source) {
        _viewScratch = {};
        _viewScratch.ViewProj = desc.View->ViewProjection;
        _viewValues = _resources->InternValues(&kForwardViewWireIdentity, AsCBufferBytes(_viewScratch));
        _viewDomain = _resources->RegisterParameterDomain(_viewValues);
        _objectDomain = _resources->RegisterParameterDomain({_objects.Identity(), &kForwardObjectWireIdentity, 0});
    }
    Nullable<PlanGroups*> indexed{nullptr};
    if (planIndex != UINT32_MAX) {
        if (_planGroups.size() <= planIndex) _planGroups.resize(size_t{planIndex} + 1);
        indexed = &_planGroups[planIndex];
        if (indexed->ViewRevision != _viewRevision) {
            *indexed = {};
            indexed->ViewRevision = _viewRevision;
            indexed->View = _resources->RegisterParameterGroup(program, viewGroup, _viewDomain);
            indexed->Object = _resources->RegisterParameterGroup(program, objectGroup, _objectDomain);
        }
    }
    const auto view = indexed ? _resources->PrepareIndexedGroup(indexed->View, 0, AsCBufferBytes(_viewScratch))
                              : _resources->PrepareGroupId(program, viewGroup, _viewDomain, 0, AsCBufferBytes(_viewScratch));
    if (!view.IsValid()) return {};
    const auto object = indexed ? _resources->PrepareIndexedGroup(indexed->Object, primitive, _objects.Row(primitive))
                                : _resources->PrepareGroupId(program, objectGroup, _objectDomain, primitive, _objects.Row(primitive));
    if (!object.IsValid()) return {};
    const array<FrameShaderGroupId, 2> groups = viewFirst ? array{view, object} : array{object, view};
    if (indexed) {
        static const byte tupleWire{};
        if (!indexed->Binding.IsValid())
            indexed->Binding = _resources->RegisterParameterDomain({_objects.Identity(), &tupleWire, (uint64_t{_viewDomain.Index} << 32) | planIndex, _objectDomain.Index});
        return _resources->PrepareIndexedBinding(indexed->Binding, primitive, groups);
    }
    return _resources->InternBinding(groups);
}

bool DepthOnlyMeshPassProcessor::PrepareBatch(std::span<const MeshPassListPreparation> batches) {
    if (_viewEpoch != _resources->GetEpoch()) ResetView();
    _batchBindings.clear();
    _batchCursor = 0;
    _batchEpoch = _resources->GetEpoch();
    size_t count = 0;
    for (const auto& batch : batches) {
        if (!batch.ViewDraws.empty() || !batch.Scene->HasPassPolicies) return true;
        count += batch.Candidates.size();
    }
    _batchBindings.reserve(count);
    constexpr auto viewRole = static_cast<size_t>(StaticBindingRole::View);
    constexpr auto objectRole = static_cast<size_t>(StaticBindingRole::Object);
    for (const auto& batch : batches) {
        const auto& scene = *batch.Scene;
        for (const auto index : batch.BindingPlans) {
            const auto& recipe = scene.BindingRecipes[index];
            if (!recipe.Valid || recipe.GroupCount != 2) return false;
        }
        for (const auto& candidate : batch.Candidates) {
            const auto& record = scene.DrawRecords[candidate.Record];
            const auto& pass = scene.Materials[record.Material].Passes[record.PassIndex];
            const auto& recipe = scene.BindingRecipes[record.BindingRecipe];
            const auto binding = PrepareBindings(*batch.Descriptor, *pass.Program, record.Primitive, recipe.Groups[viewRole], recipe.Groups[objectRole], recipe.GroupOrder[0] == viewRole, record.BindingRecipe);
            _batchBindings.push_back({&record, binding, MeshPassRejectReason::PrepareResourceFailed});
        }
    }
    return true;
}

void DepthOnlyMeshPassProcessor::PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                               const DrawRecord& record, MeshPassDrawListContext& out) {
    if (_batchEpoch == _resources->GetEpoch() && _batchCursor < _batchBindings.size() && _batchBindings[_batchCursor].Record == &record) {
        const auto& prepared = _batchBindings[_batchCursor++];
        if (prepared.Binding.IsValid())
            out.AddRecord(*_resources, prepared.Binding);
        else
            out.Reject(prepared.Failure);
        return;
    }
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
    const auto binding = PrepareBindings(desc, *pass.Program.Get(), record.Primitive, recipe.Groups[viewRole], recipe.Groups[objectRole], recipe.GroupOrder[0] == viewRole, record.BindingRecipe);
    if (!binding.IsValid()) {
        out.Reject(MeshPassRejectReason::PrepareResourceFailed);
        return;
    }
    out.AddRecord(*_resources, binding);
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
    const auto legacy = _bindings->Resolve(pass->Program.Get());
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
    for (const auto group : _resources->GetBinding(binding)) command.Groups.push_back(_resources->GetGroup(group));
    out.AddCommand(std::move(command), _resources);
}

}  // namespace radray::forward_detail
