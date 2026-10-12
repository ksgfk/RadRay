#include <radray/runtime/render_framework/scene_draw.h>
#include <algorithm>
#include <cstring>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/scene_manager.h>

namespace radray {
size_t SceneDrawKeyHash::operator()(const SceneDrawKey& key) const noexcept {
    HashCode hash;
    hash.Add(key.Geometry);
    hash.Add(key.Material);
    return hash.ToHashCode();
}
std::optional<PreparedSceneDraw> SceneDraw::Prepare(PipelineContext& context, const SceneViewRequest& view, const SceneGpuView& objects,
    render::RenderPass* pass, const RenderOutputInfo& output, std::string_view passName, Nullable<const MaterialRenderData*> fallback, bool depthTest) {
    RADRAY_PROFILE_SCOPE_N("SceneDraw::Prepare");
    const auto resolved = ResolveSceneView(view, output.Width, output.Height, context.Resources.GetDevice()->GetBackend());
    auto scene = context.Scenes ? context.Scenes->GetSceneRT(view.Scene) : nullptr;
    if (!resolved || !scene) return std::nullopt;
    PreparedSceneDraw result{*resolved, {}, {}};
    const std::span<const byte> viewBytes = std::as_bytes(std::span{resolved->ViewProjection.data(), size_t{16}});
    const std::span<const byte> views[]{viewBytes};
    RasterMaterialValues values{.Views = views, .Objects = objects.Objects};
    struct PreparedMaterial { size_t Index; const RasterMaterialPlan* Plan; const MaterialPass* Pass; };
    unordered_map<const MaterialRenderData*, PreparedMaterial> materials;
    struct Geometry { array<render::GraphicsPipelineState*, 2> Pipelines; size_t Material; };
    unordered_map<SceneDrawKey, Geometry, SceneDrawKeyHash> geometryBindings;
    const auto columns = scene->GetStaticMeshColumns();
    for (size_t row = 0; row < columns.Size(); ++row) {
        const auto object = columns.Get(row);
        const auto mesh = object.Mesh.GetRenderMesh();
        if (!mesh) continue;
        for (const auto& section : object.Mesh.GetSections()) {
            if (!section.IndexCount || section.PrimitiveIndex >= mesh->Draws.size()) continue;
            const auto data = section.MaterialSlot < object.Materials.size() ? object.Materials[section.MaterialSlot].Data : fallback;
            if (!data) { RADRAY_ERR_LOG("missing material slot {}", section.MaterialSlot); continue; }
            const auto& draw = mesh->Draws[section.PrimitiveIndex];
            const SceneDrawKey key{&draw, data.Get()};
            auto geometry = geometryBindings.find(key);
            if (geometry == geometryBindings.end()) {
                auto material = materials.find(data.Get());
                if (material == materials.end()) {
                    const auto impl = std::find_if(data->Passes.begin(), data->Passes.end(), [&](const auto& p) { return p.Name == passName; });
                    if (impl == data->Passes.end()) { RADRAY_ERR_LOG("material has no pass {}", passName); continue; }
                    auto plan = _draw.ResolveMaterial(context.Renderer, *impl);
                    if (!plan) return std::nullopt;
                    auto prepared = _draw.PrepareMaterial(context, *plan.Get(), *data.Get(), values);
                    if (!prepared) return std::nullopt;
                    const auto index = result.Materials.size();
                    result.Materials.push_back(std::move(*prepared));
                    material = materials.emplace(data.Get(), PreparedMaterial{index, plan.Get(), &*impl}).first;
                }
                auto state = *material->second.Pass;
                if (!state.DepthStencil && output.Depth) {
                    state.DepthStencil = render::DepthStencilState::Default();
                    state.DepthStencil->DepthTestEnable = depthTest;
                    state.DepthStencil->DepthWriteEnable = depthTest;
                }
                Geometry binding{{}, material->second.Index};
                for (uint32_t reverse = 0; reverse < 2; ++reverse) {
                    auto pipeline = _draw.PreparePipeline(context, *material->second.Plan, state, draw.Layout, draw.Topology, pass, output, reverse != 0);
                    if (!pipeline) return std::nullopt;
                    binding.Pipelines[reverse] = pipeline.Get();
                }
                geometry = geometryBindings.emplace(key, binding).first;
            }
            PreparedRasterDraw prepared;
            prepared.Pipeline = geometry->second.Pipelines[object.ReverseCulling ? 1 : 0];
            prepared.Vertices = draw.VertexBuffers;
            prepared.Indices = draw.Ibv;
            prepared.Count = section.IndexCount;
            prepared.First = section.FirstIndex;
            prepared.VertexOffset = section.VertexOffset;
            prepared.Viewport = resolved->Viewport;
            prepared.Scissor = resolved->Scissor;
            if (result.Materials[geometry->second.Material].Push.IsValid()) {
                const uint32_t slot = columns.Ids[row].Index;
                const auto bytes = std::as_bytes(std::span{&slot, 1});
                prepared.Push.assign(bytes.begin(), bytes.end());
            }
            result.Draws.push_back({std::move(prepared), geometry->second.Material});
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
    vector<render::RenderPassColorAttachmentDescriptor> colors;
    vector<render::TextureView*> views;
    vector<render::ColorClearValue> clear;
    for (size_t i = 0; i < info->Colors.size(); ++i) {
        colors.push_back({info->Colors[i], info->Samples, render::LoadAction::Clear, render::StoreAction::Store});
        views.push_back(request.Output->Colors[i].View);
        clear.push_back(_descriptor.ClearColor);
    }
    std::optional<render::RenderPassDepthStencilAttachmentDescriptor> depth;
    std::optional<render::DepthStencilClearValue> clearDepth;
    if (info->Depth) {
        depth = render::RenderPassDepthStencilAttachmentDescriptor{*info->Depth, info->Samples, render::LoadAction::Clear, render::StoreAction::Store, render::LoadAction::Clear, render::StoreAction::Store};
        clearDepth = render::DepthStencilClearValue{1, 0};
    }
    auto* registry = context.Renderer.GetRenderPassRegistry();
    auto pass = registry->GetOrCreateRenderPass({colors, depth});
    if (!pass) {
        result.Status = PipelineRecordStatus::RecoverableFailure;
        return result;
    }
    auto framebuffer = registry->GetOrCreateFramebuffer({pass.Get(), views, request.Output->Depth ? request.Output->Depth->View : nullptr, info->Width, info->Height, 1});
    if (!framebuffer) {
        result.Status = PipelineRecordStatus::RecoverableFailure;
        return result;
    }
    vector<PreparedSceneDraw> prepared;
    for (const auto& view : request.Views) {
        const auto objects = context.Scenes ? context.Scenes->PrepareSceneGpuRT(view.Scene, context, result.Commands) : std::nullopt;
        if (!objects) {
            const auto scene = context.Scenes ? context.Scenes->GetSceneRT(view.Scene) : nullptr;
            if (scene && !scene->GetStaticMeshes().empty()) result.Status = PipelineRecordStatus::RecoverableFailure;
            continue;
        }
        auto draw = _draw.Prepare(context, view, *objects, pass.Get(), *info, _descriptor.PassName,
                                  _descriptor.Fallback ? Nullable<const MaterialRenderData*>{&*_descriptor.Fallback} : nullptr, _descriptor.DepthTest);
        if (!draw) {
            result.Status = PipelineRecordStatus::RecoverableFailure;
            continue;
        }
        prepared.push_back(std::move(*draw));
    }
    auto* command = context.Commands.Allocate();
    TransitionRenderOutput(command, *request.Output, *result.OutputState, false);
    auto encoder = command->BeginRenderPass({pass.Get(), framebuffer.Get(), clear, clearDepth, "Scene pipeline"});
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
