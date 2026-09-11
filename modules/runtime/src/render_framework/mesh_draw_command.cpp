#include <radray/runtime/render_framework/mesh_draw_command.h>

#include <algorithm>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/renderer_list_pass_sets.h>
#include <radray/logger.h>
#include <radray/profiler.h>

namespace radray {

bool ValidateMeshGeometry(const GpuMesh::DrawData& geometry, uint32_t firstIndex, uint32_t indexCount) noexcept {
    const auto& ib = geometry.Ibv;
    if (!indexCount || !ib.Target || (ib.Stride != 2 && ib.Stride != 4) || geometry.VertexBuffers.empty() || geometry.VertexLayout.Buffers.empty()) return false;
    const auto size = ib.Target->GetDesc().Size;
    if (ib.Offset > size || uint64_t{firstIndex} + indexCount > (size - ib.Offset) / ib.Stride) return false;
    for (size_t index = 0; index < geometry.VertexBuffers.size(); ++index) {
        const auto& binding = geometry.VertexBuffers[index];
        if (!binding.View.Target || !binding.View.Size || binding.View.Offset > binding.View.Target->GetDesc().Size ||
            binding.View.Size > binding.View.Target->GetDesc().Size - binding.View.Offset) return false;
        for (size_t earlier = 0; earlier < index; ++earlier)
            if (geometry.VertexBuffers[earlier].Binding == binding.Binding) return false;
    }
    for (const auto& layout : geometry.VertexLayout.Buffers) {
        if (std::none_of(geometry.VertexBuffers.begin(), geometry.VertexBuffers.end(), [&](const auto& value) { return value.Binding == layout.Binding; })) return false;
    }
    return true;
}

bool ValidateMeshDrawCommand(const MeshDrawCommand& command) noexcept {
    if (!command.Program || !command.Geometry || !ValidateMeshGeometry(*command.Geometry.Get(), command.FirstIndex, command.IndexCount)) return false;
    std::optional<uint32_t> previous;
    for (const auto& group : command.Groups) {
        if (!group.Set || (previous && group.Group <= *previous)) return false;
        previous = group.Group;
    }
    return true;
}

bool FinalizeMeshDrawCommand(MeshDrawCommand& command) noexcept {
    std::sort(command.Groups.begin(), command.Groups.end(), [](const auto& a, const auto& b) { return a.Group < b.Group; });
    return true;
}

namespace {

bool ValidatePreparedDraws(const RendererList& list, RenderGraphPrepareContext& ctx) {
    RADRAY_PROFILE_SCOPE_N("ValidatePreparedDraws");
    if (!list.Items.empty()) {
        if (list.Items.size() != list.Commands.size()) {
            ctx.Reject("RendererListPreparation", "Draw order must reference every command exactly once");
            return false;
        }
        vector<bool> visited(list.Commands.size());
        for (const auto& item : list.Items) {
            if (item.CommandIndex >= list.Commands.size() || visited[item.CommandIndex]) {
                ctx.Reject("RendererListPreparation", "Draw order contains an invalid or duplicate command index");
                return false;
            }
            visited[item.CommandIndex] = true;
        }
    }
    for (const auto& draw : list.Commands)
        if (!ValidateMeshDrawCommand(draw)) {
            ctx.Reject("RendererListPreparation", "Draw geometry or parameter groups are invalid");
            return false;
        }
    return true;
}

}  // namespace

std::optional<PreparedRendererList> PrepareRendererList(const RendererList& list, RenderGraphPrepareContext& ctx,
                                                        Nullable<const RendererListPassSets*> passSets) {
    RADRAY_PROFILE_SCOPE_N("PrepareRendererList");
    // An argument precondition, so it precedes the content checks and holds at every validation level.
    if (passSets && passSets->GetPass() != ctx.GetPassHandle()) {
        ctx.Reject("RendererListPassSets", "Pass sets were built by another pass, so their views are undeclared here");
        return std::nullopt;
    }
    if (ctx.IsValidationFull() && !ValidatePreparedDraws(list, ctx)) return std::nullopt;
    PreparedRendererList prepared{ctx.GetPassHandle(), {}};
    prepared.Draws.reserve(list.Commands.size());
    // A pipeline is resolved per distinct recipe, not per draw: adjacent draws hit the fast path and
    // the rest fall back to this list, which stays short because a list sorts by program.
    struct Recipe {
        ShaderProgram* Program;
        MaterialPipelineState State;
        const PrimitiveVertexLayout* Layout;
        PrimitiveTopology Topology;
        render::GraphicsPipelineState* Pipeline;
    };
    vector<Recipe> recipes;
    // Geometry is checked once per distinct buffer and access, not per draw.
    InlineVector<std::pair<render::Buffer*, RgBufferAccess>, 8> checked;
    const auto check = [&](render::Buffer* buffer, RgBufferAccess access) {
        for (const auto& seen : checked)
            if (seen.first == buffer && seen.second == access) return true;
        if (!ctx.ValidateGeometryBuffer(buffer, access)) return false;
        checked.push_back({buffer, access});
        return true;
    };
    Nullable<const MeshDrawCommand*> previousDraw{nullptr};
    render::GraphicsPipelineState* previousPipeline{nullptr};
    Nullable<const ShaderProgram*> setsProgram{nullptr};
    std::span<const PreparedShaderGroup> programSets;
    for (size_t index = 0; index < list.Commands.size(); ++index) {
        const auto& draw = list.GetCommand(index);
        if (!draw.Program || !draw.Geometry) {
            ctx.Reject("RendererListPreparation", "Draw requires a shader program and geometry");
            return std::nullopt;
        }
        if (passSets && setsProgram.Get() != draw.Program.Get()) {
            setsProgram = draw.Program.Get();
            programSets = passSets->Find(*draw.Program);
        }
        const bool sameGeometry = previousDraw && previousDraw->Geometry.Get() == draw.Geometry.Get();
        if (!sameGeometry) {
            for (const auto& vertex : draw.Geometry->VertexBuffers)
                if (!check(vertex.View.Target, RgBufferAccess::Vertex)) return std::nullopt;
            if (!check(draw.Geometry->Ibv.Target, RgBufferAccess::Index)) return std::nullopt;
        }
        const bool adjacent = sameGeometry && previousDraw->Program.Get() == draw.Program.Get() &&
                              previousDraw->PipelineState == draw.PipelineState &&
                              previousDraw->Geometry->Topology == draw.Geometry->Topology;
        render::GraphicsPipelineState* pipeline{nullptr};
        if (adjacent)
            pipeline = previousPipeline;
        else {
            for (const auto& recipe : recipes) {
                if (recipe.Program == draw.Program.Get() && recipe.State == draw.PipelineState &&
                    recipe.Layout == &draw.Geometry->VertexLayout && recipe.Topology == draw.Geometry->Topology) {
                    pipeline = recipe.Pipeline;
                    break;
                }
            }
            if (pipeline == nullptr) {
                const Nullable<render::GraphicsPipelineState*> resolved = ctx.ResolveGraphicsPipeline(
                    *draw.Program, draw.PipelineState, draw.Geometry->VertexLayout, draw.Geometry->Topology);
                if (!resolved) return std::nullopt;
                pipeline = resolved.Get();
                recipes.push_back({draw.Program.Get(), draw.PipelineState, &draw.Geometry->VertexLayout,
                                   draw.Geometry->Topology, pipeline});
            }
        }
        prepared.Draws.push_back({&draw, {draw.Groups.data(), draw.Groups.size()}, programSets, pipeline});
        previousDraw = &draw;
        previousPipeline = pipeline;
    }
    return prepared;
}

void RecordRendererList(const PreparedRendererList& list, RenderGraphRasterContext& ctx, DrawExecutionStats& stats) {
    RADRAY_PROFILE_SCOPE_N("RecordRendererList");
    if (list.Pass != ctx.GetPassHandle()) {
        stats.BindingFailure += list.Draws.size();
        stats.Skipped += list.Draws.size();
        ctx.Fail("Prepared renderer list belongs to another graph or pass");
        return;
    }
    auto& commands = ctx.Encoder();
    render::GraphicsPipelineState* lastPipeline{nullptr};
    std::span<const PreparedShaderGroup> lastNative{}, lastPass{};
    Nullable<const GpuMesh::DrawData*> lastGeometry{nullptr};
    for (const auto& prepared : list.Draws) {
        const auto& draw = *prepared.Description;
        ++stats.Commands;
        // The list is plain data a caller can build by hand, so a missing pipeline degrades the draw
        // instead of reaching the encoder.
        if (prepared.Pipeline == nullptr) {
            ++stats.PsoFailure;
            ++stats.Skipped;
            continue;
        }
        const bool samePso = prepared.Pipeline == lastPipeline;
        if (!samePso) {
            commands.BindGraphicsPipelineState(prepared.Pipeline);
            lastPipeline = prepared.Pipeline;
            lastNative = {};
            lastPass = {};
        }
        // Both spans are sorted by group, so one merge walk binds them in group order and skips the
        // groups the previous draw already left bound.
        const auto groups = prepared.Groups;
        const auto passGroups = prepared.PassGroups;
        size_t nativeIndex = 0, passIndex = 0;
        bool bound = true;
        while (nativeIndex < groups.size() || passIndex < passGroups.size()) {
            const bool native = passIndex == passGroups.size() ||
                                (nativeIndex < groups.size() && groups[nativeIndex].Group < passGroups[passIndex].Group);
            const auto& group = native ? groups[nativeIndex] : passGroups[passIndex];
            const auto& last = native ? lastNative : lastPass;
            const size_t at = native ? nativeIndex : passIndex;
            const bool unchanged = at < last.size() && group.Group == last[at].Group &&
                                   group.Set.Get() == last[at].Set.Get() && group.DynamicOffsets == last[at].DynamicOffsets;
            if (!unchanged) {
                if (!group.Set) {
                    bound = false;
                    break;
                }
                commands.BindShaderParameterSet(group);
            }
            if (native)
                ++nativeIndex;
            else
                ++passIndex;
        }
        if (!bound) {
            ++stats.BindingFailure;
            ++stats.Skipped;
            lastNative = {};
            lastPass = {};
            continue;
        }
        lastNative = groups;
        lastPass = passGroups;
        if (!samePso || lastGeometry.Get() != draw.Geometry.Get()) {
            const auto bindings = std::span{draw.Geometry->VertexBuffers};
            for (size_t first = 0; first < bindings.size();) {
                size_t end = first + 1;
                while (end < bindings.size() && uint64_t{bindings[end - 1].Binding} + 1 == bindings[end].Binding) ++end;
                commands.BindVertexBuffers(bindings.subspan(first, end - first));
                first = end;
            }
            commands.BindIndexBuffer(draw.Geometry->Ibv);
            lastGeometry = draw.Geometry.Get();
        }
        commands.DrawIndexed(draw.IndexCount, 1, draw.FirstIndex, draw.VertexOffset, 0);
        ++stats.Draws;
    }
}

}  // namespace radray
