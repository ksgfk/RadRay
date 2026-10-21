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
std::optional<PreparedSceneDraw> SceneDraw::Prepare(
    PipelineContext& context, const SceneViewRequest& view, const SceneGpuView& objects,
    const RenderOutputInfo& output, std::string_view passName,
    std::optional<MaterialId> fallback, bool depthTest) {
    RADRAY_PROFILE_SCOPE_N("SceneDraw::Prepare");
    const auto resolved = ResolveSceneView(view, output.Width, output.Height, context.Resources.GetDevice()->GetBackend());
    const auto scene = context.Scenes ? context.Scenes->GetSceneRT(view.Scene) : nullptr;
    if (!resolved || !scene) return std::nullopt;
    PreparedSceneDraw result{*resolved, {}, {}, {}};
    struct MaterialInfo {
        MaterialId Id;
        TemplateId Template;
        const MaterialTemplateDescriptor* Pass;
        const CompiledMaterialTemplate* Plan;
        vector<const SceneParameterStream*> Streams;
    };
    vector<MaterialInfo> materials;
    struct MaterialBindingHash {
        size_t operator()(const MaterialBinding& binding) const noexcept { return std::hash<const MaterialRenderData*>{}(binding.Data.Get()); }
    };
    unordered_map<MaterialBinding, size_t, MaterialBindingHash> materialIndices;
    // All uniforms, layouts, viewport/scissor and scene/view/pass scope are fixed by one Prepare.
    // Geometry identity covers immutable vertex/index streams and topology; section ranges stay explicit.
    struct BatchKey {
        const GpuMesh::DrawData* Geometry;
        AssetId Mesh;
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
                const auto selection = primitive.Binding.Data ? primitive.Binding : MaterialBinding{};
                if (!selection.Data && !fallback) {
                    ++result.Stats.MissingMaterial;
                    continue;
                }
                auto material = materialIndices.find(selection);
                if (material == materialIndices.end()) {
                    auto materialId = fallback;
                    if (selection.Data) {
                        const auto registered = context.Renderer.GetScenes().Material(context.Scenes->GetRendererSceneKey(view.Scene), selection.Id, *selection.Data);
                        if (!registered) return std::nullopt;
                        materialId = registered->Material;
                    }
                    const CompiledMaterialTemplate* plan = nullptr;
                    TemplateId selected;
                    for (const auto templateId : context.Renderer.GetMaterialTemplates(*materialId)) {
                        const auto candidate = context.Renderer.GetTemplate(templateId);
                        if (candidate && candidate->Descriptor.Name == passName) {
                            plan = candidate.Get();
                            selected = templateId;
                            break;
                        }
                    }
                    if (!plan) {
                        materialIndices.emplace(selection, SIZE_MAX);
                        ++result.Stats.MissingPass;
                        continue;
                    }
                    if (plan->Push.IsValid() && !plan->SceneIndexOffset) return std::nullopt;
                    MaterialInfo info{*materialId, selected, &plan->Descriptor, plan, {}};
                    for (const auto& binding : plan->Descriptor.Inputs) {
                        if (binding.Source != MaterialInputSource::InstanceParameters) continue;
                        if (binding.ValueIndex >= plan->Descriptor.ParameterLayouts.size()) return std::nullopt;
                        const auto& layout = plan->Descriptor.ParameterLayouts[binding.ValueIndex];
                        const auto stream = scene->GetParameterStreams().find(layout.Name);
                        if (stream == scene->GetParameterStreams().end() || stream->second.Layout != layout) return std::nullopt;
                        info.Streams.push_back(&stream->second);
                    }
                    const auto index = materials.size();
                    materials.push_back(std::move(info));
                    material = materialIndices.emplace(selection, index).first;
                }
                if (material->second == SIZE_MAX) {
                    ++result.Stats.MissingPass;
                    continue;
                }
                const auto& info = materials[material->second];
                for (const auto* stream : info.Streams)
                    if (id.Index >= stream->Rows.size() || !stream->Rows[id.Index] || stream->Rows[id.Index]->Id != id) return std::nullopt;
                const bool visible = info.Plan->Descriptor.InstanceAddressing == MaterialInstanceAddressing::VisibleSlots;
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
                candidates.push_back({{primitive.Draw, columns.Bindings[row].MeshAssetId, material->second, section.IndexCount, section.FirstIndex,
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
            if (materials[batch.Key.Material].Plan->Descriptor.InstanceAddressing != MaterialInstanceAddressing::VisibleSlots) continue;
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
            if (materials[batch.Key.Material].Plan->Descriptor.InstanceAddressing == MaterialInstanceAddressing::VisibleSlots)
                slots[batch.First + batch.Cursor++] = candidates[i].Slot;
            else
                batch.First = candidates[i].Slot;
        }
    }
    if (batches.empty()) return result;
    RendererViewDescriptor viewDescriptor;
    const auto viewBytes = std::as_bytes(std::span{resolved->ViewProjection.data(), size_t{16}});
    viewDescriptor.Constants.emplace_back(viewBytes.begin(), viewBytes.end());
    viewDescriptor.VisibleSlots = std::move(slots);
    const auto viewId = context.Builder.AddView(std::move(viewDescriptor));
    if (!viewId) return std::nullopt;
    vector<FramePassId> passes;
    for (const auto& info : materials) {
        const auto pass = context.Builder.AddPass({*viewId, info.Template, output, {}, {}, depthTest});
        if (!pass) return std::nullopt;
        passes.push_back(*pass);
        result.Materials.push_back(info.Id);
    }
    RendererDrawValues values;
    values.Objects = objects.ObjectsResource;
    values.Parameters = objects.ParameterResources;
    for (const auto& batch : batches) {
        const auto& info = materials[batch.Key.Material];
        auto draw = context.Renderer.GetScenes().Geometry(context.Scenes->GetRendererSceneKey(view.Scene), batch.Key.Mesh, *batch.Key.Geometry);
        if (!draw) return std::nullopt;
        draw->Count = batch.Key.Count;
        draw->First = batch.Key.First;
        draw->VertexOffset = batch.Key.VertexOffset;
        draw->Instances = batch.Count;
        draw->Viewport = resolved->Viewport;
        draw->Scissor = resolved->Scissor;
        draw->ReverseCulling = batch.Key.Reverse;
        if (info.Plan->Push.IsValid()) {
            draw->Push = info.Pass->ScenePushValues;
            if (draw->Push.empty()) draw->Push.resize(info.Plan->PushSize);
            std::memcpy(draw->Push.data() + *info.Plan->SceneIndexOffset, &batch.First, 4);
        }
        const auto admitted = context.Builder.AddDraw(passes[batch.Key.Material], info.Id, std::move(*draw), values);
        if (!admitted) return std::nullopt;
        result.Draws.push_back(*admitted);
    }
    return result;
}

bool PreparedSceneDraw::Record(FrameBuilder& frame, render::GraphicsCommandEncoder* encoder) const {
    RADRAY_PROFILE_SCOPE_N("SceneDraw::Draw");
    return frame.Record(encoder, Draws);
}

namespace {
class ScenePipelinePlan final : public RenderPipeline::Plan {
public:
    std::optional<RenderOutput> Output;
    RenderOutputInfo Info;
    ScenePipelineDescriptor Descriptor;
    vector<SceneViewRequest> Views;
    bool Admit(PipelineContext& context) override { return !Descriptor.Fallback || context.Builder.UseMaterial(*Descriptor.Fallback); }
    bool PreparePipeline(PipelineContext& context) override;
    vector<PreparedSceneDraw> Draws;
    PipelineRecordStatus Status{PipelineRecordStatus::NoWork};
    PipelineRecordResult Record(PipelineContext& context) override {
        PipelineRecordResult result{Status};
        if (!Output) return result;
        result.OutputState = InitialRenderOutputState(*Output);
        vector<render::RenderPassColorAttachment> colors;
        for (const auto& color : Output->Colors) colors.push_back({color.View, render::LoadAction::Clear, render::StoreAction::Store, Descriptor.ClearColor});
        std::optional<render::RenderPassDepthStencilAttachment> depth;
        if (Output->Depth) depth = render::RenderPassDepthStencilAttachment{Output->Depth->View, render::LoadAction::Clear, render::StoreAction::Store, render::LoadAction::Clear, render::StoreAction::Store, false, {1, 0}};
        auto* command = context.Commands.Allocate();
        TransitionRenderOutput(command, *Output, *result.OutputState, false);
        auto encoder = command->BeginRenderPass({colors, depth, Info.Width, Info.Height, 1, {}, "Scene pipeline"});
        if (encoder) {
            for (const auto& draw : Draws)
                if (!draw.Record(context.Builder, encoder.Get())) result.Status = PipelineRecordStatus::RecoverableFailure;
            command->EndRenderPass(encoder.Release());
        } else
            result.Status = PipelineRecordStatus::RecoverableFailure;
        TransitionRenderOutput(command, *Output, *result.OutputState, true);
        context.Commands.Return(std::span{&command, 1});
        result.Commands.push_back(command);
        return result;
    }
};
}  // namespace
bool ScenePipelinePlan::PreparePipeline(PipelineContext& context) {
    if (!Output) return true;
    const auto info = ValidateRenderOutput(*Output, *context.Resources.GetDevice());
    if (!info) {
        Status = PipelineRecordStatus::RecoverableFailure;
        return false;
    }
    Info = *info;
    if (context.Scenes && context.Scenes->GetFrameSerial(context.Frame.FlightIndex) != context.Frame.FrameSerial) return false;
    SceneDraw prepare;
    for (const auto& view : Views) {
        const auto scene = context.Scenes ? context.Scenes->GetSceneRT(view.Scene) : nullptr;
        const auto objects = scene ? context.Renderer.GetScenes().Prepare(context.Scenes->GetRendererSceneKey(view.Scene), *scene, context) : std::nullopt;
        if (!objects) {
            if (scene && !scene->GetStaticMeshes().empty()) Status = PipelineRecordStatus::RecoverableFailure;
            continue;
        }
        auto draw = prepare.Prepare(context, view, *objects, *info, Descriptor.PassName, Descriptor.Fallback, Descriptor.DepthTest);
        if (!draw) {
            Status = PipelineRecordStatus::RecoverableFailure;
            continue;
        }
        if (draw->Stats.ExplicitUnsupported) {
            RADRAY_ERR_LOG("Scene pass {} does not support {} explicit instances", Descriptor.PassName, draw->Stats.ExplicitUnsupported);
            Status = PipelineRecordStatus::RecoverableFailure;
        }
        Draws.push_back(std::move(*draw));
    }
    if (Status != PipelineRecordStatus::RecoverableFailure && !Draws.empty()) Status = PipelineRecordStatus::Recorded;
    return Status != PipelineRecordStatus::RecoverableFailure;
}
UnlitRenderPipeline::UnlitRenderPipeline(ScenePipelineDescriptor descriptor) : _descriptor(std::move(descriptor)) {}
unique_ptr<RenderPipeline::Plan> UnlitRenderPipeline::CreatePlan(const RenderPipelineRequest& request) {
    auto result = make_unique<ScenePipelinePlan>();
    result->Output = request.Output;
    result->Descriptor = _descriptor;
    result->Views.assign(request.Views.begin(), request.Views.end());
    return result;
}
}  // namespace radray
