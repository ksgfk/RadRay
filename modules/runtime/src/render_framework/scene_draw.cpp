#include <radray/runtime/render_framework/scene_draw.h>
#include <algorithm>
#include <cstring>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/scene_manager.h>
#include <radray/runtime/render_framework/raster_scene.h>

namespace radray {
size_t SceneDrawKeyHash::operator()(const SceneDrawKey& key) const noexcept {
    HashCode hash;
    hash.Add(key.Geometry);
    hash.Add(key.Material);
    return hash.ToHashCode();
}
std::optional<PreparedSceneDraw> SceneDraw::Prepare(
    PipelineContext& context, const SceneViewRequest& view, const SceneGpuView& objects,
    const RenderOutputInfo& output, std::string_view passName,
    Nullable<const MaterialRenderData*> fallback, bool depthTest, Nullable<MaterialGpuState*> fallbackGpu) {
    RADRAY_PROFILE_SCOPE_N("SceneDraw::Prepare");
    const auto resolved = ResolveSceneView(view, output.Width, output.Height, context.Resources.GetDevice()->GetBackend());
    const auto scene = context.Scenes ? context.Scenes->GetSceneRT(view.Scene) : nullptr;
    if (!resolved || !scene) return std::nullopt;
    PreparedSceneDraw result{*resolved, {}, {}, {}};
    struct MaterialInfo {
        const MaterialRenderData* Data;
        Nullable<MaterialGpuState*> Gpu;
        const MaterialPass* Pass;
        const RasterMaterialPlan* Plan;
        vector<const SceneParameterStream*> Streams;
    };
    vector<MaterialInfo> materials;
    unordered_map<const MaterialRenderData*, size_t> materialIndices;
    // All uniforms, layouts, viewport/scissor and scene/view/pass scope are fixed by one Prepare.
    // Geometry identity covers immutable vertex/index streams and topology; section ranges stay explicit.
    struct BatchKey {
        const GpuMesh::DrawData* Geometry;
        size_t Material;
        uint32_t Count, First;
        int32_t VertexOffset;
        uint32_t SortGroup, Order;
        uint64_t Domain;
        bool Reverse, Explicit;
        bool operator==(const BatchKey&) const = default;
    };
    struct BatchHash {
        size_t operator()(const BatchKey& key) const noexcept {
            HashCode hash;
            hash.Add(key.Geometry);
            hash.Add(key.Material);
            hash.Add(key.Count);
            hash.Add(key.First);
            hash.Add(key.VertexOffset);
            hash.Add(key.SortGroup);
            hash.Add(key.Order);
            hash.Add(key.Domain);
            hash.Add(key.Reverse);
            hash.Add(key.Explicit);
            return hash.ToHashCode();
        }
    };
    struct Candidate {
        BatchKey Key;
        uint32_t Slot;
        bool Merge, Reorder;
    };
    vector<Candidate> candidates;
    const auto columns = scene->GetStaticMeshColumns();
    candidates.reserve(columns.Size());
    const auto planes = RasterFrustumPlanes(resolved->ViewProjection);
    {
        RADRAY_PROFILE_SCOPE_N("SceneDraw::CullAndSelect");
        for (size_t row = 0; row < columns.Size(); ++row) {
            const auto id = columns.Ids[row];
            const auto state = scene->GetDrawState(id);
            if (!state || !(state->VisibilityMask & view.VisibilityMask)) {
                ++result.Stats.MaskCulled;
                continue;
            }
            const auto& bounds = columns.Bounds[row];
            if (!RasterBoundsVisible(bounds.Min, bounds.Max, planes)) {
                ++result.Stats.BoundsCulled;
                continue;
            }
            for (const auto& primitive : scene->GetDrawPrimitives(id)) {
                const auto data = primitive.Binding.Data ? primitive.Binding.Data : fallback;
                if (!data) {
                    ++result.Stats.MissingMaterial;
                    continue;
                }
                auto material = materialIndices.find(data.Get());
                if (material == materialIndices.end()) {
                    const auto selected = std::find_if(data->Passes.begin(), data->Passes.end(), [&](const auto& p) { return p.GetDefinition().Name == passName; });
                    if (selected == data->Passes.end()) {
                        materialIndices.emplace(data.Get(), SIZE_MAX);
                        ++result.Stats.MissingPass;
                        continue;
                    }
                    const auto plan = _draw.ResolveMaterial(context.Renderer, *selected);
                    if (!plan || (plan->Push.IsValid() && !plan->SceneIndexOffset)) return std::nullopt;
                    MaterialInfo info{data.Get(), primitive.Binding.Data ? primitive.Binding.Gpu : fallbackGpu, &selected->GetDefinition(), plan.Get(), {}};
                    for (const auto& binding : selected->GetDefinition().Inputs) {
                        if (binding.Source != MaterialInputSource::InstanceParameters) continue;
                        if (binding.ValueIndex >= selected->GetDefinition().ParameterLayouts.size()) return std::nullopt;
                        const auto& layout = selected->GetDefinition().ParameterLayouts[binding.ValueIndex];
                        const auto stream = scene->GetParameterStreams().find(layout.Name);
                        if (stream == scene->GetParameterStreams().end() || stream->second.Layout != layout) return std::nullopt;
                        info.Streams.push_back(&stream->second);
                    }
                    const auto index = materials.size();
                    materials.push_back(std::move(info));
                    material = materialIndices.emplace(data.Get(), index).first;
                }
                if (material->second == SIZE_MAX) {
                    ++result.Stats.MissingPass;
                    continue;
                }
                const auto& info = materials[material->second];
                for (const auto* stream : info.Streams)
                    if (id.Index >= stream->Rows.size() || !stream->Rows[id.Index] || stream->Rows[id.Index]->Id != id) return std::nullopt;
                const bool visible = info.Plan->InstanceAddressing == MaterialInstanceAddressing::VisibleSlots;
                if (state->ExplicitInstancing && !visible) {
                    ++result.Stats.ExplicitUnsupported;
                    continue;
                }
                const bool ordered = info.Pass->Blend.has_value() || info.Pass->PreserveOrder;
                const bool merge = visible && (state->ExplicitInstancing || (info.Pass->AllowAutomaticInstancing && !ordered));
                if (!visible)
                    ++result.Stats.DirectObjects;
                else if (ordered && !state->ExplicitInstancing)
                    ++result.Stats.OrderedObjects;
                else if (!merge)
                    ++result.Stats.AutomaticDisabled;
                const auto& section = *primitive.Section;
                candidates.push_back({{primitive.Draw, material->second, section.IndexCount, section.FirstIndex,
                                       section.VertexOffset, state->SortGroup, state->Order, state->BatchDomain, bounds.ReverseCulling, state->ExplicitInstancing},
                                      id.Index,
                                      merge,
                                      merge && !ordered});
            }
        }
    }
    struct Batch {
        BatchKey Key;
        uint32_t Count{0}, First{0}, Cursor{0};
    };
    vector<Batch> batches;
    vector<size_t> assignment;
    vector<uint32_t> slots;
    {
        RADRAY_PROFILE_SCOPE_N("SceneDraw::BuildBatches");
        const auto before = [](const Candidate& a, const Candidate& b) {
            return a.Key.SortGroup < b.Key.SortGroup || (a.Key.SortGroup == b.Key.SortGroup && a.Key.Order < b.Key.Order);
        };
        if (!std::is_sorted(candidates.begin(), candidates.end(), before)) std::stable_sort(candidates.begin(), candidates.end(), before);
        assignment.reserve(candidates.size());
        unordered_map<BatchKey, size_t, BatchHash> compatible;
        for (const auto& candidate : candidates) {
            if (!batches.empty() && (batches.back().Key.SortGroup != candidate.Key.SortGroup || batches.back().Key.Order != candidate.Key.Order)) compatible.clear();
            size_t target = SIZE_MAX;
            if (candidate.Reorder) {
                const auto found = compatible.find(candidate.Key);
                if (found != compatible.end() && batches[found->second].Count < UINT32_MAX) target = found->second;
            } else {
                compatible.clear();
                if (candidate.Merge && !batches.empty() && batches.back().Key == candidate.Key && batches.back().Count < UINT32_MAX) target = batches.size() - 1;
            }
            if (target == SIZE_MAX) {
                target = batches.size();
                batches.push_back({candidate.Key});
                if (candidate.Reorder) compatible.insert_or_assign(candidate.Key, target);
            }
            ++batches[target].Count;
            assignment.push_back(target);
        }
        uint64_t total = 0;
        for (auto& batch : batches) {
            if (materials[batch.Key.Material].Plan->InstanceAddressing != MaterialInstanceAddressing::VisibleSlots) continue;
            if (total + batch.Count > UINT32_MAX) {
                RADRAY_ERR_LOG("Visible instance list exceeds uint32 addressing");
                return std::nullopt;
            }
            batch.First = static_cast<uint32_t>(total);
            total += batch.Count;
        }
        slots.resize(static_cast<size_t>(total));
        for (size_t i = 0; i < candidates.size(); ++i) {
            auto& batch = batches[assignment[i]];
            if (materials[batch.Key.Material].Plan->InstanceAddressing == MaterialInstanceAddressing::VisibleSlots)
                slots[batch.First + batch.Cursor++] = candidates[i].Slot;
            else
                batch.First = candidates[i].Slot;
        }
    }
    if (batches.empty()) return result;
    RasterMaterialValues values{.Objects = objects.Objects, .Parameters = objects.Parameters};
    RasterFrameBindingCache sharedGroups;
    values.SharedGroups = &sharedGroups;
    vector<render::ShaderBufferBinding> viewBuffers;
    {
        RADRAY_PROFILE_SCOPE_N("SceneDraw::PrepareBindings");
        auto viewAllocation = context.Resources.AllocateConstants(64);
        if (!viewAllocation.IsValid()) return std::nullopt;
        std::memcpy(viewAllocation.Data(), resolved->ViewProjection.data(), 64);
        const auto viewSlice = viewAllocation.Commit(64);
        viewBuffers.push_back({viewSlice.Target, {viewSlice.Offset, viewSlice.Size}, 0});
        values.ViewBuffers = viewBuffers;
        if (!slots.empty()) {
            const uint64_t size = uint64_t{slots.size()} * 4;
            auto allocation = context.Resources.AllocateStorage(size);
            if (!allocation.IsValid()) return std::nullopt;
            std::memcpy(allocation.Data(), slots.data(), static_cast<size_t>(size));
            const auto slice = allocation.Commit(size);
            values.VisibleSlots = render::ShaderBufferBinding{slice.Target, {slice.Offset, slice.Size}, 4};
        }
        result.Materials.reserve(materials.size());
        for (const auto& info : materials) {
            auto prepared = _draw.PrepareMaterial(context, *info.Plan, *info.Data, values, info.Gpu, info.Pass);
            if (!prepared) return std::nullopt;
            result.Materials.push_back(std::move(*prepared));
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("SceneDraw::PrepareDraws");
        result.Draws.reserve(batches.size());
        // Different section ranges share a pipeline, while retaining different batch keys above.
        struct Geometry {
            array<render::GraphicsPipelineState*, 2> Pipelines{};
        };
        unordered_map<SceneDrawKey, Geometry, SceneDrawKeyHash> geometry;
        for (const auto& batch : batches) {
            const auto& info = materials[batch.Key.Material];
            const auto& draw = *batch.Key.Geometry;
            auto& pipeline = geometry[{&draw, info.Data}].Pipelines[batch.Key.Reverse ? 1 : 0];
            if (!pipeline) {
                auto state = *info.Pass;
                if (!state.DepthStencil && output.Depth) {
                    state.DepthStencil = render::DepthStencilState::Default();
                    state.DepthStencil->DepthTestEnable = depthTest;
                    state.DepthStencil->DepthWriteEnable = depthTest;
                }
                const auto prepared = _draw.PreparePipeline(context, *info.Plan, state, draw.Layout, draw.Topology, output, batch.Key.Reverse);
                if (!prepared) return std::nullopt;
                pipeline = prepared.Get();
            }
            PreparedRasterDraw prepared;
            prepared.Pipeline = pipeline;
            prepared.Vertices = draw.VertexBuffers;
            prepared.Indices = draw.Ibv;
            prepared.Count = batch.Key.Count;
            prepared.First = batch.Key.First;
            prepared.VertexOffset = batch.Key.VertexOffset;
            prepared.Instances = batch.Count;
            prepared.FirstInstance = 0;
            prepared.Viewport = resolved->Viewport;
            prepared.Scissor = resolved->Scissor;
            if (info.Plan->Push.IsValid()) {
                prepared.Push = info.Pass->ScenePushValues;
                if (prepared.Push.empty()) prepared.Push.resize(info.Plan->PushSize);
                std::memcpy(prepared.Push.data() + *info.Plan->SceneIndexOffset, &batch.First, 4);
            }
            result.Draws.push_back({std::move(prepared), batch.Key.Material});
        }
    }
    return result;
}

void PreparedSceneDraw::Record(render::GraphicsCommandEncoder* encoder) const {
    RADRAY_PROFILE_SCOPE_N("SceneDraw::Draw");
    for (const auto& item : Draws) item.Draw.Record(encoder, Materials[item.Material]);
}

UnlitRenderPipeline::UnlitRenderPipeline(ScenePipelineDescriptor descriptor) : _descriptor(std::move(descriptor)) {}
PipelineRecordResult UnlitRenderPipeline::Record(PipelineContext& context, const RenderPipelineRequest& request) {
    PipelineRecordResult result;
    if (!request.Output) return result;
    const auto info = ValidateRenderOutput(*request.Output, *context.Resources.GetDevice());
    if (!info) {
        result.Status = PipelineRecordStatus::RecoverableFailure;
        return result;
    }
    result.OutputState = InitialRenderOutputState(*request.Output);
    vector<render::RenderPassColorAttachment> colors;
    for (const auto& color : request.Output->Colors) colors.push_back({color.View, render::LoadAction::Clear, render::StoreAction::Store, _descriptor.ClearColor});
    std::optional<render::RenderPassDepthStencilAttachment> depth;
    if (request.Output->Depth) depth = render::RenderPassDepthStencilAttachment{request.Output->Depth->View, render::LoadAction::Clear, render::StoreAction::Store, render::LoadAction::Clear, render::StoreAction::Store, false, {1, 0}};
    vector<PreparedSceneDraw> prepared;
    for (const auto& view : request.Views) {
        const auto objects = context.Scenes ? context.Scenes->PrepareSceneGpuRT(view.Scene, context) : std::nullopt;
        if (!objects) {
            const auto scene = context.Scenes ? context.Scenes->GetSceneRT(view.Scene) : nullptr;
            if (scene && !scene->GetStaticMeshes().empty()) result.Status = PipelineRecordStatus::RecoverableFailure;
            continue;
        }
        auto draw = _draw.Prepare(context, view, *objects, *info, _descriptor.PassName,
                                  _descriptor.Fallback ? Nullable<const MaterialRenderData*>{&*_descriptor.Fallback} : nullptr, _descriptor.DepthTest, &_fallbackGpu);
        if (!draw) {
            result.Status = PipelineRecordStatus::RecoverableFailure;
            continue;
        }
        if (draw->Stats.ExplicitUnsupported) {
            RADRAY_ERR_LOG("Scene pass {} does not support {} explicit instances", _descriptor.PassName, draw->Stats.ExplicitUnsupported);
            result.Status = PipelineRecordStatus::RecoverableFailure;
        }
        prepared.push_back(std::move(*draw));
    }
    auto* command = context.Commands.Allocate();
    TransitionRenderOutput(command, *request.Output, *result.OutputState, false);
    auto encoder = command->BeginRenderPass({colors, depth, info->Width, info->Height, 1, {}, "Scene pipeline"});
    if (encoder) {
        for (const auto& draw : prepared) draw.Record(encoder.Get());
        command->EndRenderPass(encoder.Release());
    } else
        result.Status = PipelineRecordStatus::RecoverableFailure;
    TransitionRenderOutput(command, *request.Output, *result.OutputState, true);
    context.Commands.Return(std::span{&command, 1});
    result.Commands.push_back(command);
    if (result.Status != PipelineRecordStatus::RecoverableFailure) result.Status = prepared.empty() ? PipelineRecordStatus::NoWork : PipelineRecordStatus::Recorded;
    return result;
}
}  // namespace radray
