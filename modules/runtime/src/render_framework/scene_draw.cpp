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
Nullable<const SceneDraw::InputPlan*> SceneDraw::ResolveInputs(ShaderProgram* program, const MaterialPass& pass) {
    for (const auto& plan : _plans)
        if (plan->Program == program && plan->Inputs == pass.Inputs && plan->PushName == pass.ObjectIndexPushConstant) return plan.get();
    auto plan = make_unique<InputPlan>();
    plan->Program = program;
    plan->Inputs = pass.Inputs;
    plan->PushName = pass.ObjectIndexPushConstant;
    plan->Push = program->GetPipelineLayout()->FindBinding(pass.ObjectIndexPushConstant);
    const auto& artifact = program->GetArtifact().Generic();
    const auto push = std::find_if(artifact.RootConstants().begin(), artifact.RootConstants().end(), [&](const auto& block) {
        return artifact.GetName(block.Name) == pass.ObjectIndexPushConstant;
    });
    if (!plan->Push.IsValid() || push == artifact.RootConstants().end() || push->Size != sizeof(uint32_t)) return nullptr;
    for (const auto& input : pass.Inputs) {
        const auto binding = program->GetArtifact().FindBindingInfo(input.Name);
        if (!binding) return nullptr;
        using Kind = shader::ShaderBindingKind;
        const auto kind = binding->LogicalKind;
        if ((input.Source == MaterialInputSource::SceneObjects && kind != Kind::StructuredBuffer) ||
            ((input.Source == MaterialInputSource::ViewConstants || input.Source == MaterialInputSource::MaterialConstants) && kind != Kind::CBuffer) ||
            (input.Source == MaterialInputSource::Texture && kind != Kind::Texture) ||
            (input.Source == MaterialInputSource::Sampler && kind != Kind::Sampler)) return nullptr;
        if ((input.Source == MaterialInputSource::SceneObjects || input.Source == MaterialInputSource::ViewConstants) && binding->Count != 1) return nullptr;
        plan->Handles.push_back(program->GetPipelineLayout()->FindBinding(input.Name));
        plan->Bindings.push_back(*binding);
    }
    // A physical group must be populated completely, including mixed logical sources.
    for (const auto& binding : artifact.Bindings()) {
        const auto name = artifact.GetName(binding.Name);
        if (!name) return nullptr;
        const auto info = program->GetArtifact().FindBindingInfo(*name);
        if (!info || info->Immutable) continue;
        if (std::none_of(pass.Inputs.begin(), pass.Inputs.end(), [&](const auto& input) { return input.Name == *name; })) return nullptr;
    }
    auto* result = plan.get();
    _plans.push_back(std::move(plan));
    return result;
}

std::optional<PreparedSceneDraw> SceneDraw::Prepare(PipelineContext& context, const SceneViewRequest& view, const SceneGpuView& objects,
                                                    render::RenderPass* pass, const RenderOutputInfo& output, std::string_view passName, Nullable<const MaterialRenderData*> fallback, bool depthTest) {
    RADRAY_PROFILE_SCOPE_N("SceneDraw::Prepare");
    auto* device = context.Resources.GetDevice();
    const auto resolved = ResolveSceneView(view, output.Width, output.Height, device->GetBackend());
    auto scene = context.Scenes ? context.Scenes->GetSceneRT(view.Scene) : nullptr;
    if (!resolved || !scene) return std::nullopt;
    PreparedSceneDraw result{scene.Get(), *resolved, fallback, {}, {}};
    auto constants = context.Resources.AllocateConstants(sizeof(float) * 16);
    if (!constants.IsValid()) return std::nullopt;
    std::memcpy(constants.Data(), resolved->ViewProjection.data(), 64);
    const auto viewAllocation = constants.Commit(64);
    const render::ShaderBufferBinding viewBinding{viewAllocation.Target, {viewAllocation.Offset, viewAllocation.Size}, 0};
    struct PreparedMaterial {
        size_t Index;
        ShaderProgram* Program;
        const MaterialPass* Pass;
    };
    unordered_map<const MaterialRenderData*, PreparedMaterial> materials;
    auto& cache = context.Renderer.GetGraphicsPipelineCache();
    const auto columns = scene->GetStaticMeshColumns();
    std::optional<SceneDrawKey> previous;
    for (size_t row = 0; row < columns.Size(); ++row) {
        const auto object = columns.Get(row);
        const auto mesh = object.Mesh.GetRenderMesh();
        if (!mesh) continue;
        for (const auto& section : object.Mesh.GetSections()) {
            if (!section.IndexCount || section.PrimitiveIndex >= mesh->Draws.size()) continue;
            const auto data = section.MaterialSlot < object.Materials.size() ? object.Materials[section.MaterialSlot].Data : fallback;
            if (!data) {
                RADRAY_ERR_LOG("missing material slot {}", section.MaterialSlot);
                continue;
            }
            const auto& draw = mesh->Draws[section.PrimitiveIndex];
            const SceneDrawKey key{&draw, data.Get()};
            if (previous && *previous == key) continue;
            if (result.GeometryBindings.contains(key)) {
                previous = key;
                continue;
            }
            auto material = materials.find(data.Get());
            if (material == materials.end()) {
                const auto impl = std::find_if(data->Passes.begin(), data->Passes.end(), [&](const auto& p) { return p.Name == passName; });
                if (impl == data->Passes.end()) {
                    RADRAY_ERR_LOG("material has no pass {}", passName);
                    continue;
                }
                auto program = context.Renderer.GetOrCreateShaderProgram(impl->Program);
                if (!program) return std::nullopt;
                const auto plan = ResolveInputs(program.Get(), *impl);
                if (!plan) {
                    RADRAY_ERR_LOG("invalid material input contract for pass {} in {}", passName, impl->Program.SourceName);
                    return std::nullopt;
                }
                PreparedSceneMaterial prepared;
                prepared.ObjectIndex = plan->Push;
                for (size_t i = 0; i < plan->Inputs.size(); ++i) {
                    const auto& input = plan->Inputs[i];
                    const auto& binding = plan->Bindings[i];
                    if (binding.Immutable) continue;
                    auto group = std::find_if(prepared.Groups.begin(), prepared.Groups.end(), [&](const auto& g) { return g.Index == binding.Group; });
                    if (group == prepared.Groups.end()) {
                        auto parameters = context.Resources.AllocateParameters(program->GetPipelineLayout(), binding.Group);
                        if (!parameters) return std::nullopt;
                        prepared.Groups.push_back({binding.Group, parameters.Get(), {}});
                        group = prepared.Groups.end() - 1;
                    }
                    for (uint32_t element = 0; element < binding.Count; ++element) {
                        const size_t valueIndex = size_t(input.ValueIndex) + element;
                        render::ShaderParameterValue value;
                        switch (input.Source) {
                            case MaterialInputSource::SceneObjects: value = objects.Objects; break;
                            case MaterialInputSource::ViewConstants: value = viewBinding; break;
                            case MaterialInputSource::MaterialConstants: {
                                if (valueIndex >= data->Constants.size()) return std::nullopt;
                                const auto& bytes = data->Constants[valueIndex];
                                auto allocation = context.Resources.AllocateConstants(bytes.size());
                                if (!allocation.IsValid()) return std::nullopt;
                                std::memcpy(allocation.Data(), bytes.data(), bytes.size());
                                const auto slice = allocation.Commit(bytes.size());
                                value = render::ShaderBufferBinding{slice.Target, {slice.Offset, slice.Size}, 0};
                                break;
                            }
                            case MaterialInputSource::Texture:
                                if (valueIndex >= data->Textures.size()) return std::nullopt;
                                value = data->Textures[valueIndex];
                                break;
                            case MaterialInputSource::Sampler: {
                                if (valueIndex >= data->Samplers.size()) return std::nullopt;
                                auto sampler = device->GetOrCreateSampler(data->Samplers[valueIndex]);
                                if (!sampler) return std::nullopt;
                                value = sampler.Get();
                                break;
                            }
                        }
                        if (!group->Parameters->Set(plan->Handles[i], element, value)) return std::nullopt;
                    }
                    if (binding.Dynamic) group->Offsets.push_back({plan->Handles[i], 0});
                }
                for (auto& group : prepared.Groups)
                    if (!group.Parameters->FlushWrites()) return std::nullopt;
                const auto index = result.Materials.size();
                result.Materials.push_back(std::move(prepared));
                material = materials.emplace(data.Get(), PreparedMaterial{index, program.Get(), &*impl}).first;
            }
            const auto& prepared = material->second;
            const auto input = cache.Match(draw.Layout, prepared.Program);
            if (!input) return std::nullopt;
            PreparedSceneDraw::Geometry geometry{{}, prepared.Index};
            for (uint32_t reverse = 0; reverse < 2; ++reverse) {
                auto primitive = prepared.Pass->Primitive;
                primitive.Topology = draw.Topology;
                if (reverse) primitive.FaceClockwise = primitive.FaceClockwise == render::FrontFace::CW ? render::FrontFace::CCW : render::FrontFace::CW;
                std::optional<render::DepthStencilState> depth;
                if (output.Depth) {
                    depth = render::DepthStencilState::Default();
                    depth->Format = *output.Depth;
                    depth->DepthTestEnable = depthTest;
                    depth->DepthWriteEnable = depthTest;
                }
                vector<render::ColorTargetState> colors;
                for (const auto format : output.Colors) colors.push_back({format, prepared.Pass->Blend, prepared.Pass->WriteMask});
                auto samples = render::MultiSampleState::Default();
                samples.Count = output.Samples;
                auto pso = cache.GetOrCreate({prepared.Program, *input.Get(), primitive, depth, samples, colors, pass});
                if (!pso) return std::nullopt;
                geometry.Pipelines[reverse] = pso.Get();
            }
            result.GeometryBindings.emplace(key, geometry);
            previous = key;
        }
    }
    return result;
}

void PreparedSceneDraw::Record(render::GraphicsCommandEncoder* encoder) const {
    RADRAY_PROFILE_SCOPE_N("SceneDraw::Draw");
    encoder->SetViewport(View.Viewport);
    encoder->SetScissor(View.Scissor);
    const auto columns = Scene->GetStaticMeshColumns();
    std::optional<SceneDrawKey> previous;
    Nullable<const Geometry*> binding{nullptr};
    for (size_t row = 0; row < columns.Size(); ++row) {
        const auto object = columns.Get(row);
        const auto mesh = object.Mesh.GetRenderMesh();
        if (!mesh) continue;
        for (const auto& section : object.Mesh.GetSections()) {
            if (!section.IndexCount || section.PrimitiveIndex >= mesh->Draws.size()) continue;
            const auto data = section.MaterialSlot < object.Materials.size() ? object.Materials[section.MaterialSlot].Data : Fallback;
            if (!data) continue;
            const auto& draw = mesh->Draws[section.PrimitiveIndex];
            const SceneDrawKey key{&draw, data.Get()};
            if (!previous || *previous != key) {
                const auto found = GeometryBindings.find(key);
                binding = found == GeometryBindings.end() ? nullptr : &found->second;
                previous = key;
            }
            if (!binding) continue;
            const auto& geometry = *binding.Get();
            const auto& material = Materials[geometry.Material];
            encoder->BindGraphicsPipelineState(geometry.Pipelines[object.ReverseCulling ? 1 : 0]);
            for (const auto& group : material.Groups) encoder->BindShaderParameterSet(group.Index, group.Parameters, group.Offsets);
            const uint32_t slot = columns.Ids[row].Index;
            if (!encoder->SetPushConstants(material.ObjectIndex, std::as_bytes(std::span{&slot, 1}))) RADRAY_ABORT("prepared object binding failed");
            encoder->BindVertexBuffers(draw.VertexBuffers);
            encoder->BindIndexBuffer(draw.Ibv);
            encoder->DrawIndexed(section.IndexCount, 1, section.FirstIndex, section.VertexOffset, 0);
        }
    }
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
