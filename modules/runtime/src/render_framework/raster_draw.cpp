#include <radray/runtime/render_framework/raster_draw.h>
#include <algorithm>
#include <cstring>
#include <radray/runtime/render_system.h>
#include <radray/profiler.h>
#include <radray/logger.h>

namespace radray {
RasterInstanceData::RasterInstanceData(const RasterInstanceData& other)
    : Objects(other.Objects), VisibleSlots(other.VisibleSlots), Layouts(other.Layouts), Parameters(other.Parameters), Visible(other.Visible) {
    RADRAY_ASSERT(Layouts.size() == Parameters.size());
    for (size_t i = 0; i < Parameters.size(); ++i) Parameters[i].Layout = &Layouts[i];
}
RasterInstanceData& RasterInstanceData::operator=(const RasterInstanceData& other) {
    if (this != &other) {
        RasterInstanceData copy{other};
        *this = std::move(copy);
    }
    return *this;
}
RasterInstancePreparation PrepareRasterInstances(PipelineContext& context, const RasterMaterialPlan& plan,
                                                 render::ShaderBufferBinding objects, std::span<const uint32_t> slots, std::span<const InstanceParameterBuffer> parameters) {
    using Status = RasterInstancePrepareStatus;
    if (plan.InstanceAddressing != MaterialInstanceAddressing::VisibleSlots) return {Status::Unsupported, {}};
    if (slots.empty()) return {Status::NoWork, {}};
    if (!plan.Generation->Alive || plan.Generation != context.Renderer.GetGeneration()) return {Status::InvalidData, {}};
    const auto validBuffer = [&](const render::ShaderBufferBinding& binding, uint32_t stride) {
        if (!binding.Target || binding.Target->GetDevice() != context.Resources.GetDevice() || binding.StructureByteStride != stride ||
            !binding.Range.Size || binding.Range.Size % stride || binding.Range.Offset % stride) return false;
        const auto desc = binding.Target->GetDesc();
        const auto alignment = std::max(uint64_t{1}, context.Resources.GetDevice()->GetCapabilities().Limits.StorageBufferOffsetAlignment);
        return desc.Usage.HasFlag(render::BufferUse::Resource) && binding.Range.Offset % alignment == 0 &&
               binding.Range.Offset <= desc.Size && binding.Range.Size <= desc.Size - binding.Range.Offset;
    };
    if (!validBuffer(objects, 64) || slots.size() > UINT32_MAX) return {Status::InvalidData, {}};
    for (auto slot : slots)
        if (slot >= objects.Range.Size / 64) return {Status::InvalidData, {}};
    for (const auto& input : plan.Inputs) {
        if (input.Source != MaterialInputSource::InstanceParameters) continue;
        const auto& layout = plan.ParameterLayouts[input.ValueIndex];
        const auto found = std::find_if(parameters.begin(), parameters.end(), [&](const auto& value) { return value.Layout && *value.Layout == layout; });
        if (found == parameters.end() || !validBuffer(found->Values, layout.Stride)) return {Status::InvalidData, {}};
        for (auto slot : slots)
            if (slot >= found->Values.Range.Size / layout.Stride) return {Status::InvalidData, {}};
    }
    auto allocation = context.Resources.AllocateStorage(slots.size_bytes());
    if (!allocation.IsValid()) return {Status::AllocationFailure, {}};
    std::memcpy(allocation.Data(), slots.data(), slots.size_bytes());
    const auto slice = allocation.Commit(slots.size_bytes());
    RasterInstanceData data;
    data.Objects = objects;
    data.VisibleSlots = {slice.Target, {slice.Offset, slice.Size}, 4};
    data.Visible = {0, static_cast<uint32_t>(slots.size())};
    data.Layouts.reserve(parameters.size());
    data.Parameters.reserve(parameters.size());
    for (const auto& value : parameters) {
        if (!value.Layout) return {Status::InvalidData, {}};
        data.Layouts.push_back(*value.Layout);
        data.Parameters.push_back({&data.Layouts.back(), value.Values});
    }
    return {Status::Prepared, std::move(data)};
}
RasterInstancePreparation PrepareRasterInstances(PipelineContext& context, const RasterMaterialPlan& plan,
                                                 std::span<const Eigen::Matrix4f> matrices, std::span<const uint32_t> slots, std::span<const RasterFrameParameters> parameters) {
    using Status = RasterInstancePrepareStatus;
    if (plan.InstanceAddressing != MaterialInstanceAddressing::VisibleSlots) return {Status::Unsupported, {}};
    if (slots.empty()) return {Status::NoWork, {}};
    if (matrices.empty()) return {Status::InvalidData, {}};
    for (const auto& matrix : matrices)
        if (!matrix.allFinite() || matrix(3, 0) != 0 || matrix(3, 1) != 0 || matrix(3, 2) != 0 || matrix(3, 3) != 1) return {Status::InvalidData, {}};
    for (auto slot : slots)
        if (slot >= matrices.size()) return {Status::InvalidData, {}};
    for (const auto& value : parameters)
        if (!value.Layout.IsValid() || value.Values.size() != matrices.size() * value.Layout.Stride) return {Status::InvalidData, {}};
    auto allocation = context.Resources.AllocateStorage(matrices.size_bytes(), 64);
    if (!allocation.IsValid()) return {Status::AllocationFailure, {}};
    std::memcpy(allocation.Data(), matrices.data(), matrices.size_bytes());
    const auto slice = allocation.Commit(matrices.size_bytes());
    vector<InstanceParameterBuffer> bindings;
    for (const auto& value : parameters) {
        auto bytes = context.Resources.AllocateStorage(value.Values.size(), value.Layout.Stride);
        if (!bytes.IsValid()) return {Status::AllocationFailure, {}};
        std::memcpy(bytes.Data(), value.Values.data(), value.Values.size());
        const auto uploaded = bytes.Commit(value.Values.size());
        bindings.push_back({&value.Layout, {uploaded.Target, {uploaded.Offset, uploaded.Size}, value.Layout.Stride}});
    }
    return PrepareRasterInstances(context, plan, render::ShaderBufferBinding{slice.Target, {slice.Offset, slice.Size}, 64}, slots, bindings);
}
Nullable<const RasterMaterialPlan*> RasterDraw::ResolveMaterial(RenderSystem& renderer, const MaterialPass& source) {
    const auto& pass = source.GetDefinition();
    if (source._definition && pass._nativePlan && pass._nativePlan->Generation == renderer.GetGeneration() && pass._nativePlan->Generation->Alive) return pass._nativePlan.get();
    auto program = pass.ResolveProgram(renderer);
    if (!program) return nullptr;
    if (!pass.ObjectIndexPushConstant.empty() && !pass.PushConstant.empty()) return nullptr;
    const auto pushName = pass.PushConstant.empty() ? pass.ObjectIndexPushConstant : pass.PushConstant;
    if (!pass.ScenePushValues.empty()) {
        const auto& artifact = program->GetArtifact().Generic();
        const auto push = std::find_if(artifact.RootConstants().begin(), artifact.RootConstants().end(), [&](const auto& block) { return artifact.GetName(block.Name) == pushName; });
        if (push == artifact.RootConstants().end() || pass.ScenePushValues.size() != push->Size) return nullptr;
    }
    const auto requiresConstant = [](const render::BlendComponent& blend) {
        return blend.Src == render::BlendFactor::Constant || blend.Src == render::BlendFactor::OneMinusConstant ||
               blend.Dst == render::BlendFactor::Constant || blend.Dst == render::BlendFactor::OneMinusConstant;
    };
    if (pass.Blend && (requiresConstant(pass.Blend->Color) || requiresConstant(pass.Blend->Alpha))) return nullptr;
    if (pass.DepthStencil && pass.DepthStencil->Stencil) return nullptr;
    const auto& cached = pass._nativePlan;
    if (cached) {
        const auto& plan = cached;
        if (plan->Generation == renderer.GetGeneration() && plan->Program == program.Get() && plan->Inputs == pass.Inputs && plan->PushName == pushName &&
            plan->LegacyObjectIndexPush == !pass.ObjectIndexPushConstant.empty() && plan->ParameterLayouts == pass.ParameterLayouts &&
            plan->InstanceAddressing == pass.InstanceAddressing && plan->SceneIndexField == pass.SceneIndexField) return plan.get();
        if (source._definition && plan->Generation->Alive && plan->Generation != renderer.GetGeneration()) return nullptr;
    }
    auto plan = make_shared<RasterMaterialPlan>();
    plan->Generation = renderer.GetGeneration();
    plan->Program = program.Get();
    plan->Inputs = pass.Inputs;
    plan->ParameterLayouts = pass.ParameterLayouts;
    plan->PushName = pushName;
    plan->LegacyObjectIndexPush = !pass.ObjectIndexPushConstant.empty();
    plan->InstanceAddressing = pass.InstanceAddressing;
    plan->SceneIndexField = pass.SceneIndexField;
    const auto& artifact = program->GetArtifact().Generic();
    if (!pushName.empty()) {
        plan->Push = program->GetPipelineLayout()->FindBinding(pushName);
        const auto push = std::find_if(artifact.RootConstants().begin(), artifact.RootConstants().end(), [&](const auto& block) {
            return artifact.GetName(block.Name) == pushName;
        });
        if (!plan->Push.IsValid() || push == artifact.RootConstants().end() ||
            (!pass.ObjectIndexPushConstant.empty() && push->Size != sizeof(uint32_t))) return nullptr;
        plan->PushSize = push->Size;
        if (!pass.ObjectIndexPushConstant.empty()) {
            if (pass.InstanceAddressing != MaterialInstanceAddressing::DirectObject || !pass.SceneIndexField.empty()) return nullptr;
            plan->SceneIndexOffset = 0;
        } else if (!pass.SceneIndexField.empty()) {
            const auto field = std::find_if(artifact.Types().begin(), artifact.Types().end(), [&](const auto& type) {
                return type.ParentIndex == push->TypeIndex && artifact.GetName(type.Name) == pass.SceneIndexField;
            });
            if (field == artifact.Types().end() || field->Size != 4 || field->ScalarKind != uint32_t(shader::ShaderScalarKind::UnsignedInteger) ||
                field->RowCount != 1 || field->ColumnCount != 1 || field->Offset > push->Size || 4 > push->Size - field->Offset) return nullptr;
            plan->SceneIndexOffset = field->Offset;
        }
        if (!pass.ScenePushValues.empty() && pass.ScenePushValues.size() != push->Size) return nullptr;
    } else if (!artifact.RootConstants().empty())
        return nullptr;
    for (size_t i = 0; i < pass.Inputs.size(); ++i) {
        const auto& input = pass.Inputs[i];
        for (size_t j = 0; j < i; ++j)
            if (pass.Inputs[j].Name == input.Name) return nullptr;
        const auto binding = program->GetArtifact().FindBindingInfo(input.Name);
        if (!binding) return nullptr;
        using Kind = shader::ShaderBindingKind;
        const auto kind = binding->LogicalKind;
        switch (input.Source) {
            case MaterialInputSource::SceneObjects:
            case MaterialInputSource::VisibleInstanceSlots:
                if (kind != Kind::StructuredBuffer || binding->Count != 1) return nullptr;
                break;
            case MaterialInputSource::InstanceParameters:
                if (kind != Kind::StructuredBuffer || binding->Count != 1 || input.ValueIndex >= pass.ParameterLayouts.size() || !pass.ParameterLayouts[input.ValueIndex].IsValid()) return nullptr;
                break;
            case MaterialInputSource::ViewConstants:
            case MaterialInputSource::FrameConstants:
            case MaterialInputSource::MaterialConstants:
                if (kind != Kind::CBuffer) return nullptr;
                break;
            case MaterialInputSource::Texture:
                if (kind != Kind::Texture) return nullptr;
                break;
            case MaterialInputSource::Sampler:
                if (kind != Kind::Sampler) return nullptr;
                break;
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
        if (!binding->Immutable) {
            auto group = std::find_if(plan->Groups.begin(), plan->Groups.end(), [&](const auto& entry) { return entry.Index == binding->Group; });
            if (group == plan->Groups.end()) {
                plan->Groups.push_back({binding->Group, true, {}});
                group = plan->Groups.end() - 1;
            }
            group->Inputs.push_back(i);
            group->Static &= input.Source == MaterialInputSource::MaterialConstants || input.Source == MaterialInputSource::Texture || input.Source == MaterialInputSource::Sampler;
        }
    }
    if (pass.InstanceAddressing == MaterialInstanceAddressing::VisibleSlots &&
        (!plan->SceneIndexOffset || std::none_of(pass.Inputs.begin(), pass.Inputs.end(), [](const auto& input) { return input.Source == MaterialInputSource::VisibleInstanceSlots; }))) return nullptr;
    for (const auto& binding : artifact.Bindings()) {
        const auto name = artifact.GetName(binding.Name);
        if (!name) return nullptr;
        const auto info = program->GetArtifact().FindBindingInfo(*name);
        if (!info || info->Immutable) continue;
        if (std::none_of(pass.Inputs.begin(), pass.Inputs.end(), [&](const auto& input) { return input.Name == *name; })) return nullptr;
    }
    auto* result = plan.get();
    pass._nativePlan = std::move(plan);
    return result;
}

std::optional<PreparedRasterMaterial> RasterDraw::PrepareMaterial(PipelineContext& context, const RasterMaterialPlan& plan,
                                                                  MaterialBindingView binding, const RasterMaterialValues& values) {
    return PrepareMaterial(context, plan, binding.Data, values, &binding.Gpu);
}
std::optional<PreparedRasterMaterial> RasterDraw::PrepareMaterial(PipelineContext& context, const RasterMaterialPlan& plan,
                                                                  const MaterialRenderData& material, const RasterMaterialValues& values, Nullable<MaterialGpuState*> gpu, Nullable<const MaterialPass*> source) {
    RADRAY_PROFILE_SCOPE_N("RasterDraw::PrepareMaterial");
    if (!plan.Generation->Alive || plan.Generation != context.Renderer.GetGeneration() || plan.Program->GetDevice() != context.Resources.GetDevice()) return std::nullopt;
    Nullable<MaterialGpuState::Pass*> cachedPass{nullptr};
    if (gpu) {
        if (gpu->_data && gpu->_data.Get() != &material) return std::nullopt;
        gpu->_data = &material;
        if (gpu->_generation != plan.Generation) {
            if (gpu->_generation && gpu->_generation->Alive) return std::nullopt;
            gpu->_passes.clear();
            gpu->_constants.clear();
            gpu->_generation = plan.Generation;
        }
        gpu->_constants.resize(material.Constants.size());
        if (!source && material.Passes.size() == 1) source = &material.Passes[0];
        if (source) source = &source->GetDefinition();
        if (source && std::none_of(material.Passes.begin(), material.Passes.end(), [&](const auto& pass) { return &pass.GetDefinition() == source.Get(); })) return std::nullopt;
        if (!source) return std::nullopt;
        const auto found = gpu->_passes.find(source.Get());
        if (found != gpu->_passes.end()) {
            if (found->second.Plan->Program != plan.Program) {
                RADRAY_ERR_LOG("A changed shader program requires a new immutable material GPU owner");
                return std::nullopt;
            }
            cachedPass = &found->second;
        } else {
            auto owner = plan.weak_from_this().lock();
            if (!owner) return std::nullopt;
            cachedPass = &gpu->_passes.emplace(source.Get(), MaterialGpuState::Pass{std::move(owner), {}}).first->second;
        }
    }
    PreparedRasterMaterial result;
    result.Push = plan.Push;
    result.PushSize = plan.PushSize;
    result.Groups.reserve(plan.Groups.size());
    for (const auto& groupPlan : plan.Groups) {
        const bool overridden = std::any_of(groupPlan.Inputs.begin(), groupPlan.Inputs.end(), [&](size_t i) {
            switch (plan.Inputs[i].Source) {
                case MaterialInputSource::MaterialConstants: return !values.Constants.empty() || !values.ConstantBuffers.empty();
                case MaterialInputSource::Texture: return !values.Textures.empty();
                case MaterialInputSource::Sampler: return !values.Samplers.empty();
                default: return false;
            }
        });
        const bool persistent = cachedPass && groupPlan.Static && !overridden;
        if (persistent) {
            const auto found = std::find_if(cachedPass->Groups.begin(), cachedPass->Groups.end(), [&](const auto& group) { return group.Index == groupPlan.Index; });
            if (found != cachedPass->Groups.end()) {
                result.Groups.push_back({found->Index, found->Parameters.get(), found->Offsets});
                continue;
            }
        }
        vector<render::ShaderParameterValue> writes;
        vector<render::ShaderBufferBinding> buffers;
        const bool shared = values.SharedGroups && std::none_of(groupPlan.Inputs.begin(), groupPlan.Inputs.end(), [&](size_t i) {
                                const auto source = plan.Inputs[i].Source;
                                return source == MaterialInputSource::MaterialConstants || source == MaterialInputSource::Texture || source == MaterialInputSource::Sampler;
                            });
        for (const auto i : groupPlan.Inputs) {
            const auto& input = plan.Inputs[i];
            const auto& binding = plan.Bindings[i];
            for (uint32_t element = 0; element < binding.Count; ++element) {
                const size_t index = size_t{input.ValueIndex} + element;
                render::ShaderParameterValue value;
                switch (input.Source) {
                    case MaterialInputSource::SceneObjects:
                        if (!values.Objects) return std::nullopt;
                        value = *values.Objects;
                        break;
                    case MaterialInputSource::VisibleInstanceSlots:
                        if (!values.VisibleSlots || values.VisibleSlots->StructureByteStride != 4 || !values.VisibleSlots->Range.Size || values.VisibleSlots->Range.Size % 4) return std::nullopt;
                        value = *values.VisibleSlots;
                        break;
                    case MaterialInputSource::InstanceParameters: {
                        if (index >= plan.ParameterLayouts.size()) return std::nullopt;
                        const auto& layout = plan.ParameterLayouts[index];
                        const auto found = std::find_if(values.Parameters.begin(), values.Parameters.end(), [&](const auto& p) { return p.Layout && *p.Layout == layout; });
                        if (found == values.Parameters.end() || found->Values.StructureByteStride != layout.Stride) return std::nullopt;
                        value = found->Values;
                        break;
                    }
                    case MaterialInputSource::MaterialConstants:
                    case MaterialInputSource::ViewConstants:
                    case MaterialInputSource::FrameConstants: {
                        const auto buffers = input.Source == MaterialInputSource::ViewConstants ? values.ViewBuffers : input.Source == MaterialInputSource::FrameConstants ? values.FrameBuffers
                                                                                                                                                                           : values.ConstantBuffers;
                        if (!buffers.empty()) {
                            if (index >= buffers.size() || !buffers[index].Target || buffers[index].Range.Size < plan.BufferSizes[i]) return std::nullopt;
                            value = buffers[index];
                            break;
                        }
                        const bool defaults = input.Source == MaterialInputSource::MaterialConstants && values.Constants.empty();
                        std::span<const byte> bytes;
                        if (defaults) {
                            if (index >= material.Constants.size()) return std::nullopt;
                            bytes = material.Constants[index];
                        } else {
                            const auto source = input.Source == MaterialInputSource::ViewConstants ? values.Views : input.Source == MaterialInputSource::FrameConstants ? values.Frames
                                                                                                                                                                        : values.Constants;
                            if (index >= source.size()) return std::nullopt;
                            bytes = source[index];
                        }
                        if (bytes.empty() || bytes.size() < plan.BufferSizes[i]) return std::nullopt;
                        if (defaults && gpu) {
                            auto& allocation = gpu->_constants[index];
                            if (!allocation) allocation = context.Renderer.GetPersistentConstants().Allocate(bytes);
                            if (!allocation) return std::nullopt;
                            value = allocation->Binding;
                        } else {
                            auto allocation = context.Resources.AllocateConstants(bytes.size());
                            if (!allocation.IsValid()) return std::nullopt;
                            std::memcpy(allocation.Data(), bytes.data(), bytes.size());
                            const auto slice = allocation.Commit(bytes.size());
                            value = render::ShaderBufferBinding{slice.Target, {slice.Offset, slice.Size}, 0};
                        }
                        break;
                    }
                    case MaterialInputSource::Texture: {
                        const std::span<render::TextureView* const> source = values.Textures.empty() ? std::span{material.Textures} : values.Textures;
                        if (index >= source.size()) return std::nullopt;
                        value = source[index];
                        break;
                    }
                    case MaterialInputSource::Sampler: {
                        const std::span<const render::SamplerDescriptor> source = values.Samplers.empty() ? std::span{material.Samplers} : values.Samplers;
                        if (index >= source.size()) return std::nullopt;
                        auto sampler = context.Resources.GetDevice()->GetOrCreateSampler(source[index]);
                        if (!sampler) return std::nullopt;
                        value = sampler.Get();
                        break;
                    }
                    default: return std::nullopt;
                }
                if (shared) buffers.push_back(std::get<render::ShaderBufferBinding>(value));
                writes.push_back(value);
            }
        }
        if (shared) {
            const auto& entries = values.SharedGroups->Groups[&plan][groupPlan.Index];
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.Values == buffers; });
            if (found != entries.end()) {
                result.Groups.push_back(found->Group);
                continue;
            }
        }
        unique_ptr<render::ShaderParameterSet> owned;
        RADRAY_PROFILE_SCOPE_DYN(persistent ? std::string_view{"RasterDraw::InitializeStaticGroup"} : std::string_view{"RasterDraw::WriteFrameGroup"});
        Nullable<render::ShaderParameterSet*> parameters;
        if (persistent) {
            auto created = context.Resources.GetDevice()->CreateShaderParameterSet({plan.Program->GetPipelineLayout(), groupPlan.Index});
            if (!created) return std::nullopt;
            owned = created.Release();
            parameters = owned.get();
        } else
            parameters = context.Resources.AllocateParameters(plan.Program->GetPipelineLayout(), groupPlan.Index, plan.Generation);
        if (!parameters) return std::nullopt;
        PreparedRasterMaterial::Group group{groupPlan.Index, parameters.Get(), {}};
        size_t next = 0;
        for (const auto i : groupPlan.Inputs) {
            for (uint32_t element = 0; element < plan.Bindings[i].Count; ++element)
                if (!parameters->Set(plan.Handles[i], element, writes[next++])) return std::nullopt;
            if (plan.Bindings[i].Dynamic) group.Offsets.push_back({plan.Handles[i], 0});
        }
        if (!parameters->FlushWrites()) return std::nullopt;
        if (persistent) cachedPass->Groups.push_back({group.Index, std::move(owned), group.Offsets});
        if (shared) values.SharedGroups->Groups[&plan][groupPlan.Index].push_back({std::move(buffers), group});
        result.Groups.push_back(std::move(group));
    }
    return result;
}

Nullable<render::GraphicsPipelineState*> RasterDraw::PreparePipeline(PipelineContext& context, const RasterMaterialPlan& plan,
                                                                     const MaterialPass& source, const GeometryVertexLayout& geometry, render::PrimitiveTopology topology,
                                                                     const RenderOutputInfo& output, bool reverseCulling) {
    const auto& material = source.GetDefinition();
    if (!plan.Generation->Alive || plan.Generation != context.Renderer.GetGeneration() ||
        plan.Program->GetDevice() != context.Resources.GetDevice() || (material.DepthStencil && !output.Depth)) return nullptr;
    auto& cache = context.Renderer.GetGraphicsPipelineCache();
    const auto layout = cache.Match(geometry, plan.Program);
    if (!layout) return nullptr;
    auto primitive = material.Primitive;
    primitive.Topology = topology;
    if (reverseCulling) primitive.FaceClockwise = primitive.FaceClockwise == render::FrontFace::CW ? render::FrontFace::CCW : render::FrontFace::CW;
    auto depth = material.DepthStencil;
    if (depth)
        depth->Format = *output.Depth;
    else if (output.Depth)
        depth = render::DepthStencilState{.Format = *output.Depth};
    vector<render::ColorTargetState> colors;
    for (const auto format : output.Colors) colors.push_back({format, material.Blend, material.WriteMask});
    auto samples = material.Samples;
    samples.Count = output.Samples;
    return cache.GetOrCreate({plan.Program, *layout.Get(), primitive, depth, samples, colors});
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
    } else
        encoder->Draw(Count, Instances, First, FirstInstance);
}
}  // namespace radray
