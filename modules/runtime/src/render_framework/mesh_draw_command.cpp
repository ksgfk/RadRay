#include <radray/runtime/render_framework/mesh_draw_command.h>

#include <algorithm>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/renderer_list_pass_sets.h>
#include <radray/logger.h>
#include <radray/profiler.h>

#include "renderer_list_validation.h"
#include "renderer_list_preparation.h"

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

struct PipelineRecipe {
    ShaderProgram* Program;
    MaterialPipelineState State;
    PrimitiveVertexLayoutId Layout;
    PrimitiveTopology Topology;
    bool operator==(const PipelineRecipe&) const = default;
};

struct PipelineRecipeHash {
    size_t operator()(const PipelineRecipe& recipe) const noexcept {
        HashCode hash;
        hash.Add(recipe.Program);
        hash.Add(recipe.Layout.Value);
        hash.Add(static_cast<uint32_t>(recipe.Topology));
        const auto& primitive = recipe.State.Primitive;
        hash.Add(static_cast<uint32_t>(primitive.FaceClockwise));
        hash.Add(static_cast<uint32_t>(primitive.Cull));
        hash.Add(static_cast<uint32_t>(primitive.Poly));
        hash.Add(primitive.UnclippedDepth);
        hash.Add(primitive.Conservative);
        const auto& depth = recipe.State.DepthStencil;
        hash.Add(static_cast<uint32_t>(depth.DepthCompare));
        hash.Add(depth.DepthBias.Constant);
        hash.Add(depth.DepthBias.SlopScale);
        hash.Add(depth.DepthBias.Clamp);
        hash.Add(depth.DepthTestEnable);
        hash.Add(depth.DepthWriteEnable);
        hash.Add(depth.Stencil.has_value());
        if (depth.Stencil) {
            const auto addFace = [&](const render::StencilFaceState& face) {
                hash.Add(static_cast<uint32_t>(face.Compare));
                hash.Add(static_cast<uint32_t>(face.FailOp));
                hash.Add(static_cast<uint32_t>(face.DepthFailOp));
                hash.Add(static_cast<uint32_t>(face.PassOp));
            };
            addFace(depth.Stencil->Front);
            addFace(depth.Stencil->Back);
            hash.Add(depth.Stencil->ReadMask);
            hash.Add(depth.Stencil->WriteMask);
        }
        hash.Add(recipe.State.Blend.has_value());
        if (recipe.State.Blend) {
            const auto addBlend = [&](const render::BlendComponent& blend) {
                hash.Add(static_cast<uint32_t>(blend.Src));
                hash.Add(static_cast<uint32_t>(blend.Dst));
                hash.Add(static_cast<uint32_t>(blend.Op));
            };
            addBlend(recipe.State.Blend->Color);
            addBlend(recipe.State.Blend->Alpha);
        }
        hash.Add(recipe.State.WriteMask.value());
        return hash.ToHashCode();
    }
};

size_t GroupHash(const PreparedShaderGroup& group) noexcept {
    HashCode hash;
    hash.Add(group.Group);
    hash.Add(group.Set.Get());
    hash.Add(group.DynamicOffsets.size());
    for (const auto& offset : group.DynamicOffsets) {
        hash.Add(offset.Offset);
    }
    return hash.ToHashCode();
}

bool ValidatePreparedDraws(const void* payload, RenderGraphPrepareContext& ctx) {
    RADRAY_PROFILE_SCOPE_N("ValidatePreparedDraws");
    const auto& source = *static_cast<const RendererListValidationSource*>(payload);
    if (!source.IsCurrent()) {
        ctx.Reject("RendererListLifetime", "Renderer list changed before ready frame validation");
        return false;
    }
    const auto& list = *source.List;
    if (!list.Items.empty()) {
        if (list.Items.size() != list.GetDrawCount()) {
            ctx.Reject("RendererListPreparation", "Draw order must reference every command exactly once");
            return false;
        }
        vector<bool> visited(list.GetDrawCount());
        for (const auto& item : list.Items) {
            if (item.CommandIndex >= list.GetDrawCount() || visited[item.CommandIndex]) {
                ctx.Reject("RendererListPreparation", "Draw order contains an invalid or duplicate command index");
                return false;
            }
            visited[item.CommandIndex] = true;
        }
    }
    unordered_map<render::Buffer*, uint8_t> checked;
    const auto check = [&](render::Buffer* buffer, RgBufferAccess access) {
        const uint8_t bit = access == RgBufferAccess::Vertex ? 1 : 2;
        auto& seen = checked[buffer];
        if (seen & bit) return true;
        seen |= bit;
        return ctx.ValidateGeometryBuffer(buffer, access);
    };
    for (size_t index = 0; index < list.GetDrawCount(); ++index) {
        const auto& draw = list.GetDescription(index);
        if (!draw.Program || !draw.Geometry || !ValidateMeshGeometry(*draw.Geometry, draw.FirstIndex, draw.IndexCount)) {
            ctx.Reject("RendererListPreparation", "Draw geometry or parameter groups are invalid");
            return false;
        }
        const auto groups = list.GetGroups(index);
        std::optional<uint32_t> previous;
        for (size_t at = 0; at < groups.size(); ++at) {
            if (!groups[at].Set || (previous && groups[at].Group <= *previous)) {
                ctx.Reject("RendererListPreparation", "Draw parameter groups must be valid and ordered without duplicates");
                return false;
            }
            previous = groups[at].Group;
        }
        for (const auto& vertex : draw.Geometry->VertexBuffers)
            if (!check(vertex.View.Target, RgBufferAccess::Vertex)) return false;
        if (!check(draw.Geometry->Ibv.Target, RgBufferAccess::Index)) return false;
    }
    return true;
}

}  // namespace

PreparedRendererList::PreparedRendererList(RgPassHandle pass, const RendererList* source, shared_ptr<Storage> storage) noexcept
    : Pass(pass), Source(source), SourceRevision(source->GetBuildRevision()),
      Resources(source->GetFrameResources()), ResourceEpoch(source->GetFrameEpoch()),
      SourceCommands(source->Commands.data()), SourceItems(source->Items.data()),
      SourceCommandCount(source->Commands.size()), SourceItemCount(source->Items.size()),
      StorageOwner(std::move(storage)), Draws(StorageOwner->Draws), Groups(StorageOwner->Groups),
      LocalGroups(std::span<const PreparedShaderGroup>{StorageOwner->LocalGroups}.first(StorageOwner->ActiveLocalGroups)),
      Geometries(StorageOwner->Geometries), VertexRuns(StorageOwner->VertexRuns) {}

std::optional<PreparedRendererList> PrepareRendererList(const RendererList& list, RenderGraphPrepareContext& ctx,
                                                        Nullable<const RendererListPassSets*> passSets) {
    RADRAY_PROFILE_SCOPE_N("PrepareRendererList");
    // An argument precondition, so it precedes the content checks and holds at every validation level.
    if (passSets && passSets->GetPass() != ctx.GetPassHandle()) {
        ctx.Reject("RendererListPassSets", "Pass sets were built by another pass, so their views are undeclared here");
        return std::nullopt;
    }
    if (!list.IsCurrent()) {
        ctx.Reject("RendererListLifetime", "Renderer list belongs to an expired frame or snapshot publication");
        return std::nullopt;
    }
    if (!HasSafeRendererListOrder(list)) {
        ctx.Reject("RendererListPreparation", "Draw order exceeds its source draw array");
        return std::nullopt;
    }
    const auto frameResources = list.GetFrameResources();
    unique_ptr<PreparedRendererList::Workspace> fallback;
    if (!frameResources) fallback = make_unique<PreparedRendererList::Workspace>();
    auto& workspace = frameResources ? frameResources->GetPreparationWorkspace() : *fallback;
    auto storage = workspace.Acquire();
    auto& prepared = *storage;
    prepared.Draws.reserve(list.GetDrawCount());
    auto& pipelines = workspace.Pipelines;
    auto& bindings = workspace.Bindings;
    auto& programs = workspace.Programs;
    auto& geometryLayouts = workspace.GeometryLayouts;
    auto& orderedGroups = workspace.OrderedGroups;
    auto& programGroups = workspace.ProgramGroups;
    auto& nativeScratch = workspace.NativeScratch;
    auto& mergedScratch = workspace.MergedScratch;
    auto& pipelineIndex = workspace.PipelineIndex;
    auto& geometryIndex = workspace.GeometryIndex;
    auto& localGroupIndex = workspace.LocalGroupIndex;
    auto& bindingIndex = workspace.BindingIndex;
    auto& programIndex = workspace.ProgramIndex;
    std::optional<PrimitiveVertexLayoutRegistry> legacyLayouts;
    const auto internGroup = [&](const PreparedShaderGroup& group) {
        const auto hash = GroupHash(group);
        uint32_t id = localGroupIndex.Find(hash, [&](uint32_t at) {
            const auto& other = prepared.LocalGroups[at];
            return group.Group == other.Group && group.Set == other.Set && group.DynamicOffsets == other.DynamicOffsets;
        });
        if (id == UINT32_MAX) {
            id = prepared.AddLocalGroup(group);
            localGroupIndex.Insert(hash, id);
        }
        return PreparedRendererList::GroupReference{0, id};
    };
    const auto groupValue = [&](PreparedRendererList::GroupReference ref) -> const PreparedShaderGroup& {
        return ref.Source == 0 ? prepared.LocalGroups[ref.Index] : frameResources->GetGroup(FrameShaderGroupId{ref.Index});
    };
    uint32_t previousGeometry = UINT32_MAX, previousBinding = UINT32_MAX;
    Nullable<render::GraphicsPipelineState*> previousPipeline{nullptr};
    for (size_t index = 0; index < list.GetDrawCount(); ++index) {
        const auto& draw = list.GetDescription(index);
        if (!draw.Program || !draw.Geometry) {
            ctx.Reject("RendererListPreparation", "Draw requires a shader program and geometry");
            return std::nullopt;
        }
        const auto* geometry = draw.Geometry.Get();
        const auto geometryHash = std::hash<const GpuMesh::DrawData*>{}(geometry);
        uint32_t geometryId = previousGeometry != UINT32_MAX && prepared.Geometries[previousGeometry].Source == geometry
                                  ? previousGeometry
                                  : geometryIndex.Find(geometryHash, [&](uint32_t at) { return prepared.Geometries[at].Source == geometry; });
        if (geometryId == UINT32_MAX) {
            for (const auto& vertex : draw.Geometry->VertexBuffers)
                if (!vertex.View.Target) {
                    ctx.Reject("GeometryBuffer", "Geometry binding requires a non-null buffer");
                    return std::nullopt;
                }
            if (!draw.Geometry->Ibv.Target) {
                ctx.Reject("GeometryBuffer", "Geometry binding requires a non-null buffer");
                return std::nullopt;
            }
            geometryId = static_cast<uint32_t>(prepared.Geometries.size());
            const auto firstRun = static_cast<uint32_t>(prepared.VertexRuns.size());
            const auto vertices = std::span<const render::VertexBufferBinding>{geometry->VertexBuffers};
            const auto runs = list.GetVertexBindingRuns(index);
            if (!runs.empty()) {
                for (const auto& run : runs) {
                    if (run.First > vertices.size() || run.Count > vertices.size() - run.First) {
                        ctx.Reject("GeometryBuffer", "Geometry binding plan exceeds the vertex binding array");
                        return std::nullopt;
                    }
                    prepared.VertexRuns.push_back(vertices.subspan(run.First, run.Count));
                }
            } else {
                for (size_t first = 0; first < vertices.size();) {
                    size_t end = first + 1;
                    while (end < vertices.size() && uint64_t{vertices[end - 1].Binding} + 1 == vertices[end].Binding) ++end;
                    prepared.VertexRuns.push_back(vertices.subspan(first, end - first));
                    first = end;
                }
            }
            prepared.Geometries.push_back({geometry, firstRun, static_cast<uint32_t>(prepared.VertexRuns.size()) - firstRun});
            auto layoutId = draw.LayoutId;
            if (!layoutId.IsValid()) {
                if (!legacyLayouts) legacyLayouts.emplace();
                layoutId = legacyLayouts->Intern(geometry->VertexLayout);
            }
            geometryLayouts.push_back(layoutId);
            geometryIndex.Insert(geometryHash, geometryId);
        }
        const auto& state = list.GetPipelineState(index);
        const auto stateId = list.GetEffectiveStateId(index);
        const auto layout = geometryLayouts[geometryId];
        HashCode stateHash;
        stateHash.Add(draw.Program.Get());
        stateHash.Add(stateId);
        stateHash.Add(layout.Value);
        stateHash.Add(static_cast<uint32_t>(geometry->Topology));
        if (stateId == 0) stateHash.Add(PipelineRecipeHash{}({draw.Program.Get(), state, layout, geometry->Topology}));
        const auto hash = stateHash.ToHashCode();
        uint32_t pipelineId = pipelineIndex.Find(hash, [&](uint32_t at) {
            const auto& entry = pipelines[at];
            return entry.Program == draw.Program.Get() && entry.StateId == stateId && entry.Layout == layout &&
                   entry.Topology == geometry->Topology && (stateId != 0 || *entry.State == state);
        });
        if (pipelineId == UINT32_MAX) {
            const auto resolved = ctx.ResolveGraphicsPipeline(*draw.Program, state, geometry->VertexLayout, geometry->Topology);
            if (!resolved) return std::nullopt;
            pipelineId = static_cast<uint32_t>(pipelines.size());
            pipelines.push_back({draw.Program.Get(), stateId, &state, layout, geometry->Topology, resolved.Get()});
            pipelineIndex.Insert(hash, pipelineId);
        }
        auto* pipeline = pipelines[pipelineId].Pipeline;
        const auto frameBinding = list.GetBindingId(index);
        HashCode bindingHash;
        bindingHash.Add(draw.Program.Get());
        bindingHash.Add(frameBinding.Value);
        if (!frameBinding.IsValid()) {
            nativeScratch.clear();
            const auto groups = list.GetGroups(index);
            for (size_t at = 0; at < groups.size(); ++at) {
                if (!groups[at].Set) {
                    ctx.Reject("RendererListPreparation", "A draw parameter group has no native set");
                    return std::nullopt;
                }
                const auto ref = internGroup(groups[at]);
                nativeScratch.push_back(ref);
                bindingHash.Add(ref.Index);
            }
        }
        // Static tuple IDs already include every native group. Pass groups are fixed for each program.
        const auto tupleHash = bindingHash.ToHashCode();
        uint32_t bindingId = frameBinding.IsValid()
                                 ? bindingIndex.Find(tupleHash, [&](uint32_t at) {
                                       return bindings[at].Program == draw.Program.Get() && bindings[at].FrameBinding == frameBinding;
                                   })
                                 : UINT32_MAX;
        if (bindingId == UINT32_MAX) {
            if (frameBinding.IsValid()) {
                nativeScratch.clear();
                for (const auto id : frameResources->GetBinding(frameBinding)) nativeScratch.push_back({1, id.Value});
            }
            const auto programHash = std::hash<const ShaderProgram*>{}(draw.Program.Get());
            uint32_t programId = programIndex.Find(programHash, [&](uint32_t at) { return programs[at].Program == draw.Program.Get(); });
            if (programId == UINT32_MAX) {
                programId = static_cast<uint32_t>(programs.size());
                const auto first = static_cast<uint32_t>(programGroups.size());
                if (passSets) {
                    for (const auto& group : passSets->Find(*draw.Program)) {
                        if (!group.Set) {
                            ctx.Reject("RendererListPreparation", "A pass parameter group has no native set");
                            return std::nullopt;
                        }
                        programGroups.push_back(internGroup(group));
                    }
                }
                programs.push_back({draw.Program.Get(), first, static_cast<uint32_t>(programGroups.size()) - first});
                programIndex.Insert(programHash, programId);
            }
            const auto& program = programs[programId];
            mergedScratch.clear();
            size_t nativeAt = 0, passAt = 0;
            while (nativeAt < nativeScratch.size() || passAt < program.Count) {
                const bool native = passAt == program.Count || (nativeAt < nativeScratch.size() &&
                                                                groupValue(nativeScratch[nativeAt]).Group < groupValue(programGroups[program.First + passAt]).Group);
                const auto ref = native ? nativeScratch[nativeAt++] : programGroups[program.First + passAt++];
                if (!groupValue(ref).Set) {
                    ctx.Reject("RendererListPreparation", "A parameter group has no native set");
                    return std::nullopt;
                }
                mergedScratch.push_back(ref);
            }
            if (!frameBinding.IsValid()) {
                bindingId = bindingIndex.Find(tupleHash, [&](uint32_t at) {
                    const auto& other = bindings[at];
                    return !other.FrameBinding.IsValid() && other.Program == draw.Program.Get() && other.Count == mergedScratch.size() &&
                           std::equal(mergedScratch.begin(), mergedScratch.end(), orderedGroups.begin() + other.First);
                });
            }
            if (bindingId == UINT32_MAX) {
                bindingId = static_cast<uint32_t>(bindings.size());
                bindings.push_back({draw.Program.Get(), frameBinding, static_cast<uint32_t>(orderedGroups.size()), static_cast<uint32_t>(mergedScratch.size())});
                orderedGroups.insert(orderedGroups.end(), mergedScratch.begin(), mergedScratch.end());
                bindingIndex.Insert(tupleHash, bindingId);
            }
        }
        const bool bindPipeline = previousPipeline.Get() != pipeline;
        const auto firstBinding = static_cast<uint32_t>(prepared.Groups.size());
        if (bindPipeline || bindingId != previousBinding) {
            const auto& row = bindings[bindingId];
            for (uint32_t at = 0; at < row.Count; ++at) {
                const auto ref = orderedGroups[row.First + at];
                const bool unchanged = !bindPipeline && previousBinding != UINT32_MAX && at < bindings[previousBinding].Count &&
                                       ref == orderedGroups[bindings[previousBinding].First + at];
                if (!unchanged) prepared.Groups.push_back(ref);
            }
        }
        prepared.Draws.push_back({pipeline, firstBinding, static_cast<uint32_t>(prepared.Groups.size()) - firstBinding,
                                  geometryId, draw.IndexCount, draw.FirstIndex, draw.VertexOffset,
                                  bindPipeline, bindPipeline || geometryId != previousGeometry});
        previousGeometry = geometryId;
        previousBinding = bindingId;
        previousPipeline = pipeline;
    }
    if (ctx.IsValidationFull()) ctx.DeferReadyValidation(make_shared<RendererListValidationSource>(list), ValidatePreparedDraws);
    return PreparedRendererList{ctx.GetPassHandle(), &list, std::move(storage)};
}

void RecordRendererList(const PreparedRendererList& list, RenderGraphRasterContext& ctx, DrawExecutionStats& stats) {
    RADRAY_PROFILE_SCOPE_N("RecordRendererList");
    if (!list.StorageOwner) {
        ++stats.BindingFailure;
        ctx.Fail("Prepared renderer list was moved from");
        return;
    }
    if (list.Pass != ctx.GetPassHandle()) {
        stats.BindingFailure += list.Draws.size();
        stats.Skipped += list.Draws.size();
        ctx.Fail("Prepared renderer list belongs to another graph or pass");
        return;
    }
    const auto& source = *list.Source;
    if (!source.IsCurrent() || source.GetBuildRevision() != list.SourceRevision ||
        source.GetFrameResources() != list.Resources || source.GetFrameEpoch() != list.ResourceEpoch ||
        source.GetDrawCount() != list.Draws.size() || source.Commands.data() != list.SourceCommands.Get() ||
        source.Items.data() != list.SourceItems.Get() || source.Commands.size() != list.SourceCommandCount ||
        source.Items.size() != list.SourceItemCount) {
        stats.BindingFailure += list.Draws.size();
        stats.Skipped += list.Draws.size();
        ctx.Fail("Prepared renderer list source was reset, republished, or modified before recording");
        return;
    }
    auto& tracked = ctx.Encoder();
    auto& commands = tracked._encoder;
    RenderGraphCommandCalls calls;
    const auto commandCount = list.Draws.size();
    stats.Commands += commandCount;
    const auto resources = list.Resources;
    const Nullable<const PreparedShaderGroup*> groupSources[]{list.LocalGroups.data(), resources ? resources->GetGroups().data() : nullptr};
    for (const auto& prepared : list.Draws) {
        if (prepared.BindPipeline) {
            commands.BindGraphicsPipelineState(prepared.Pipeline);
            ++calls.SetPipeline;
        }
        for (uint32_t at = 0; at < prepared.BindingCount; ++at) {
            const auto ref = list.Groups[prepared.FirstBinding + at];
            const auto& group = groupSources[ref.Source].Get()[ref.Index];
            commands.BindShaderParameterSet(group.Group, group.Set.Get(), group.DynamicOffsets);
            ++calls.SetParameters;
        }
        if (prepared.BindGeometry) {
            const auto& geometry = list.Geometries[prepared.Geometry];
            for (uint32_t at = 0; at < geometry.RunCount; ++at) {
                commands.BindVertexBuffers(list.VertexRuns[geometry.FirstRun + at]);
                ++calls.VertexBuffer;
            }
            commands.BindIndexBuffer(geometry.Source->Ibv);
            ++calls.IndexBuffer;
        }
        commands.DrawIndexed(prepared.IndexCount, 1, prepared.FirstIndex, prepared.VertexOffset, 0);
    }
    calls.DrawIndexed = commandCount;
    stats.Draws += calls.DrawIndexed;
    tracked._calls.Add(calls);
}

}  // namespace radray
