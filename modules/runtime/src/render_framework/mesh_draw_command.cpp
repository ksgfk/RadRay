#include <radray/runtime/render_framework/mesh_draw_command.h>

#include <algorithm>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/renderer_list_pass_bindings.h>
#include <radray/logger.h>

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
    return ValidateMeshDrawCommand(command);
}

std::optional<PreparedRendererList> PrepareRendererList(const RendererList& list, RenderGraphRasterBuilder& builder,
                                                       Nullable<const RendererListPassBindings*> bindings) {
    PreparedRendererList prepared{builder.GetPassHandle(), {}};
    prepared.Draws.reserve(list.Commands.size());
    for (const auto& draw : list.Commands) {
        if (!ValidateMeshDrawCommand(draw) || (bindings && !bindings->IsValidFor(builder, *draw.Program))) {
            builder.Reject("RendererListPreparation", "Draw geometry or pass parameter bindings are invalid");
            return std::nullopt;
        }
        const auto program = builder.UseGraphicsProgram(*draw.Program, draw.PipelineState, draw.Geometry->VertexLayout, draw.Geometry->Topology);
        if (!program.IsValid()) return std::nullopt;
        prepared.Draws.push_back({&draw, program, bindings ? bindings->Find(*draw.Program) : std::span<const RendererListPassBinding>{}});
    }
    return prepared;
}

void SubmitRendererList(const PreparedRendererList& list, RenderGraphRasterContext& ctx, DrawExecutionStats& stats) {
    if (list.Pass != ctx.GetPassHandle()) {
        stats.BindingFailure += list.Draws.size();
        stats.Skipped += list.Draws.size();
        ctx.Fail("Prepared renderer list belongs to another graph or pass");
        return;
    }
    auto& commands = ctx.Encoder();
    for (const auto& prepared : list.Draws) {
        const auto& draw = *prepared.Command;
        ++stats.Commands;
        ctx.BindGraphicsProgram(prepared.Program);
        const auto graphGroups = prepared.GraphGroups;
        size_t nativeIndex = 0, graphIndex = 0;
        while (nativeIndex < draw.Groups.size() || graphIndex < graphGroups.size()) {
            if (graphIndex == graphGroups.size() || (nativeIndex < draw.Groups.size() && draw.Groups[nativeIndex].Group < graphGroups[graphIndex].Group)) {
                const auto& group = draw.Groups[nativeIndex++];
                commands.BindPersistentShaderParameterSet(group.Group, group.Set.Get(), group.DynamicOffsets);
            } else {
                ctx.BindParameterSet(graphGroups[graphIndex++].Parameters);
            }
        }
        const auto bindings = std::span{draw.Geometry->VertexBuffers};
        for (size_t first = 0; first < bindings.size();) {
            size_t end = first + 1;
            while (end < bindings.size() && uint64_t{bindings[end - 1].Binding} + 1 == bindings[end].Binding) ++end;
            commands.BindVertexBuffers(bindings.subspan(first, end - first));
            first = end;
        }
        commands.BindIndexBuffer(draw.Geometry->Ibv);
        commands.DrawIndexed(draw.IndexCount, 1, draw.FirstIndex, draw.VertexOffset, 0);
        ++stats.Draws;
    }
}

}  // namespace radray
