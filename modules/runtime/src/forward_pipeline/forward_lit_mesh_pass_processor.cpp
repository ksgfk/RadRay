#include "forward_lit_mesh_pass_processor.h"
#include "forward_frame.h"

namespace radray::forward_detail {
namespace {
constexpr size_t kView = static_cast<size_t>(StaticBindingRole::View);
constexpr size_t kMaterial = static_cast<size_t>(StaticBindingRole::Material);
constexpr size_t kObject = static_cast<size_t>(StaticBindingRole::Object);
constexpr size_t kPass = static_cast<size_t>(StaticBindingRole::Pass);
StaticBindingRecipe LegacyRecipe(const ForwardProgramBindings& value) {
    StaticBindingRecipe result;
    result.Groups = {value.ViewGroup, value.MaterialGroup, value.ObjectGroup, value.PassGroup.value_or(UINT32_MAX)};
    std::copy(value.GroupOrder.begin(), value.GroupOrder.end(), result.GroupOrder.begin());
    result.GroupCount = 3;
    result.Valid = true;
    return result;
}
}  // namespace

void ForwardLitMeshPassProcessor::ResetView() noexcept {
    _viewValues = {};
    _viewEpoch = _resources.GetEpoch();
}

FrameDrawBindingId ForwardLitMeshPassProcessor::PrepareBindings(
    const RendererListDesc& desc, const RenderSceneSnapshot& scene, uint32_t materialIndex, uint32_t primitiveIndex,
    const MaterialPassRenderData& pass, const StaticBindingRecipe& binding) {
    if (primitiveIndex >= scene.Primitives.size() || primitiveIndex >= _objects.RowCount() ||
        _objects.Stride() != sizeof(Forward_ObjectData) || binding.GroupCount != 3) return {};
    for (size_t index = 0; index < 3; ++index)
        if (binding.GroupOrder[index] >= 3) return {};
    if (_viewEpoch != _resources.GetEpoch()) ResetView();
    auto& program = *pass.Program.Get();
    const bool separateLighting = binding.Groups[kPass] != UINT32_MAX || desc.MaterialPassName == "DepthNormalsMotion" || desc.MaterialPassName == "ShadowCaster";
    const size_t mode = separateLighting ? 1 : 0;
    if (!_viewValues[mode].Source) {
        FillViewParameters(_viewScratch[mode], *desc.Culling.Get(), *desc.View.Get(), _lightOverflowWarned, separateLighting);
        _viewValues[mode] = _resources.InternValues(&kForwardViewWireIdentity, AsCBufferBytes(_viewScratch[mode]));
    }
    array<FrameShaderGroupId, 3> groups;
    groups[kView] = _resources.PrepareGroupId(program, binding.Groups[kView], _viewValues[mode], 0, AsCBufferBytes(_viewScratch[mode]));
    if (!groups[kView].IsValid()) return {};
    const auto& material = scene.Materials[materialIndex];
    // Authoring materials share numeric/resource values across compatible passes. Hand-built data
    // has no generation guarantee, so its independent pass payloads use independent value sources.
    const void* materialSource = material.Generation ? static_cast<const void*>(&material) : static_cast<const void*>(&pass);
    const FrameCBufferIdentity materialIdentity{materialSource, &kForwardMaterialWireIdentity, 0, material.Generation, material.ValuesRevision, material.BindingsRevision};
    groups[kMaterial] = _resources.PrepareGroupId(program, binding.Groups[kMaterial], materialIdentity, 0, pass.NumericBytes, pass.Textures, pass.Samplers);
    if (!groups[kMaterial].IsValid()) return {};
    FrameCBufferIdentity objectIdentity{&_objects, &kForwardObjectWireIdentity, 0};
    if (_temporal) {
        const auto stamp = _temporal->GetPrimitiveHistoryStamp(desc.View->StateId);
        objectIdentity.Context = desc.View->StateId.Value;
        objectIdentity.Generation = stamp.CommittedSerial;
        objectIdentity.Revision = stamp.InvalidationRevision;
        objectIdentity.HistoryRevision = desc.View->PreviousViewValid ? 1 : 0;
        objectIdentity.HistoryOwner = _temporal.Get();
    }
    std::span<const byte> bytes;
    if (!_resources.HasCBufferValues(objectIdentity, primitiveIndex)) {
        bytes = _objects.Row(primitiveIndex);
        if (_temporal) {
            const auto motion = _temporal->GetPrimitiveMotion(desc.View->StateId, scene.Primitives[primitiveIndex]);
            _objectScratch = *AsCBuffer<Forward_ObjectData>(bytes);
            _objectScratch.PreviousLocalToWorld = motion.PreviousLocalToWorld;
            _objectScratch.MotionValid = motion.Valid && desc.View->PreviousViewValid ? 1u : 0u;
            bytes = AsCBufferBytes(_objectScratch);
        }
    }
    groups[kObject] = _resources.PrepareGroupId(program, binding.Groups[kObject], objectIdentity, primitiveIndex, bytes);
    if (!groups[kObject].IsValid()) return {};
    const array<FrameShaderGroupId, 3> ordered{groups[binding.GroupOrder[0]], groups[binding.GroupOrder[1]], groups[binding.GroupOrder[2]]};
    return _resources.InternBinding(ordered);
}

void ForwardLitMeshPassProcessor::PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
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
    if (!pass.Valid || !pass.Program || !recipe.Valid || pass.ParameterGroup != recipe.Groups[kMaterial]) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    const auto binding = PrepareBindings(desc, scene, record.Material, record.Primitive, pass, recipe);
    if (!binding.IsValid()) {
        out.Reject(MeshPassRejectReason::PrepareResourceFailed);
        return;
    }
    out.AddRecord(_resources, binding);
}

void ForwardLitMeshPassProcessor::AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                               const MeshBatch& batch, MeshPassDrawListContext& out) {
    if (batch.Material >= scene.Materials.size() || batch.Primitive >= scene.Primitives.size()) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    const auto& material = scene.Materials[batch.Material];
    const auto pass = material.FindPass(desc.MaterialPassName);
    if (!pass || !pass->Valid || !pass->Program) {
        out.Reject(MeshPassRejectReason::MissingPass);
        return;
    }
    if (!batch.Geometry) {
        out.Reject(MeshPassRejectReason::InvalidGeometry);
        return;
    }
    const auto legacy = _bindings.Resolve(pass->Program.Get());
    if (!legacy || pass->ParameterGroup != legacy->MaterialGroup) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    const auto binding = PrepareBindings(desc, scene, batch.Material, batch.Primitive, *pass.Get(), LegacyRecipe(*legacy.Get()));
    if (!binding.IsValid()) {
        out.Reject(MeshPassRejectReason::PrepareResourceFailed);
        return;
    }
    MeshDrawCommand command;
    command.Program = pass->Program;
    command.PipelineState = pass->PipelineState;
    command.PipelineState.DepthStencil.DepthTestEnable = true;
    command.PipelineState.DepthStencil.DepthCompare = render::CompareFunction::LessEqual;
    command.PipelineState.DepthStencil.DepthWriteEnable = desc.Policy != kForwardLitReadOnlyDepthPolicy && RenderQueueRange::Opaque().Contains(material.Queue);
    if (!command.PipelineState.DepthStencil.DepthWriteEnable && command.PipelineState.DepthStencil.Stencil) command.PipelineState.DepthStencil.Stencil->WriteMask = 0;
    if (IsMirroredAffine(scene.Primitives[batch.Primitive].LocalToWorld)) command.PipelineState.Primitive.FaceClockwise = OppositeFrontFace(command.PipelineState.Primitive.FaceClockwise);
    command.Geometry = batch.Geometry;
    command.FirstIndex = batch.FirstIndex;
    command.IndexCount = batch.IndexCount;
    command.VertexOffset = batch.VertexOffset;
    for (const auto group : _resources.GetBinding(binding)) command.Groups.push_back(_resources.GetGroup(group));
    out.AddCommand(std::move(command), &_resources);
}

}  // namespace radray::forward_detail
