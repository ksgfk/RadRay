#include <radray/runtime/render_framework/mesh_draw_command.h>

#include <algorithm>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/renderer_list_pass_bindings.h>
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
    return ValidateMeshDrawCommand(command);
}

std::optional<PreparedRendererList> PrepareRendererList(const RendererList& list, RenderGraphRasterBuilder& builder,
                                                        Nullable<const RendererListPassBindings*> bindings) {
    RADRAY_PROFILE_SCOPE_N("PrepareRendererList");
    PreparedRendererList prepared{builder.GetPassHandle(), {}};
    if (!list.Items.empty()) {
        if (list.Items.size() != list.Commands.size()) {
            builder.Reject("RendererListPreparation", "Draw order must reference every command exactly once");
            return std::nullopt;
        }
        vector<bool> visited(list.Commands.size());
        for (const auto& item : list.Items) {
            if (item.CommandIndex >= list.Commands.size() || visited[item.CommandIndex]) {
                builder.Reject("RendererListPreparation", "Draw order contains an invalid or duplicate command index");
                return std::nullopt;
            }
            visited[item.CommandIndex] = true;
        }
    }
    prepared.Draws.reserve(list.Commands.size());
    // Many draws share geometry; declaring the same (buffer, range) read repeatedly only grows the pass
    // access list (and every compile step that walks it), so each distinct read is declared once.
    struct DeclaredRange {
        uint64_t Offset, Size;
        RgBufferAccess Access;
    };
    unordered_map<render::Buffer*, InlineVector<DeclaredRange, 2>> declared;
    uint64_t uniqueReads = 0;
    for (size_t index = 0; index < list.Commands.size(); ++index) {
        const auto& draw = list.GetCommand(index);
        if (!ValidateMeshDrawCommand(draw) || (bindings && !bindings->IsValidFor(builder, *draw.Program))) {
            builder.Reject("RendererListPreparation", "Draw geometry or pass parameter bindings are invalid");
            return std::nullopt;
        }
        const auto declare = [&](render::Buffer& buffer, RgBufferAccess access, render::BufferRange range) {
            auto& ranges = declared[&buffer];
            for (const auto& seen : ranges)
                if (seen.Offset == range.Offset && seen.Size == range.Size && seen.Access == access) return true;
            const auto desc = buffer.GetDesc();
            render::BufferStates state = render::BufferState::HostWrite;
            if (desc.Memory == render::MemoryType::Device) {
                state = render::BufferState::UNKNOWN;
                if (desc.Usage.HasFlag(render::BufferUse::Vertex)) state |= render::BufferState::Vertex;
                if (desc.Usage.HasFlag(render::BufferUse::Index)) state |= render::BufferState::Index;
            }
            if (!builder.ReadImmutableBuffer(buffer, state, access, range).IsValid()) return false;
            ranges.push_back({range.Offset, range.Size, access});
            ++uniqueReads;
            return true;
        };
        for (const auto& vertex : draw.Geometry->VertexBuffers)
            if (!declare(*vertex.View.Target, RgBufferAccess::Vertex, {vertex.View.Offset, vertex.View.Size})) return std::nullopt;
        if (!declare(*draw.Geometry->Ibv.Target, RgBufferAccess::Index, {draw.Geometry->Ibv.Offset, render::BufferRange::All()})) return std::nullopt;
        const auto program = builder.UseGraphicsProgram(*draw.Program, draw.PipelineState, draw.Geometry->VertexLayout, draw.Geometry->Topology);
        if (!program.IsValid()) return std::nullopt;
        prepared.Draws.push_back({&draw, {draw.Groups.data(), draw.Groups.size()}, program, bindings ? bindings->Find(*draw.Program) : std::span<const RendererListPassBinding>{}});
    }
    prepared.UniqueBufferReads = uniqueReads;
    return prepared;
}

void SubmitRendererList(const PreparedRendererList& list, RenderGraphRasterContext& ctx, DrawExecutionStats& stats) {
    RADRAY_PROFILE_SCOPE_N("SubmitRendererList");
    if (list.Pass != ctx.GetPassHandle()) {
        stats.BindingFailure += list.Draws.size();
        stats.Skipped += list.Draws.size();
        ctx.Fail("Prepared renderer list belongs to another graph or pass");
        return;
    }
    auto& commands = ctx.Encoder();
    RgGraphicsProgramHandle lastProgram{};
    std::span<const PreparedShaderGroup> lastNative{};
    std::span<const RendererListPassBinding> lastGraph{};
    bool haveProgram = false;
    const auto sameNative = [](std::span<const PreparedShaderGroup> a, std::span<const PreparedShaderGroup> b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].Group != b[i].Group || a[i].Set.Get() != b[i].Set.Get() || a[i].DynamicOffsets != b[i].DynamicOffsets) return false;
        return true;
    };
    const auto sameGraph = [](std::span<const RendererListPassBinding> a, std::span<const RendererListPassBinding> b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].Program != b[i].Program || a[i].Group != b[i].Group || a[i].Parameters.Index != b[i].Parameters.Index ||
                a[i].Parameters.Generation != b[i].Parameters.Generation)
                return false;
        return true;
    };
    for (const auto& prepared : list.Draws) {
        const auto& draw = *prepared.Description;
        const auto groups = prepared.Groups;
        ++stats.Commands;
        const bool samePso = haveProgram && lastProgram.Index == prepared.Program.Index && lastProgram.Generation == prepared.Program.Generation;
        if (!samePso) {
            ctx.BindGraphicsProgram(prepared.Program);
            lastProgram = prepared.Program;
            haveProgram = true;
            lastNative = {};
            lastGraph = {};
        }
        const auto graphGroups = prepared.GraphGroups;
        if (!(samePso && sameNative(groups, lastNative) && sameGraph(graphGroups, lastGraph))) {
            size_t nativeIndex = 0, graphIndex = 0;
            while (nativeIndex < groups.size() || graphIndex < graphGroups.size()) {
                if (graphIndex == graphGroups.size() || (nativeIndex < groups.size() && groups[nativeIndex].Group < graphGroups[graphIndex].Group)) {
                    const auto& group = groups[nativeIndex++];
                    commands.BindPersistentShaderParameterSet(group.Group, group.Set.Get(), group.DynamicOffsets);
                } else {
                    ctx.BindParameterSet(graphGroups[graphIndex++].Parameters);
                }
            }
            lastNative = groups;
            lastGraph = graphGroups;
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
