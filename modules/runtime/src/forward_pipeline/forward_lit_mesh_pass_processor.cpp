#include "forward_lit_mesh_pass_processor.h"
#include "forward_frame.h"

#include <algorithm>
#include <radray/profiler.h>
#include <radray/runtime/render_framework/cpu_draw_record.h>

namespace radray::forward_detail {

ForwardLitMeshPassProcessor::ProgramState::ProgramState(ShaderProgram* program, const ForwardProgramBindings* binding)
    : Program(program),
      Binding(binding),
      Layout(&program->GetParameterLayout()) {}

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
        if (_temporal) {
            state->Objects.Clear();
            state->Templates.Clear();
        }
    }
}

void ForwardLitMeshPassProcessor::PrepareCommand(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                                  const MeshBatch& batch, const MaterialPassRenderData& pass,
                                                  RenderQueue queue, bool mirrored, MeshBatchIndex batchIndex,
                                                  bool reuseCommand, MeshPassDrawListContext& out) {
    RADRAY_PROFILE_SCOPE_N("PrepareCommand");
    auto* program = pass.Program.Get();
    const auto state = ResolveProgram(program);
    if (!state || pass.ParameterGroup != state->Binding->MaterialGroup) {
        out.Reject(MeshPassRejectReason::InvalidBindings);
        return;
    }
    auto& ps = *state.Get();
    const auto& binding = *ps.Binding;
    if (!ps.ViewPrepared) {
        constexpr size_t kInitialCapacityLimit = 1024;
        const auto materialCapacity = std::min(scene.Materials.size(), kInitialCapacityLimit);
        const auto objectCapacity = std::min(scene.Primitives.size(), kInitialCapacityLimit);
        if (ps.Materials.Slots.size() < materialCapacity) ps.Materials.Slots.resize(materialCapacity, kNoSlot);
        ps.Materials.Groups.reserve(materialCapacity);
        if (ps.Objects.Slots.size() < objectCapacity) ps.Objects.Slots.resize(objectCapacity, kNoSlot);
        ps.Objects.Groups.reserve(objectCapacity);
        if (reuseCommand && !_temporal) {
            const auto templateCapacity = std::min(scene.MeshBatches.size(), kInitialCapacityLimit);
            if (ps.Templates.Slots.size() < templateCapacity) ps.Templates.Slots.resize(templateCapacity, kNoSlot);
            ps.Templates.Items.reserve(templateCapacity);
        }
        ps.ViewPrepared = true;
        FillViewParameters(_viewScratch, *desc.Culling.Get(), *desc.View.Get(), _lightOverflowWarned,
                           binding.PassGroup.has_value() || desc.MaterialPassName == "DepthNormalsMotion" || desc.MaterialPassName == "ShadowCaster");
        ps.View = _resources.PrepareGroup(*program, binding.ViewGroup, AsCBufferBytes(_viewScratch));
    }
    const bool cacheCommand = reuseCommand && !_temporal;
    if (cacheCommand) {
        if (auto* tmpl = ps.Templates.Find(batchIndex)) {
            ++_duplicatePreparations;
            if (!ps.View) {
                out.Reject(MeshPassRejectReason::PrepareResourceFailed);
                return;
            }
            MeshDrawCommand command = tmpl->Command;
            if (tmpl->ViewGroupIndex < command.Groups.size()) command.Groups[tmpl->ViewGroupIndex] = *ps.View;
            out.AddCommand(std::move(command));
            return;
        }
    }
    auto* material = ps.Materials.Find(batch.Material);
    if (material == nullptr) {
        material = &ps.Materials.Insert(batch.Material);
        *material = _resources.PrepareGroup(*program, binding.MaterialGroup, pass.NumericBytes, pass.Textures, pass.Samplers);
    } else {
        ++_duplicatePreparations;
    }
    auto* object = ps.Objects.Find(batch.Primitive);
    if (object == nullptr) {
        object = &ps.Objects.Insert(batch.Primitive);
        if (batch.Primitive >= _objects.RowCount()) {
            out.Reject(MeshPassRejectReason::InvalidBindings);
            return;
        }
        // The frozen row already holds the identity motion the non-temporal case wants, so it goes
        // to the arena as is. Only a temporal view has to patch motion, and it pays one extra copy.
        const auto row = _objects.Row(batch.Primitive);
        std::span<const byte> bytes = row;
        if (_temporal) {
            const auto motion = _temporal->GetPrimitiveMotion(desc.View->StateId, scene.Primitives[batch.Primitive]);
            _objectScratch = *AsCBuffer<Forward_ObjectData>(row);
            _objectScratch.PreviousLocalToWorld = motion.PreviousLocalToWorld;
            _objectScratch.MotionValid = motion.Valid && desc.View->PreviousViewValid ? 1u : 0u;
            bytes = AsCBufferBytes(_objectScratch);
        }
        *object = _resources.PrepareGroup(*program, binding.ObjectGroup, bytes);
    } else {
        ++_duplicatePreparations;
    }
    if (!ps.View || !*material || !*object) {
        out.Reject(MeshPassRejectReason::PrepareResourceFailed);
        return;
    }
    MeshDrawCommand command;
    command.Program = program;
    command.PipelineState = pass.PipelineState;
    command.PipelineState.DepthStencil.DepthTestEnable = true;
    command.PipelineState.DepthStencil.DepthCompare = render::CompareFunction::LessEqual;
    command.PipelineState.DepthStencil.DepthWriteEnable = RenderQueueRange::Opaque().Contains(queue);
    if (!command.PipelineState.DepthStencil.DepthWriteEnable && command.PipelineState.DepthStencil.Stencil) {
        command.PipelineState.DepthStencil.Stencil->WriteMask = 0;
    }
    if (mirrored) command.PipelineState.Primitive.FaceClockwise = OppositeFrontFace(command.PipelineState.Primitive.FaceClockwise);
    command.Geometry = batch.Geometry;
    command.FirstIndex = batch.FirstIndex;
    command.IndexCount = batch.IndexCount;
    command.VertexOffset = batch.VertexOffset;
    const PreparedShaderGroup* groups[3]{&*ps.View, &**material, &**object};
    std::sort(std::begin(groups), std::end(groups), [](const auto* a, const auto* b) { return a->Group < b->Group; });
    for (const auto* group : groups) command.Groups.push_back(*group);
    if (cacheCommand) {
        uint32_t viewIndex = 0;
        for (uint32_t index = 0; index < command.Groups.size(); ++index) {
            if (command.Groups[index].Group == binding.ViewGroup) {
                viewIndex = index;
                break;
            }
        }
        auto& stored = ps.Templates.Insert(batchIndex);
        stored.Command = command;
        stored.ViewGroupIndex = viewIndex;
    }
    out.AddCommand(std::move(command));
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
    PrepareCommand(desc, scene, batch, *pass.Get(), materialData.Queue, IsMirroredAffine(scene.Primitives[batch.Primitive].LocalToWorld), 0, false, out);
}

void ForwardLitMeshPassProcessor::PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
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
    PrepareCommand(desc, scene, scene.MeshBatches[record.Batch], pass, record.Queue, record.Mirrored, record.Batch, true, out);
}

}  // namespace radray::forward_detail
