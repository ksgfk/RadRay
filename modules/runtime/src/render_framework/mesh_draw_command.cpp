#include <radray/runtime/render_framework/mesh_draw_command.h>

#include <algorithm>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/render_graph.h>

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

void SubmitRendererList(const RendererList& list, RenderGraphRasterContext& ctx, const GraphicsPassState& passState, DrawExecutionStats& stats) {
    auto& commands = ctx.Encoder();
    for (const auto& draw : list.Commands) {
        ++stats.Commands;
        if (!ValidateMeshDrawCommand(draw)) {
            ++stats.BindingFailure;
            ++stats.Skipped;
            continue;
        }
        const auto pso = draw.Program->GetOrCreateGraphicsPipelineState(draw.PipelineState, draw.Geometry->VertexLayout, draw.Geometry->Topology, passState);
        if (!pso) {
            ++stats.PsoFailure;
            ++stats.Skipped;
            continue;
        }
        commands.BindGraphicsPipelineState(pso.Get());
        for (const auto& group : draw.Groups) commands.BindShaderParameterSet(group.Group, group.Set.Get(), group.DynamicOffsets);
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
