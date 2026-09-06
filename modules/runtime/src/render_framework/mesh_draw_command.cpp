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

namespace {
void Submit(const RendererList& list, RenderGraphRasterContext& ctx, const GraphicsPassState& passState,
            Nullable<const RendererListPassBindings*> bindings, DrawExecutionStats& stats) {
    auto& commands = ctx.Encoder();
    for (const auto& draw : list.Commands) {
        ++stats.Commands;
        if (!ValidateMeshDrawCommand(draw) || (bindings && !bindings->IsValidFor(ctx, *draw.Program))) {
            ++stats.BindingFailure;
            ++stats.Skipped;
            RADRAY_ERR_LOG("RendererList binding failure in pass {} for program {} batch {}", ctx.GetPassHandle().Index, draw.SortData.ProgramFrameId, draw.SortData.Batch);
            continue;
        }
        const auto pso = draw.Program->GetOrCreateGraphicsPipelineState(draw.PipelineState, draw.Geometry->VertexLayout, draw.Geometry->Topology, passState);
        if (!pso) {
            ++stats.PsoFailure;
            ++stats.Skipped;
            RADRAY_ERR_LOG("RendererList PSO failure in pass {} for program {} batch {}", ctx.GetPassHandle().Index, draw.SortData.ProgramFrameId, draw.SortData.Batch);
            continue;
        }
        commands.BindGraphicsPipelineState(pso.Get());
        const auto graphGroups = bindings ? bindings->Find(*draw.Program) : std::span<const RendererListPassBinding>{};
        size_t nativeIndex = 0, graphIndex = 0;
        while (nativeIndex < draw.Groups.size() || graphIndex < graphGroups.size()) {
            if (graphIndex == graphGroups.size() || (nativeIndex < draw.Groups.size() && draw.Groups[nativeIndex].Group < graphGroups[graphIndex].Group)) {
                const auto& group = draw.Groups[nativeIndex++];
                commands.BindShaderParameterSet(group.Group, group.Set.Get(), group.DynamicOffsets);
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
}  // namespace

void SubmitRendererList(const RendererList& list, RenderGraphRasterContext& ctx, const GraphicsPassState& passState, DrawExecutionStats& stats) {
    Submit(list, ctx, passState, nullptr, stats);
}

void SubmitRendererList(const RendererList& list, RenderGraphRasterContext& ctx, const GraphicsPassState& passState,
                        const RendererListPassBindings& bindings, DrawExecutionStats& stats) {
    Submit(list, ctx, passState, &bindings, stats);
}

}  // namespace radray
