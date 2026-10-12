#include <radray/runtime/render_framework/raster_draw.h>
#include <algorithm>
#include <cstring>
#include <radray/runtime/render_system.h>

namespace radray {
Nullable<const RasterMaterialPlan*> RasterDraw::ResolveMaterial(RenderSystem& renderer, const MaterialPass& pass) {
    auto program = pass.ResolveProgram(renderer);
    if (!program) return nullptr;
    if (!pass.ObjectIndexPushConstant.empty() && !pass.PushConstant.empty()) return nullptr;
    const auto pushName = pass.PushConstant.empty() ? pass.ObjectIndexPushConstant : pass.PushConstant;
    const auto requiresConstant = [](const render::BlendComponent& blend) {
        return blend.Src == render::BlendFactor::Constant || blend.Src == render::BlendFactor::OneMinusConstant ||
            blend.Dst == render::BlendFactor::Constant || blend.Dst == render::BlendFactor::OneMinusConstant;
    };
    if (pass.Blend && (requiresConstant(pass.Blend->Color) || requiresConstant(pass.Blend->Alpha))) return nullptr;
    if (pass.DepthStencil && pass.DepthStencil->Stencil) return nullptr;
    for (const auto& plan : _plans)
        if (plan->Program == program.Get() && plan->Inputs == pass.Inputs && plan->PushName == pushName) return plan.get();
    auto plan = make_unique<RasterMaterialPlan>();
    plan->Program = program.Get();
    plan->Inputs = pass.Inputs;
    plan->PushName = pushName;
    const auto& artifact = program->GetArtifact().Generic();
    if (!pushName.empty()) {
        plan->Push = program->GetPipelineLayout()->FindBinding(pushName);
        const auto push = std::find_if(artifact.RootConstants().begin(), artifact.RootConstants().end(), [&](const auto& block) {
            return artifact.GetName(block.Name) == pushName;
        });
        if (!plan->Push.IsValid() || push == artifact.RootConstants().end() ||
            (!pass.ObjectIndexPushConstant.empty() && push->Size != sizeof(uint32_t))) return nullptr;
        plan->PushSize = push->Size;
    } else if (!artifact.RootConstants().empty()) return nullptr;
    for (size_t i = 0; i < pass.Inputs.size(); ++i) {
        const auto& input = pass.Inputs[i];
        for (size_t j = 0; j < i; ++j) if (pass.Inputs[j].Name == input.Name) return nullptr;
        const auto binding = program->GetArtifact().FindBindingInfo(input.Name);
        if (!binding) return nullptr;
        using Kind = shader::ShaderBindingKind;
        const auto kind = binding->LogicalKind;
        switch (input.Source) {
            case MaterialInputSource::SceneObjects: if (kind != Kind::StructuredBuffer || binding->Count != 1) return nullptr; break;
            case MaterialInputSource::ViewConstants:
            case MaterialInputSource::FrameConstants:
            case MaterialInputSource::MaterialConstants: if (kind != Kind::CBuffer) return nullptr; break;
            case MaterialInputSource::Texture: if (kind != Kind::Texture) return nullptr; break;
            case MaterialInputSource::Sampler: if (kind != Kind::Sampler) return nullptr; break;
            default: return nullptr;
        }
        auto handle = program->GetPipelineLayout()->FindBinding(input.Name);
        if (!handle.IsValid()) return nullptr;
        plan->Handles.push_back(handle);
        plan->Bindings.push_back(*binding);
        uint32_t bufferSize = 0;
        if (kind == Kind::CBuffer) {
            const auto declaration = artifact.FindBinding(input.Name);
            if (!declaration || declaration->Record.TypeIndex >= artifact.Types().size()) return nullptr;
            bufferSize = artifact.Types()[declaration->Record.TypeIndex].Size;
        }
        plan->BufferSizes.push_back(bufferSize);
    }
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

std::optional<PreparedRasterMaterial> RasterDraw::PrepareMaterial(PipelineContext& context, const RasterMaterialPlan& plan,
    const MaterialRenderData& material, const RasterMaterialValues& values) {
    if (plan.Program->GetDevice() != context.Resources.GetDevice()) return std::nullopt;
    PreparedRasterMaterial result;
    result.Push = plan.Push;
    result.PushSize = plan.PushSize;
    for (size_t i = 0; i < plan.Inputs.size(); ++i) {
        const auto& input = plan.Inputs[i];
        const auto& binding = plan.Bindings[i];
        if (binding.Immutable) continue;
        auto group = std::find_if(result.Groups.begin(), result.Groups.end(), [&](const auto& g) { return g.Index == binding.Group; });
        if (group == result.Groups.end()) {
            const bool samplersOnly = std::all_of(plan.Bindings.begin(), plan.Bindings.end(), [&](const auto& b) {
                return b.Group != binding.Group || b.Immutable || b.LogicalKind == shader::ShaderBindingKind::Sampler;
            });
            if (samplersOnly) {
                vector<render::SamplerDescriptor> descriptors;
                const std::span<const render::SamplerDescriptor> source = values.Samplers.empty() ? std::span{material.Samplers} : values.Samplers;
                for (size_t entry = 0; entry < plan.Inputs.size(); ++entry) {
                    const auto& b = plan.Bindings[entry];
                    if (b.Group != binding.Group || b.Immutable) continue;
                    const auto first = plan.Inputs[entry].ValueIndex;
                    if (first > source.size() || b.Count > source.size() - first) return std::nullopt;
                    descriptors.insert(descriptors.end(), source.begin() + first, source.begin() + first + b.Count);
                }
                auto cached = std::find_if(_samplerGroups.begin(), _samplerGroups.end(), [&](const auto& cachedGroup) {
                    return cachedGroup.Plan == &plan && cachedGroup.Index == binding.Group && cachedGroup.Descriptors == descriptors;
                });
                if (cached == _samplerGroups.end()) {
                    auto parameters = context.Resources.GetDevice()->CreateShaderParameterSet({plan.Program->GetPipelineLayout(), binding.Group});
                    if (!parameters) return std::nullopt;
                    size_t valueIndex = 0;
                    for (size_t entry = 0; entry < plan.Inputs.size(); ++entry) {
                        const auto& b = plan.Bindings[entry];
                        if (b.Group != binding.Group || b.Immutable) continue;
                        for (uint32_t element = 0; element < b.Count; ++element) {
                            auto sampler = context.Resources.GetDevice()->GetOrCreateSampler(descriptors[valueIndex++]);
                            if (!sampler || !parameters->Set(plan.Handles[entry], element, sampler.Get())) return std::nullopt;
                        }
                    }
                    if (!parameters->FlushWrites()) return std::nullopt;
                    _samplerGroups.push_back({&plan, binding.Group, std::move(descriptors), parameters.Release()});
                    cached = _samplerGroups.end() - 1;
                }
                result.Groups.push_back({binding.Group, cached->Parameters.get(), {}});
                continue;
            }
            auto parameters = context.Resources.AllocateParameters(plan.Program->GetPipelineLayout(), binding.Group);
            if (!parameters) return std::nullopt;
            result.Groups.push_back({binding.Group, parameters.Get(), {}});
            group = result.Groups.end() - 1;
        }
        if (std::any_of(_samplerGroups.begin(), _samplerGroups.end(), [&](const auto& cached) { return cached.Parameters.get() == group->Parameters; })) continue;
        for (uint32_t element = 0; element < binding.Count; ++element) {
            const size_t index = size_t{input.ValueIndex} + element;
            render::ShaderParameterValue value;
            switch (input.Source) {
                case MaterialInputSource::SceneObjects:
                    if (!values.Objects) return std::nullopt;
                    value = *values.Objects; break;
                case MaterialInputSource::MaterialConstants:
                case MaterialInputSource::ViewConstants:
                case MaterialInputSource::FrameConstants: {
                    std::span<const byte> bytes;
                    if (input.Source == MaterialInputSource::MaterialConstants && values.Constants.empty()) {
                        if (index >= material.Constants.size()) return std::nullopt;
                        bytes = material.Constants[index];
                    } else {
                        const auto source = input.Source == MaterialInputSource::ViewConstants ? values.Views :
                            input.Source == MaterialInputSource::FrameConstants ? values.Frames : values.Constants;
                        if (index >= source.size()) return std::nullopt;
                        bytes = source[index];
                    }
                    if (bytes.empty() || bytes.size() < plan.BufferSizes[i]) return std::nullopt;
                    auto allocation = context.Resources.AllocateConstants(bytes.size());
                    if (!allocation.IsValid()) return std::nullopt;
                    std::memcpy(allocation.Data(), bytes.data(), bytes.size());
                    const auto slice = allocation.Commit(bytes.size());
                    value = render::ShaderBufferBinding{slice.Target, {slice.Offset, slice.Size}, 0};
                    break;
                }
                case MaterialInputSource::Texture: {
                    const std::span<render::TextureView* const> textures = values.Textures.empty() ? std::span{material.Textures} : values.Textures;
                    if (index >= textures.size()) return std::nullopt;
                    value = textures[index]; break;
                }
                case MaterialInputSource::Sampler: {
                    const std::span<const render::SamplerDescriptor> samplers = values.Samplers.empty() ? std::span{material.Samplers} : values.Samplers;
                    if (index >= samplers.size()) return std::nullopt;
                    auto sampler = context.Resources.GetDevice()->GetOrCreateSampler(samplers[index]);
                    if (!sampler) return std::nullopt;
                    value = sampler.Get(); break;
                }
                default: return std::nullopt;
            }
            if (!group->Parameters->Set(plan.Handles[i], element, value)) return std::nullopt;
        }
        if (binding.Dynamic) group->Offsets.push_back({plan.Handles[i], 0});
    }
    for (auto& group : result.Groups) if (!group.Parameters->FlushWrites()) return std::nullopt;
    return result;
}

Nullable<render::GraphicsPipelineState*> RasterDraw::PreparePipeline(PipelineContext& context, const RasterMaterialPlan& plan,
    const MaterialPass& material, const GeometryVertexLayout& geometry, render::PrimitiveTopology topology,
    render::RenderPass* pass, const RenderOutputInfo& output, bool reverseCulling) {
    if (plan.Program->GetDevice() != context.Resources.GetDevice() || (material.DepthStencil && !output.Depth)) return nullptr;
    auto& cache = context.Renderer.GetGraphicsPipelineCache();
    const auto layout = cache.Match(geometry, plan.Program);
    if (!layout) return nullptr;
    auto primitive = material.Primitive;
    primitive.Topology = topology;
    if (reverseCulling) primitive.FaceClockwise = primitive.FaceClockwise == render::FrontFace::CW ? render::FrontFace::CCW : render::FrontFace::CW;
    auto depth = material.DepthStencil;
    if (depth) depth->Format = *output.Depth;
    vector<render::ColorTargetState> colors;
    for (const auto format : output.Colors) colors.push_back({format, material.Blend, material.WriteMask});
    auto samples = material.Samples;
    samples.Count = output.Samples;
    return cache.GetOrCreate({plan.Program, *layout.Get(), primitive, depth, samples, colors, pass});
}

void PreparedRasterDraw::Record(render::GraphicsCommandEncoder* encoder, const PreparedRasterMaterial& material) const {
    if (!Count || !Instances || !Scissor.Width || !Scissor.Height) return;
    encoder->SetViewport(Viewport);
    encoder->SetScissor(Scissor);
    encoder->BindGraphicsPipelineState(Pipeline);
    for (const auto& group : material.Groups) encoder->BindShaderParameterSet(group.Index, group.Parameters, group.Offsets);
    if (material.Push.IsValid() && (Push.size() != material.PushSize || !encoder->SetPushConstants(material.Push, Push))) RADRAY_ABORT("prepared raster push binding failed");
    encoder->BindVertexBuffers(Vertices);
    if (Indices) {
        encoder->BindIndexBuffer(*Indices);
        encoder->DrawIndexed(Count, Instances, First, VertexOffset, FirstInstance);
    } else encoder->Draw(Count, Instances, First, FirstInstance);
}
}  // namespace radray
