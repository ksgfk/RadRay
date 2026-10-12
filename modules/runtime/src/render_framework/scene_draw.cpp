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
std::span<const SceneDraw::PrimitiveRecord> SceneDraw::GetObjectRecords(SceneId sceneId, const RenderScene& scene, ShapeId id) {
    if (_scene != sceneId) {
        _records.clear();
        _scene = sceneId;
    }
    const auto object = scene.GetStaticMesh(id);
    const auto revision = scene.GetDrawRevision(id);
    if (!object || !revision) {
        if (id.Index < _records.size()) _records[id.Index].reset();
        return {};
    }
    if (id.Index >= _records.size()) _records.resize(size_t{id.Index} + 1);
    auto& record = _records[id.Index];
    if (!record || record->Id != id || record->Geometry != revision->Geometry || record->Materials != revision->Materials) {
        record = make_unique<ObjectRecord>();
        record->Id = id;
        record->Geometry = revision->Geometry;
        record->Materials = revision->Materials;
        const auto mesh = object->Mesh.GetRenderMesh();
        if (mesh)
            for (const auto& section : object->Mesh.GetSections()) {
                if (!section.IndexCount || section.PrimitiveIndex >= mesh->Draws.size()) continue;
                record->Items.push_back({&mesh->Draws[section.PrimitiveIndex], &section,
                                         section.MaterialSlot < object->Materials.size() ? object->Materials[section.MaterialSlot] : MaterialBinding{{}, nullptr}});
            }
    }
    return record->Items;
}
std::optional<PreparedSceneDraw> SceneDraw::Prepare(
    PipelineContext& context,
    const SceneViewRequest& view,
    const SceneGpuView& objects,
    render::RenderPass* pass,
    const RenderOutputInfo& output,
    std::string_view passName,
    Nullable<const MaterialRenderData*> fallback,
    bool depthTest) {
    /**
     * 1.检查对象与视图的 visibility mask。
     * 2.检查 GPU 几何是否可用。
     * 3.遍历网格 section。
     * 4.用 section.MaterialSlot 找到对应材质。
     * 5.从材质中选择 pipeline 要求的 pass，例如 "Unlit"。
     * 6.为该 section 生成一个绘制项。
     */
    RADRAY_PROFILE_SCOPE_N("SceneDraw::Prepare");
    const auto resolved = ResolveSceneView(view, output.Width, output.Height, context.Resources.GetDevice()->GetBackend());
    auto scene = context.Scenes ? context.Scenes->GetSceneRT(view.Scene) : nullptr;
    if (!resolved || !scene) return std::nullopt;
    PreparedSceneDraw result{*resolved, {}, {}};
    const std::span<const byte> viewBytes = std::as_bytes(std::span{resolved->ViewProjection.data(), size_t{16}});
    const std::span<const byte> views[]{viewBytes};
    RasterMaterialValues values{.Views = views, .Objects = objects.Objects, .Parameters = objects.Parameters};
    struct PreparedMaterial {
        size_t Index;
        const RasterMaterialPlan* Plan;
        const MaterialPass* Pass;
    };
    unordered_map<const MaterialRenderData*, PreparedMaterial> materials;
    struct Geometry {
        array<render::GraphicsPipelineState*, 2> Pipelines;
        size_t Material;
    };
    unordered_map<SceneDrawKey, Geometry, SceneDrawKeyHash> geometryBindings;
    const auto columns = scene->GetStaticMeshColumns();
    if (_scene == view.Scene)
        for (auto& record : _records)
            if (record && !scene->ContainsShape(record->Id)) record.reset();
    for (size_t row = 0; row < columns.Size(); ++row) {
        const auto object = columns.Get(row);
        const auto state = scene->GetDrawState(columns.Ids[row]);
        if (!state || !(state->VisibilityMask & view.VisibilityMask)) continue;
        const auto mesh = object.Mesh.GetRenderMesh();
        if (!mesh) continue;
        for (const auto& record : GetObjectRecords(view.Scene, *scene.Get(), columns.Ids[row])) {
            const auto& section = *record.Section;
            const auto data = record.Binding.Data ? record.Binding.Data : fallback;
            if (!data) {
                RADRAY_ERR_LOG("missing material slot {}", section.MaterialSlot);
                continue;
            }
            const auto& draw = mesh->Draws[section.PrimitiveIndex];
            const SceneDrawKey key{&draw, data.Get()};
            auto geometry = geometryBindings.find(key);
            if (geometry == geometryBindings.end()) {
                auto material = materials.find(data.Get());
                if (material == materials.end()) {
                    const auto impl = std::find_if(data->Passes.begin(), data->Passes.end(), [&](const auto& p) { return p.Name == passName; });
                    if (impl == data->Passes.end()) {
                        RADRAY_ERR_LOG("material has no pass {}", passName);
                        continue;
                    }
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
            for (const auto& input : data->Passes)
                for (const auto& binding : input.Inputs) {
                    if (binding.Source != MaterialInputSource::InstanceParameters) continue;
                    if (binding.ValueIndex >= input.ParameterLayouts.size()) return std::nullopt;
                    const auto stream = scene->GetParameterStreams().find(input.ParameterLayouts[binding.ValueIndex].Name);
                    const auto id = columns.Ids[row];
                    if (stream == scene->GetParameterStreams().end() || id.Index >= stream->second.Rows.size() || !stream->second.Rows[id.Index] || stream->second.Rows[id.Index]->Id != id) return std::nullopt;
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
