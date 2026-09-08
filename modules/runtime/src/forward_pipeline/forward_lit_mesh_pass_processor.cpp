#include "forward_lit_mesh_pass_processor.h"
#include "forward_frame.h"

#include <algorithm>

namespace radray::forward_detail {

ForwardLitMeshPassProcessor::ProgramState::ProgramState(ShaderProgram* program, const ForwardProgramBindings* binding)
    : Program(program),
      Binding(binding),
      Layout(&program->GetParameterLayout()),
      ObjectValues(Layout, binding->ObjectGroup),
      LocalToWorld(Layout->Find("ForwardObject.LocalToWorld")),
      NormalToWorld(Layout->Find("ForwardObject.NormalToWorld")),
      PreviousLocalToWorld(Layout->Find("ForwardObject.PreviousLocalToWorld")),
      MotionValid(Layout->Find("ForwardObject.MotionValid")) {}

Nullable<ForwardLitMeshPassProcessor::ProgramState*> ForwardLitMeshPassProcessor::ResolveProgram(ShaderProgram* program) {
    if (program == _lastProgram) return _lastState;
    auto [found, inserted] = _programs.try_emplace(program);
    if (inserted) {
        const auto binding = _bindings.Resolve(program);
        if (binding) found->second = make_unique<ProgramState>(program, binding.Get());
    }
    _lastProgram = program;
    _lastState = found->second.get();
    return _lastState;
}

void ForwardLitMeshPassProcessor::ResetView() noexcept {
    for (auto& [program, state] : _programs) {
        if (!state) continue;
        state->ViewPrepared = false;
        state->View.reset();
        if (state->ViewDependent()) state->Objects.Clear();
    }
}

void ForwardLitMeshPassProcessor::AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                               const MeshBatch& batch, MeshPassDrawListContext& out) {
    const auto& materialData = scene.Materials[batch.Material];
    const auto pass = materialData.FindPass(desc.MaterialPassName);
    if (!pass || !pass->Valid || !pass->Program) {
        out.Reject(MeshPassRejectReason::MissingPass);
        return;
    }
    if (!batch.Geometry || !ValidateMeshGeometry(*batch.Geometry.Get(), batch.FirstIndex, batch.IndexCount)) {
        out.Reject(MeshPassRejectReason::InvalidGeometry);
        return;
    }
    auto* program = pass->Program.Get();
    const auto state = ResolveProgram(program);
    if (!state || pass->ParameterGroup != state->Binding->MaterialGroup) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    auto& ps = *state.Get();
    const auto& binding = *ps.Binding;
    if (!ps.ViewPrepared) {
        ps.ViewPrepared = true;
        ShaderParameterStorage values{ps.Layout, binding.ViewGroup};
        if (FillViewParameters(values, *desc.Culling.Get(), *desc.View.Get(), _lightOverflowWarned,
                               binding.PassGroup.has_value() || desc.MaterialPassName == "DepthNormalsMotion" || desc.MaterialPassName == "ShadowCaster"))
            ps.View = _resources.PrepareGroup(*program, binding.ViewGroup, values);
    }
    auto* material = ps.Materials.Find(batch.Material);
    if (material == nullptr) {
        material = &ps.Materials.Insert(batch.Material);
        *material = _resources.PrepareGroup(*program, binding.MaterialGroup, pass->Parameters, pass->Textures, pass->Samplers);
    }
    auto* object = ps.Objects.Find(batch.Primitive);
    if (object == nullptr) {
        object = &ps.Objects.Insert(batch.Primitive);
        auto& values = ps.ObjectValues;
        const auto& primitive = scene.Primitives[batch.Primitive];
        if (ps.LocalToWorld == nullptr || !values.SetMatrix4x4(*ps.LocalToWorld, primitive.LocalToWorld)) {
            out.Reject(MeshPassRejectReason::InvalidBindings);
            return;
        }
        if (ps.NormalToWorld != nullptr &&
            !values.SetMatrix4x4(*ps.NormalToWorld, MakeNormalToWorld(primitive.LocalToWorld))) {
            out.Reject(MeshPassRejectReason::InvalidBindings);
            return;
        }
        if (ps.PreviousLocalToWorld != nullptr) {
            const auto motion = _temporal ? _temporal->GetPrimitiveMotion(desc.View->StateId, primitive) : PrimitiveMotionData{primitive.LocalToWorld, false};
            if (!values.SetMatrix4x4(*ps.PreviousLocalToWorld, motion.PreviousLocalToWorld) ||
                ps.MotionValid == nullptr ||
                !values.SetUInt(*ps.MotionValid, motion.Valid && desc.View->PreviousViewValid ? 1u : 0u)) {
                out.Reject(MeshPassRejectReason::InvalidBindings);
                return;
            }
        }
        *object = _resources.PrepareGroup(*program, binding.ObjectGroup, values);
    }
    if (!ps.View || !*material || !*object) {
        out.Reject(MeshPassRejectReason::PrepareResourceFailed);
        return;
    }
    MeshDrawCommand command;
    command.Program = program;
    command.PipelineState = pass->PipelineState;
    command.PipelineState.DepthStencil.DepthTestEnable = true;
    command.PipelineState.DepthStencil.DepthCompare = render::CompareFunction::LessEqual;
    command.PipelineState.DepthStencil.DepthWriteEnable = RenderQueueRange::Opaque().Contains(materialData.Queue);
    if (!command.PipelineState.DepthStencil.DepthWriteEnable && command.PipelineState.DepthStencil.Stencil) {
        command.PipelineState.DepthStencil.Stencil->WriteMask = 0;
    }
    command.Geometry = batch.Geometry;
    command.FirstIndex = batch.FirstIndex;
    command.IndexCount = batch.IndexCount;
    command.VertexOffset = batch.VertexOffset;
    // Groups must be ascending by group index (see ValidateMeshDrawCommand); the three forward groups are
    // distinct, so emit them in order here instead of sorting and re-validating geometry per draw.
    const PreparedShaderGroup* groups[3]{&*ps.View, &**material, &**object};
    std::sort(std::begin(groups), std::end(groups), [](const auto* a, const auto* b) { return a->Group < b->Group; });
    for (const auto* group : groups) command.Groups.push_back(*group);
    out.AddCommand(std::move(command));
}

}  // namespace radray::forward_detail
