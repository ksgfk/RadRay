#include "forward_effects.h"
#include "forward_lit_mesh_pass_processor.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/forward_pipeline/forward_graph.h>
#include <radray/runtime/render_framework/viewport.h>
#include <radray/runtime/render_system.h>
#include <radray/utility.h>

namespace radray::forward_detail {
namespace {
using render::TextureFormat;
using render::TextureUse;
using render::TextureUses;
constexpr TextureUses kHdrUsage = TextureUse::Resource | TextureUse::UnorderedAccess | TextureUse::CopySource | TextureUse::CopyDestination;
constexpr TextureUses kScalarUsage = TextureUse::Resource | TextureUse::UnorderedAccess | TextureUse::CopySource | TextureUse::CopyDestination;

RgTextureValue Texture(RenderGraph& graph, RenderExtent extent, TextureFormat format, TextureUses usage, std::string_view name, uint32_t samples = 1, uint32_t layers = 1) {
    return graph.CreateTexture({layers == 1 ? render::TextureDimension::Dim2D : render::TextureDimension::Dim2DArray,
                                extent.Width,
                                extent.Height,
                                layers,
                                1,
                                samples,
                                format,
                                render::MemoryType::Device,
                                usage,
                                {}},
                               name);
}
RenderExtent Half(RenderExtent value) { return {std::max(1u, (value.Width + 1) / 2), std::max(1u, (value.Height + 1) / 2)}; }
render::SamplerDescriptor ClampSampler() {
    render::SamplerDescriptor sampler;
    sampler.MinFilter = sampler.MagFilter = sampler.MipmapFilter = render::FilterMode::Linear;
    sampler.AddressS = sampler.AddressT = sampler.AddressR = render::AddressMode::ClampToEdge;
    return sampler;
}
Eigen::Matrix4f UnjitteredProjection(const ResolvedRenderView& view) {
    Eigen::Matrix4f p = view.Projection;
    p.row(0) -= view.JitterNdc.x() * p.row(3);
    p.row(1) -= view.JitterNdc.y() * p.row(3);
    return p;
}
inline constexpr uint64_t kForwardWorkEffects = 16;

struct EffectReady {
    Nullable<render::ComputePipelineState*> Compute{nullptr};
    Nullable<render::GraphicsPipelineState*> Raster{nullptr};
    PreparedShaderGroup Set{};
};
struct EffectFrame {
    Nullable<ForwardHdrView*> Work{nullptr};
    ForwardPipelineSettings Settings;
    ResolvedRenderView View;
    Rect OutputViewport{}, OutputScissor{};
    uint64_t Serial{0};
    bool HistoryValid{false};
    Forward_EffectsData Base{};
    vector<EffectReady> Ready;
    vector<ForwardOutputSurface> Surfaces;
};
void PrepareEffectInputs(EffectFrame& frame) {
    auto& values = frame.Base;
    const auto& view = frame.View;
    values.InverseProjection = view.Projection.inverse().eval();
    values.InverseViewProjection = view.ViewProjection.inverse().eval();
    values.PreviousViewProjection = view.PreviousViewValid ? view.PreviousViewProjection : view.ViewProjection;
    values.WorldToView = view.View;
    values.Projection = UnjitteredProjection(view);
    values.Eye = Eigen::Vector4f{view.WorldPosition.x(), view.WorldPosition.y(), view.WorldPosition.z(), 1};
}
struct EffectGraphBuilder {
    RenderGraph& Graph;
    RgTemplateSlot<EffectFrame> Frame;
    RgWorkHandle Work;
    uint32_t ReadyCount{0};
};
enum class EffectValueMode : uint8_t { Fixed,
                                       AmbientOcclusion,
                                       Fireflies,
                                       ToneMap,
                                       Surface };
struct EffectRecipe {
    ShaderProgram* Program{nullptr};
    uint64_t ProgramGeneration{0};
    MaterialPipelineState State{};
    vector<RgParameterBinding> Resources;
    vector<string> Names;
    vector<string> CBuffers;
    string Sampler;
    RenderExtent Size{};
    float4 Extent{}, Options{};
    uint32_t DebugMode{0}, ReadyIndex{0}, Vertices{3}, SurfaceIndex{0};
    EffectValueMode Mode{EffectValueMode::Fixed};
    render::RenderBackend Backend{render::RenderBackend::MAX_COUNT};
    RgIndirectArgumentsHandle Arguments{};
    bool Declared{true}, OutputViewport{false};
};
Forward_EffectsData EffectValues(RenderExtent output, RenderExtent input) {
    Forward_EffectsData values{};
    values.Extent = Eigen::Vector4f{float(output.Width), float(output.Height), float(input.Width), float(input.Height)};
    return values;
}
template <class Builder>
bool DeclareEffectResources(Builder& builder, const ShaderProgram& program,
                            std::span<const ForwardPassResource> resources, vector<RgParameterBinding>& out) {
    out.reserve(resources.size());
    bool declared = true;
    for (const ForwardPassResource& resource : resources) {
        if (!program.GetArtifact().FindBindingInfo(resource.Declaration)) continue;
        const auto row = DeclareForwardPassResource(builder, resource);
        if (row)
            out.push_back(*row);
        else
            declared = false;
    }
    return declared;
}
void FinishEffectRecipe(EffectRecipe& recipe, ShaderProgram& program, std::string_view sampler) {
    recipe.Program = &program;
    recipe.ProgramGeneration = program.GetGeneration();
    for (const auto& buffer : program.GetParameterLayout().Buffers()) recipe.CBuffers.push_back(buffer.Name);
    if (program.GetArtifact().FindBindingInfo(sampler)) recipe.Sampler = sampler;
    recipe.Names.reserve(recipe.Resources.size());
    for (const auto& row : recipe.Resources) recipe.Names.emplace_back(row.Declaration);
    for (size_t i = 0; i < recipe.Resources.size(); ++i) recipe.Resources[i].Declaration = recipe.Names[i];
}
template <class Constants>
vector<RgParameterBinding> EffectBindings(const EffectRecipe& recipe, const Constants& values) {
    vector<RgParameterBinding> bindings;
    bindings.reserve(recipe.CBuffers.size() + recipe.Resources.size() + !recipe.Sampler.empty());
    for (const auto& name : recipe.CBuffers) bindings.push_back({name, 0, RgCBufferParameterBinding{AsCBufferBytes(values)}});
    bindings.insert(bindings.end(), recipe.Resources.begin(), recipe.Resources.end());
    if (!recipe.Sampler.empty()) bindings.push_back({recipe.Sampler, 0, RgSamplerParameterBinding{ClampSampler()}});
    return bindings;
}
Forward_EffectsData FrameEffectValues(const EffectRecipe& recipe, const EffectFrame& frame) {
    auto values = frame.Base;
    values.Extent = recipe.Extent;
    values.Options = recipe.Options;
    values.DebugMode = recipe.DebugMode;
    values.LocalLightCount = frame.Work ? frame.Work->LightCount : 0;
    values.TileCapacity = frame.Settings.MaxLightsPerTile;
    values.HistoryValid = frame.HistoryValid ? 1u : 0u;
    switch (recipe.Mode) {
        case EffectValueMode::AmbientOcclusion: values.Options.x = frame.Settings.AoRadius; break;
        case EffectValueMode::Fireflies: values.Options.x = float(frame.Serial % 360000) / 60; break;
        case EffectValueMode::ToneMap:
            values.Options.x = frame.Settings.Exposure;
            values.Options.y = frame.Settings.Bloom && frame.Settings.DebugView == ForwardDebugView::Final ? frame.Settings.BloomStrength : 0.f;
            break;
        default: break;
    }
    return values;
}
bool PrepareComputeEffect(const EffectRecipe& recipe, EffectFrame& frame, RenderGraphPrepareContext& context) {
    if (!recipe.Declared || recipe.Program->GetGeneration() != recipe.ProgramGeneration || recipe.ReadyIndex >= frame.Ready.size()) return false;
    auto& ready = frame.Ready[recipe.ReadyIndex];
    ready.Compute = context.ResolveComputePipeline(*recipe.Program);
    const auto values = FrameEffectValues(recipe, frame);
    ready.Set = context.CreateParameterSet(*recipe.Program, 0, EffectBindings(recipe, values));
    return bool(ready.Compute) && ready.Set.IsValid();
}
void ExecuteComputeEffect(const EffectRecipe& recipe, const EffectFrame& frame, RenderGraphComputeContext& context) {
    const auto& ready = frame.Ready[recipe.ReadyIndex];
    context.Encoder().BindComputePipelineState(ready.Compute.Get());
    context.Encoder().BindShaderParameterSet(ready.Set);
    context.Encoder().Dispatch((recipe.Size.Width + 7) / 8, (recipe.Size.Height + 7) / 8, 1);
}
bool PrepareRasterEffect(const EffectRecipe& recipe, EffectFrame& frame, RenderGraphPrepareContext& context) {
    if (!recipe.Declared || recipe.Program->GetGeneration() != recipe.ProgramGeneration || recipe.ReadyIndex >= frame.Ready.size()) return false;
    auto& ready = frame.Ready[recipe.ReadyIndex];
    ready.Raster = context.ResolveGraphicsPipeline(*recipe.Program, recipe.State);
    if (recipe.Mode == EffectValueMode::Surface) {
        if (recipe.SurfaceIndex >= frame.Surfaces.size()) return false;
        const auto& surface = frame.Surfaces[recipe.SurfaceIndex];
        Forward_OutputSurfaceData values{};
        values.LocalToClip = (frame.View.ViewProjection * surface.LocalToWorld).eval();
        values.Options = recipe.Options;
        values.Options.x = surface.Brightness;
        ready.Set = context.CreateParameterSet(*recipe.Program, 0, EffectBindings(recipe, values));
    } else {
        const auto values = FrameEffectValues(recipe, frame);
        ready.Set = context.CreateParameterSet(*recipe.Program, 0, EffectBindings(recipe, values));
    }
    return bool(ready.Raster) && ready.Set.IsValid();
}
void ExecuteRasterEffect(const EffectRecipe& recipe, const EffectFrame& frame, RenderGraphRasterContext& context) {
    const auto& ready = frame.Ready[recipe.ReadyIndex];
    auto& encoder = context.Encoder();
    encoder.BindGraphicsPipelineState(ready.Raster.Get());
    encoder.BindShaderParameterSet(ready.Set);
    const auto& viewport = recipe.OutputViewport ? frame.OutputViewport : frame.View.ViewRect;
    encoder.SetViewport(MakeViewport(recipe.Backend, float(viewport.X), float(viewport.Y), float(viewport.Width), float(viewport.Height)));
    encoder.SetScissor(recipe.OutputViewport ? frame.OutputScissor : frame.View.ScissorRect);
    if (recipe.Arguments.IsValid())
        encoder.DrawIndirect(recipe.Arguments);
    else
        encoder.Draw(recipe.Vertices, 1, 0, 0);
}
void Compute(EffectGraphBuilder& effects, std::string_view name, ShaderProgram& program, const Forward_EffectsData& values,
             std::span<const ForwardPassResource> resources, RenderExtent size, EffectValueMode mode = EffectValueMode::Fixed) {
    const uint32_t index = effects.ReadyCount++;
    effects.Graph.AddTemplateComputePass<EffectRecipe>(name, effects.Frame, [&](EffectRecipe& recipe, RenderGraphComputeBuilder& builder) {
            recipe.Extent = values.Extent;
            recipe.Options = values.Options;
            recipe.DebugMode = values.DebugMode;
            recipe.Size = size;
            recipe.Mode = mode;
            recipe.ReadyIndex = index;
            builder.RequireWork(effects.Work, kForwardWorkEffects);
            recipe.Declared = DeclareEffectResources(builder, program, resources, recipe.Resources);
            FinishEffectRecipe(recipe, program, "ClampSampler"); }, PrepareComputeEffect, ExecuteComputeEffect);
}
RgTextureValue ScalarEffect(EffectGraphBuilder& effects, const ForwardEffectPrograms& programs, uint32_t effect, std::string_view name,
                            RenderExtent size, RenderExtent inputSize, RgTextureValue a, RgTextureValue b = {}, Eigen::Vector4f options = Eigen::Vector4f::Zero()) {
    const auto output = Texture(effects.Graph, size, TextureFormat::R32_FLOAT, kScalarUsage, name);
    auto values = EffectValues(size, inputSize);
    values.Options = options;
    const ForwardPassResource resources[]{
        {.Declaration = "InputA", .Texture = a}, {.Declaration = "InputB", .Texture = b}, {.Declaration = "OutputScalar", .Write = true, .Texture = output}};
    Compute(effects, name, *programs.Programs[effect].Get(), values, resources, size);
    return output;
}
RgTextureValue ColorEffect(EffectGraphBuilder& effects, const ForwardEffectPrograms& programs, uint32_t effect, std::string_view name,
                           RenderExtent size, RenderExtent inputSize, RgTextureValue a, RgTextureValue b = {}) {
    const auto output = Texture(effects.Graph, size, TextureFormat::RGBA16_FLOAT, kHdrUsage, name);
    const auto values = EffectValues(size, inputSize);
    const ForwardPassResource resources[]{
        {.Declaration = "InputA", .Texture = a}, {.Declaration = "InputB", .Texture = b}, {.Declaration = "OutputColor", .Write = true, .Texture = output}};
    Compute(effects, name, *programs.Programs[effect].Get(), values, resources, size);
    return output;
}
MaterialPipelineState EffectRasterState(bool depth = false, bool additive = false) {
    MaterialPipelineState state;
    state.Primitive.Cull = render::CullMode::None;
    state.DepthStencil.DepthTestEnable = depth;
    state.DepthStencil.DepthWriteEnable = false;
    state.DepthStencil.DepthCompare = render::CompareFunction::LessEqual;
    if (additive) {
        state.Blend = render::BlendState::Default();
        state.Blend->Color = {render::BlendFactor::One, render::BlendFactor::One, render::BlendOperation::Add};
    }
    return state;
}
RgPassHandle Composite(EffectGraphBuilder& effects, ShaderProgram& program, const Forward_EffectsData& values,
                       std::span<const ForwardPassResource> inputs, RgTextureValue& output,
                       render::LoadAction load, render::RenderBackend backend, std::string_view name, EffectValueMode mode) {
    auto& graph = effects.Graph;
    output = graph.NextVersion(output);
    const uint32_t index = effects.ReadyCount++;
    return graph.AddTemplateRasterPass<EffectRecipe>(name, effects.Frame, [&](EffectRecipe& recipe, RenderGraphRasterBuilder& builder) {
            recipe.State = EffectRasterState();
            recipe.Extent = values.Extent;
            recipe.Options = values.Options;
            recipe.DebugMode = values.DebugMode;
            recipe.Mode = mode;
            recipe.OutputViewport = true;
            recipe.ReadyIndex = index;
            recipe.Backend = backend;
            builder.RequireWork(effects.Work, kForwardWorkEffects);
            recipe.Declared = DeclareEffectResources(builder, program, inputs, recipe.Resources) && builder.SetColorAttachment(0, output, {.Load = load}).IsValid();
            FinishEffectRecipe(recipe, program, "ClampSampler"); }, PrepareRasterEffect, ExecuteRasterEffect);
}
void DrawSky(EffectGraphBuilder& effects, const ForwardEffectPrograms& programs, RenderExtent size,
             RgTextureValue& hdr, RgTextureValue depth, render::RenderBackend backend) {
    hdr = effects.Graph.NextVersion(hdr);
    const uint32_t index = effects.ReadyCount++;
    effects.Graph.AddTemplateRasterPass<EffectRecipe>("Forward.Sky", effects.Frame, [&](EffectRecipe& recipe, RenderGraphRasterBuilder& builder) {
            recipe.State = EffectRasterState(true);
            recipe.Extent = EffectValues(size, size).Extent;
            recipe.ReadyIndex = index;
            recipe.Backend = backend;
            builder.RequireWork(effects.Work, kForwardWorkEffects);
            recipe.Declared = builder.SetColorAttachment(0, hdr, {.Load = render::LoadAction::Load}).IsValid() &&
                builder.SetDepthAttachment(depth, {.Load = render::LoadAction::Load, .ReadOnly = true}).IsValid();
            FinishEffectRecipe(recipe, *programs.Programs[10].Get(), "ClampSampler"); }, PrepareRasterEffect, ExecuteRasterEffect);
}
void Fireflies(EffectGraphBuilder& effects, const ForwardEffectPrograms& programs, RenderExtent size,
               RgTextureValue& hdr, RgTextureValue depth, render::RenderBackend backend) {
    auto& graph = effects.Graph;
    constexpr uint32_t count = 128;
    const auto particles = graph.CreateBuffer({count * 16, render::MemoryType::Device, render::BufferUse::Resource | render::BufferUse::UnorderedAccess, {}}, "Forward.Fireflies.Instances");
    const auto arguments = graph.CreateBuffer({16, render::MemoryType::Device, render::BufferUse::Indirect | render::BufferUse::UnorderedAccess, {}}, "Forward.Fireflies.Arguments");
    auto values = EffectValues({count, 1}, size);
    values.Options = Eigen::Vector4f{0, float(count), 0, 0};
    const ForwardPassResource writes[]{
        {.Declaration = "Particles", .Write = true, .Buffer = particles, .StructureByteStride = 16},
        {.Declaration = "Arguments", .Write = true, .Buffer = arguments, .StructureByteStride = 4}};
    Compute(effects, "Forward.Fireflies.Update", *programs.Programs[11].Get(), values, writes, {count, 1}, EffectValueMode::Fireflies);
    const ForwardPassResource reads[]{{.Declaration = "Particles", .Buffer = particles, .StructureByteStride = 16}};
    hdr = graph.NextVersion(hdr);
    const uint32_t index = effects.ReadyCount++;
    graph.AddTemplateRasterPass<EffectRecipe>("Forward.Fireflies.Draw", effects.Frame, [&](EffectRecipe& recipe, RenderGraphRasterBuilder& builder) {
            recipe.State = EffectRasterState(true, true);
            recipe.Extent = EffectValues(size, size).Extent;
            recipe.ReadyIndex = index;
            recipe.Backend = backend;
            recipe.Arguments = builder.ReadIndirectArguments(arguments, RgIndirectCommand::Draw, 0, 1);
            builder.RequireWork(effects.Work, kForwardWorkEffects);
            auto& program = *programs.Programs[12].Get();
            recipe.Declared = DeclareEffectResources(builder, program, reads, recipe.Resources) &&
                builder.SetColorAttachment(0, hdr, {.Load = render::LoadAction::Load}).IsValid() &&
                builder.SetDepthAttachment(depth, {.Load = render::LoadAction::Load, .ReadOnly = true}).IsValid() && recipe.Arguments.IsValid();
            FinishEffectRecipe(recipe, program, "ClampSampler"); }, PrepareRasterEffect, ExecuteRasterEffect);
}

using LocalLightGpu = ForwardHdrView::LocalLight;
static_assert(sizeof(LocalLightGpu) == 64 && std::is_trivially_copyable_v<LocalLightGpu>);
uint32_t PackLights(ForwardHdrView& work, const CullingResults& culling,
                    uint32_t maxCount, bool& warned) {
    RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.LightInputs");
    vector<VisibleLight> selected;
    for (const auto& visible : culling.Lights) {
        const auto type = culling.Scene->Lights[visible.Light].Type;
        if (type == LightType::Point || type == LightType::Spot) selected.push_back(visible);
    }
    std::stable_sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) { return a.DistanceSquared < b.DistanceSquared; });
    if (selected.size() > maxCount && !warned) {
        RADRAY_WARN_LOG("Forward local light limit {} exceeded; stable nearest selection is applied", maxCount);
        warned = true;
    }
    selected.resize(std::min<size_t>(selected.size(), maxCount));
    auto& gpu = work.Lights;
    gpu.assign(maxCount, {});
    for (size_t i = 0; i < selected.size(); ++i) {
        const auto& light = culling.Scene->Lights[selected[i].Light];
        const auto& p = light.Parameters;
        auto& packed = gpu[i];
        for (uint32_t axis = 0; axis < 3; ++axis) {
            packed.PositionRadius[axis] = p.WorldPosition[axis];
            packed.ColorType[axis] = p.Color[axis] * p.DiffuseScale;
            packed.DirectionCosOuter[axis] = p.Direction[axis];
        }
        packed.PositionRadius[3] = light.WorldBounds.Radius;
        packed.ColorType[3] = light.Type == LightType::Spot ? 1.f : 0.f;
        packed.DirectionCosOuter[3] = p.SpotAngles.x();
        packed.Cone[0] = p.SpotAngles.y();
    }
    work.LightUpload.Bytes = std::as_bytes(std::span{gpu});
    return static_cast<uint32_t>(selected.size());
}

struct HdrDrawWork {
    ForwardHdrView* Output;
    const RenderSceneSnapshot* Scene;
    RenderPipelineContext* Context;
    shared_ptr<ForwardLitMeshPassProcessor> Processor;
    uint32_t MaxLights;
    RenderValidationMode Validation;
    bool Temporal, ReadOnlyDepth;
    bool* Warned;
    shared_ptr<EffectFrame> Effects;
};
bool PrepareHdrDrawWork(HdrDrawWork& request, uint64_t mask) {
    RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.Prepare");
    auto& work = *request.Output;
    auto& main = work.Main;
    main.RequestedRoles = mask;
    if (mask & kForwardWorkEffects) PrepareEffectInputs(*request.Effects);
    if (!(mask & (kForwardWorkDepth | kForwardWorkOpaque | kForwardWorkTransparent | kForwardWorkLights))) return true;
    if (request.Temporal && (mask & (kForwardWorkDepth | kForwardWorkOpaque | kForwardWorkTransparent))) {
        RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.TemporalInputs");
        if (!request.Context->PreparePrimitiveHistory(main.View, *request.Scene)) return false;
    }
    ++main.CullCalls;
    Eigen::Matrix4f cullMatrix;
    {
        RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.Inputs");
        auto projection = UnjitteredProjection(main.View);
        const auto& rect = main.View.ViewRect;
        projection.row(0) *= float(rect.Width) / float(rect.Width + 2);
        projection.row(1) *= float(rect.Height) / float(rect.Height + 2);
        cullMatrix = projection * main.View.View;
    }
    if (!Cull({request.Scene, &main.View, 0xffffffffu, cullMatrix}, main.Culling)) return false;
    request.Processor->ResetView();
    array<RendererListDesc, 3> descriptions;
    array<RendererList*, 3> outputs{};
    uint32_t count = 0;
    const auto add = [&](uint64_t bit, std::string_view name, std::string_view pass, RenderQueueRange queue,
                         RendererListSorting sorting, PassPolicyId policy, RendererList& out) {
        if (!(mask & bit)) return;
        descriptions[count] = {string{name}, string{pass}, &main.Culling, &main.View, queue, 0xffffffffu, sorting, true, request.Validation, policy};
        outputs[count++] = &out;
    };
    add(kForwardWorkDepth, "DepthNormalsMotion", "DepthNormalsMotion", RenderQueueRange::Opaque(), RendererListSorting::FrontToBack, kDepthNormalsMotionPolicy, main.DepthOnly);
    add(kForwardWorkOpaque, "Opaque", "ForwardLit", RenderQueueRange::Opaque(), RendererListSorting::StateThenFrontToBack,
        request.ReadOnlyDepth ? kForwardLitReadOnlyDepthPolicy : kForwardLitPolicy, main.Opaque);
    add(kForwardWorkTransparent, "Transparent", "ForwardLit", RenderQueueRange::Transparent(), RendererListSorting::BackToFront, kForwardLitPolicy, main.Transparent);
    if (count && !BuildRendererLists(std::span{descriptions.data(), count}, *request.Processor, std::span{outputs.data(), count})) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!outputs[i]->Stats.ContentSucceeded()) return false;
    if (mask & kForwardWorkLights) work.LightCount = PackLights(work, main.Culling, request.MaxLights, *request.Warned);
    work.ContentValid = true;
    return true;
}

struct ShadowDrawWork {
    ForwardHdrView* Output;
    const RenderSceneSnapshot* Scene;
    shared_ptr<ForwardLitMeshPassProcessor> Processor;
    ResolvedRenderView Main;
    ForwardPipelineSettings Settings;
    RenderValidationMode Validation;
};
bool PrepareShadowDrawWork(ShadowDrawWork& request, uint64_t mask) {
    RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.Prepare");
    auto& work = *request.Output;
    const auto& main = request.Main;
    const auto& scene = *request.Scene;
    const auto& settings = request.Settings;
    bool enabled = false;
    {
        RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.Inputs");
        auto& values = work.ShadowValues;
        values = {};
        const array<float4x4*, 4> matrices{&values.ShadowMatrix0, &values.ShadowMatrix1, &values.ShadowMatrix2, &values.ShadowMatrix3};
        Nullable<const RenderLightData*> sun{nullptr};
        for (const auto& light : scene.Lights) {
            if (light.Type == LightType::Directional) {
                sun = &light;
                break;
            }
        }
        enabled = settings.Shadows && sun && sun->CastShadow;
        const auto inverse = (UnjitteredProjection(main) * main.View).inverse().eval();
        array<Eigen::Vector3f, 4> nearCorners, farCorners;
        for (uint32_t i = 0; i < 4; ++i) {
            const float x = i & 1 ? 1.f : -1.f, y = i & 2 ? 1.f : -1.f;
            Eigen::Vector4f a = inverse * Eigen::Vector4f{x, y, 0, 1};
            Eigen::Vector4f b = inverse * Eigen::Vector4f{x, y, 1, 1};
            nearCorners[i] = a.head<3>() / a.w();
            farCorners[i] = b.head<3>() / b.w();
        }
        const float nearZ = std::max(.001f, (main.View * Eigen::Vector4f{nearCorners[0].x(), nearCorners[0].y(), nearCorners[0].z(), 1}).z());
        const float cameraFar = (main.View * Eigen::Vector4f{farCorners[0].x(), farCorners[0].y(), farCorners[0].z(), 1}).z();
        const float farZ = std::min(cameraFar, std::max(nearZ + .1f, settings.ShadowDistance));
        float previous = nearZ;
        for (uint32_t cascade = 0; cascade < 4; ++cascade) {
            auto& draw = work.Cascades[cascade];
            const float ratio = float(cascade + 1) / 4;
            const float split = .65f * nearZ * std::pow(farZ / nearZ, ratio) + .35f * (nearZ + (farZ - nearZ) * ratio);
            if (!(mask & (uint64_t{1} << cascade))) {
                previous = split;
                continue;
            }
            array<Eigen::Vector3f, 8> corners;
            Eigen::Vector3f center = Eigen::Vector3f::Zero();
            for (uint32_t i = 0; i < 4; ++i)
                for (uint32_t end = 0; end < 2; ++end) {
                    corners[i + end * 4] = nearCorners[i] + (farCorners[i] - nearCorners[i]) * (((end ? split : previous) - nearZ) / std::max(.001f, cameraFar - nearZ));
                    center += corners[i + end * 4] / 8;
                }
            float radius = .1f;
            for (const auto& point : corners) radius = std::max(radius, (point - center).norm());
            radius = std::ceil(radius * 16) / 16;
            const Eigen::Vector3f direction = sun ? sun->Parameters.Direction.normalized().eval() : Eigen::Vector3f::UnitZ().eval();
            const Eigen::Vector3f up = std::abs(direction.y()) > .95f ? Eigen::Vector3f::UnitX().eval() : Eigen::Vector3f::UnitY().eval();
            const Eigen::Matrix4f orientation = LookAtFrontLH(Eigen::Vector3f::Zero().eval(), direction, up);
            Eigen::Vector3f lightCenter = (orientation * Eigen::Vector4f{center.x(), center.y(), center.z(), 1}).head<3>();
            const float texel = 2 * radius / settings.ShadowResolution;
            lightCenter.x() = std::floor(lightCenter.x() / texel) * texel;
            lightCenter.y() = std::floor(lightCenter.y() / texel) * texel;
            const Eigen::Vector3f snapped = orientation.block<3, 3>(0, 0).transpose() * lightCenter;
            const Eigen::Vector3f eye = snapped - direction * (radius + settings.ShadowDistance);
            draw.View.View = LookAtFrontLH(eye, direction, up);
            draw.View.Projection = OrthoLH(-radius, radius, -radius, radius, 0.f, 2 * (radius + settings.ShadowDistance));
            draw.View.ViewProjection = draw.View.Projection * draw.View.View;
            draw.View.PreviousViewProjection = draw.View.ViewProjection;
            draw.View.PreviousViewValid = false;
            draw.View.JitterNdc.setZero();
            draw.View.WorldPosition = eye;
            draw.View.ViewRect = draw.View.ScissorRect = {0, 0, settings.ShadowResolution, settings.ShadowResolution};
            *matrices[cascade] = draw.View.ViewProjection;
            values.ShadowSphere[cascade] = Eigen::Vector4f{center.x(), center.y(), center.z(), radius * radius};
            values.ShadowBias[cascade] = Eigen::Vector4f{texel, texel * 2, 0, 0};
            previous = split;
        }
        values.ShadowParams = Eigen::Vector4f{enabled ? 1.f : 0.f, float(settings.ShadowResolution), 4, 1};
    }
    for (uint32_t cascade = 0; cascade < work.Cascades.size(); ++cascade) {
        if (!(mask & (uint64_t{1} << cascade))) continue;
        auto& draw = work.Cascades[cascade];
        draw.RequestedRoles = kForwardWorkDepth;
        if (!enabled) continue;
        ++draw.CullCalls;
        if (!Cull({request.Scene, &draw.View}, draw.Culling)) return false;
        request.Processor->ResetView();
        if (!BuildRendererList({"ShadowCaster", "ShadowCaster", &draw.Culling, &draw.View, RenderQueueRange::Opaque(), 0xffffffffu,
                                RendererListSorting::FrontToBack, true, request.Validation, kShadowCasterPolicy},
                               *request.Processor, draw.DepthOnly) ||
            !draw.DepthOnly.Stats.ContentSucceeded()) return false;
    }
    return true;
}
struct ShadowTemplate {
    uint64_t ViewId{0}, Used{0};
    uint32_t Resolution{0};
    shared_ptr<const RenderGraphTemplate> Graph;
    RgTextureValue Output;
    RgTemplateSlot<ShadowDrawWork> WorkSlot;
    RgWorkHandle Work;
    array<RgTemplateSlot<ForwardGraphFrameData>, 4> Stages;
};
struct SurfaceTemplateKey {
    RenderOutputId Source;
    TexturePoolKey Texture;
    friend bool operator==(const SurfaceTemplateKey&, const SurfaceTemplateKey&) = default;
};
struct HdrTemplateKey {
    uint64_t ViewId{0};
    RenderExtent Size;
    TexturePoolKey Output, Shadow;
    ForwardPipelineSettings Settings;
    array<ShaderProgram*, 15> Programs;
    array<uint64_t, 15> Generations;
    vector<SurfaceTemplateKey> Surfaces;
    bool Auxiliary{false}, FirstOutput{false}, HistoryValid{false};
    friend bool operator==(const HdrTemplateKey&, const HdrTemplateKey&) = default;
};
struct HdrTemplate {
    HdrTemplateKey Key;
    shared_ptr<const RenderGraphTemplate> Graph;
    RgTemplateSlot<EffectFrame> Effects;
    RgTemplateSlot<HdrDrawWork> WorkSlot;
    RgTemplateSlot<RgUploadData> LightUpload;
    RgWorkHandle Work;
    array<RgTemplateSlot<ForwardGraphFrameData>, 3> Stages;
    RgTexturePort OutputInput, ShadowInput;
    vector<RgTexturePort> SurfaceInputs;
    RgTextureValue PreviousColor, PreviousDepth, CurrentColor, CurrentDepth;
    RgTextureValue Output;
    RgPassHandle Completion;
    uint32_t ReadyCount{0};
    uint64_t Used{0};
};
struct OverlayTemplate {
    TexturePoolKey SourceDesc, OutputDesc;
    uint64_t ProgramGeneration{0}, Used{0};
    ShaderProgram* Program{nullptr};
    shared_ptr<const RenderGraphTemplate> Graph;
    RgTemplateSlot<EffectFrame> Frame;
    RgTexturePort Source, Destination;
    RgTextureValue Output;
    uint32_t ReadyCount{0};
};
template <class Entry>
Entry& InsertTemplate(vector<Entry>& entries, Entry entry, size_t maximum) {
    if (entries.size() < maximum) {
        entries.push_back(std::move(entry));
        return entries.back();
    }
    auto& oldest = *std::min_element(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.Used < b.Used; });
    oldest = std::move(entry);
    return oldest;
}

vector<ForwardPassResource> LitResources(RgTextureValue shadows, RgBufferValue lights, RgBufferValue headers, RgBufferValue indices,
                                         RgTextureValue ao, RgTextureValue opaqueColor) {
    return {
        {.Declaration = "ShadowMap", .Texture = shadows},
        {.Declaration = "AmbientOcclusion", .Texture = ao},
        {.Declaration = "OpaqueColor", .Texture = opaqueColor},
        {.Declaration = "LocalLights", .Buffer = lights, .StructureByteStride = sizeof(LocalLightGpu)},
        {.Declaration = "TileHeaders", .Buffer = headers, .StructureByteStride = 8},
        {.Declaration = "TileIndices", .Buffer = indices, .StructureByteStride = 4}};
}

}  // namespace

struct ForwardEffectTemplates::Impl {
    vector<ShadowTemplate> Shadows;
    vector<HdrTemplate> Views;
    vector<OverlayTemplate> Overlays;
    uint64_t UseSerial{0};
};
ForwardEffectTemplates::ForwardEffectTemplates() : _impl(make_unique<Impl>()) {}
ForwardEffectTemplates::~ForwardEffectTemplates() = default;

bool ForwardEffectPrograms::Initialize(RenderSystem& system) {
    static constexpr std::string_view sources[]{"linear_depth", "depth_pyramid", "ambient_occlusion", "ao_blur", "temporal_resolve", "bloom", "bloom", "bloom", "output", "tile_lights", "sky", "firefly_update", "firefly_draw", "debug", "output_surface"};
    for (uint32_t effect = 0; effect < Programs.size(); ++effect) {
        if (!Programs[effect]) Programs[effect] = system.GetOrCreateShaderProgram({.SourceName = fmt::format("shaderlib/pipelines/forward/{}.hlsl", sources[effect]), .Defines = {{"FORWARD_EFFECT", std::to_string(effect)}}});
        if (!Programs[effect]) return false;
    }
    return true;
}
bool ForwardViewSignature::Matches(const ForwardViewSignature& other) const noexcept {
    // Numeric controls are temporal inputs. Effects after TAA do not invalidate opaque history.
    auto a = Settings, b = other.Settings;
    a.DebugView = b.DebugView;
    a.Exposure = b.Exposure;
    a.Bloom = b.Bloom;
    a.BloomStrength = b.BloomStrength;
    a.Fireflies = b.Fireflies;
    a.AoRadius = b.AoRadius;
    a.ShadowDistance = b.ShadowDistance;
    return Extent == other.Extent && ViewRect.X == other.ViewRect.X && ViewRect.Y == other.ViewRect.Y &&
           ViewRect.Width == other.ViewRect.Width && ViewRect.Height == other.ViewRect.Height && OutputFormat == other.OutputFormat &&
           Auxiliary == other.Auxiliary && a == b;
}
void ForwardHdrView::Reset() {
    Main.ResetForReuse();
    for (auto& cascade : Cascades) cascade.ResetForReuse();
    Execution = {};
    Completion = {};
    ContentValid = false;
    PassesSucceeded = true;
    LightUpload = {};
    LightCount = 0;
    TemporalHistory = HistoryValid = false;
}

bool DeclareForwardSharedShadows(RenderGraph& graph, ForwardEffectTemplates& templates, const ForwardPipelineSettings& settings, const ResolvedRenderView& primary,
                                 const RenderSceneSnapshot& scene, const PackedCBufferTable& objects, FrameDrawResources& draws, ForwardBindingCache& bindings,
                                 ForwardHdrView& work, render::RenderBackend backend, bool& warned, ForwardShadowAtlas& out) {
    RADRAY_PROFILE_SCOPE_N("DeclareSharedShadows");
    auto& cache = templates.Get();
    Nullable<ShadowTemplate*> found{nullptr};
    for (auto& entry : cache.Shadows)
        if (entry.ViewId == primary.StateId.Value && entry.Resolution == settings.ShadowResolution) {
            found = &entry;
            break;
        }
    if (!found) {
        ShadowTemplate next;
        next.ViewId = primary.StateId.Value;
        next.Resolution = settings.ShadowResolution;
        auto builder = graph.CreateTemplateBuilder("Forward.ShadowAtlas");
        builder.SetResourceView(primary.StateId.Value);
        next.WorkSlot = builder.DeclareTemplateSlot<ShadowDrawWork>();
        next.Work = builder.AddTemplateWork("Forward.ShadowWork", next.WorkSlot, PrepareShadowDrawWork);
        next.Output = Texture(builder, {settings.ShadowResolution, settings.ShadowResolution}, TextureFormat::D32_FLOAT,
                              TextureUse::DepthStencilWrite | TextureUse::Resource, "Forward.Shadows", 1, 4);
        for (uint32_t cascade = 0; cascade < next.Stages.size(); ++cascade) {
            next.Stages[cascade] = builder.DeclareTemplateSlot<ForwardGraphFrameData>();
            const ForwardGraphView view{.Work = next.Work, .WorkMask = uint64_t{1} << cascade};
            RgDepthAttachmentDesc attachment;
            attachment.View.Range = {cascade, 1, 0, 1};
            const auto stage = ForwardGraph::DeclareTemplate(builder, ForwardGraphStage::Depth, next.Stages[cascade],
                                                             {.Name = fmt::format("Forward.Shadow.{}", cascade), .Views = std::span{&view, 1}, .Depth = next.Output, .DepthAttachment = attachment});
            if (!stage.Success) return false;
            next.Output = stage.Depth;
        }
        next.Graph = builder.FreezeTemplate();
        if (!next.Graph) return false;
        found = &InsertTemplate(cache.Shadows, std::move(next), 4);
    }
    auto& recipe = *found;
    recipe.Used = ++cache.UseSerial;
    const auto instance = graph.Instantiate(recipe.Graph);
    auto processor = [&] {
        RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.Processor");
        return make_shared<ForwardLitMeshPassProcessor>(draws, bindings, warned, objects);
    }();
    if (!instance.IsValid() || !instance.Bind(recipe.WorkSlot, [&] {
            RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.EffectFrameInputs");
            return make_shared<ShadowDrawWork>(ShadowDrawWork{&work, &scene, std::move(processor), primary, settings, graph.GetRuntimeOptions().Validation});
        }())) return false;
    for (uint32_t cascade = 0; cascade < recipe.Stages.size(); ++cascade) {
        auto& draw = work.Cascades[cascade];
        {
            RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.ViewFrameInputs");
            draw.View = primary;
            draw.View.ViewRect = draw.View.ScissorRect = {0, 0, settings.ShadowResolution, settings.ShadowResolution};
        }
        draw.Work = instance.Value(recipe.Work);
        const ForwardGraphView view = [&] {
            RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.PassViewInputs");
            return ForwardGraphView{.View = draw.View, .List = &draw.DepthOnly};
        }();
        if (!instance.Bind(recipe.Stages[cascade], ForwardGraph::MakeFrame({.Backend = backend, .Views = std::span{&view, 1}, .Execution = &work.Execution}))) return false;
    }
    work.ContentValid = true;
    out = {instance.Value(recipe.Output), &work.ShadowValues};
    return out.Texture.IsValid();
}

namespace {
bool BuildOutputSurfaces(EffectGraphBuilder& effects, ShaderProgram& program,
                         RgTextureValue& color, RgTextureValue depth, render::RenderBackend backend,
                         std::span<const RgTextureValue> sources) {
    auto& graph = effects.Graph;
    for (uint32_t index = 0; index < sources.size(); ++index) {
        const auto texture = sources[index];
        const auto descriptor = graph.GetTextureDescriptor(texture);
        if (!descriptor || descriptor->SampleCount != 1 || descriptor->Dim != render::TextureDimension::Dim2D || !descriptor->Usage.HasFlag(TextureUse::Resource)) {
            graph.AddDiagnostic("ForwardOutputSurface", "Screen source requires a sampleable single-sample 2D output");
            return false;
        }
        const auto format = descriptor->Format;
        const bool srgb = format == TextureFormat::RGBA8_UNORM_SRGB || format == TextureFormat::BGRA8_UNORM_SRGB;
        if (!srgb && format != TextureFormat::RGBA8_UNORM && format != TextureFormat::BGRA8_UNORM && format != TextureFormat::RGBA16_FLOAT) {
            graph.AddDiagnostic("ForwardOutputSurface", "Screen source must contain an SDR scene output");
            return false;
        }
        const ForwardPassResource resources[]{{.Declaration = "SceneOutput", .Texture = texture}};
        color = graph.NextVersion(color);
        const uint32_t readyIndex = effects.ReadyCount++;
        graph.AddTemplateRasterPass<EffectRecipe>("Forward.OutputSurface", effects.Frame, [&](EffectRecipe& recipe, RenderGraphRasterBuilder& builder) {
                recipe.State = EffectRasterState(true);
                recipe.Options = Eigen::Vector4f{0, srgb || format == TextureFormat::RGBA16_FLOAT ? 0.f : 1.f, 0, 0};
                recipe.Mode = EffectValueMode::Surface;
                recipe.SurfaceIndex = index;
                recipe.ReadyIndex = readyIndex;
                recipe.Backend = backend;
                recipe.Vertices = 6;
                builder.RequireWork(effects.Work, kForwardWorkEffects);
                recipe.Declared = DeclareEffectResources(builder, program, resources, recipe.Resources) &&
                    builder.SetColorAttachment(0, color, {.Load = render::LoadAction::Load}).IsValid() &&
                    builder.SetDepthAttachment(depth, {.Load = render::LoadAction::Load, .ReadOnly = true}).IsValid();
                FinishEffectRecipe(recipe, program, "OutputSampler"); }, PrepareRasterEffect, ExecuteRasterEffect);
    }
    return true;
}
}  // namespace

// The top-level ForwardPipeline appends this view's stages to the same frame graph.
bool BuildForwardHdrView(RenderGraph& graph, ForwardEffectTemplates& templates, RenderPipelineContext& context, render::Device& device,
                         const ForwardEffectPrograms& programs, const ForwardPipelineSettings& settings,
                         const ResolvedRenderViewFamily& family, const ResolvedRenderView& sourceView,
                         const RenderSceneSnapshot& scene, const PackedCBufferTable& objects, FrameDrawResources& draws, ForwardBindingCache& bindings,
                         ForwardHdrView& work, bool firstOutputView, bool& lightOverflowWarned,
                         std::span<const ForwardOutputSurface> surfaces, std::span<RenderGraphOutputBinding> outputs,
                         const ForwardShadowAtlas& shadows, bool auxiliary, shared_ptr<ForwardLitMeshPassProcessor> sharedLit) {
    RADRAY_PROFILE_SCOPE_N("BuildForwardHdrView");
    const bool temporal = !auxiliary && settings.Antialiasing == ForwardAntialiasing::Temporal;
    const bool msaa = !auxiliary && settings.Antialiasing == ForwardAntialiasing::Msaa4;
    const bool writeDepthInOpaque = msaa || auxiliary;
    auto viewSettings = settings;
    if (auxiliary) viewSettings.AmbientOcclusion = viewSettings.Bloom = viewSettings.Fireflies = false;
    const uint32_t samples = msaa ? 4 : 1;
    const RenderExtent size{sourceView.ViewRect.Width, sourceView.ViewRect.Height};
    if (!size.Width || !size.Height || !family.OutputAvailable || family.SampleCount != 1) return false;
    auto& view = work.Main.View;
    {
        RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.ViewFrameInputs");
        work.Main.View = sourceView;
        view.ViewRect = {0, 0, size.Width, size.Height};
        view.ScissorRect = {sourceView.ScissorRect.X - sourceView.ViewRect.X, sourceView.ScissorRect.Y - sourceView.ViewRect.Y,
                            sourceView.ScissorRect.Width, sourceView.ScissorRect.Height};
    }
    HistoryTexturePair colorHistory, depthHistory;
    {
        RADRAY_PROFILE_SCOPE_N("HdrHistoryAcquire");
        if (temporal) {
            string reason;
            RuntimeTextureDesc history;
            history.Extent.Width = size.Width;
            history.Extent.Height = size.Height;
            history.Format = TextureFormat::RGBA16_FLOAT;
            history.Usage = kHdrUsage;
            colorHistory = context.AcquireHistoryTexture(sourceView, family, {"Forward.Color.v1", "Forward.ColorHistory", history, 3, HistoryCommitMode::WithView}, reason);
            history.Format = TextureFormat::R32_FLOAT;
            history.Usage = kScalarUsage;
            depthHistory = context.AcquireHistoryTexture(sourceView, family, {"Forward.Depth.v1", "Forward.DepthHistory", history, 3, HistoryCommitMode::WithView}, reason);
            if (!colorHistory.Current || !depthHistory.Current) {
                RADRAY_ERR_LOG("Forward history allocation failed: {}", reason);
                return false;
            }
        }
    }
    if (!shadows.Texture.IsValid() || !shadows.Values) return false;
    const auto outputBinding = FindGraphOutput(outputs, family.OutputId);
    const auto outputDesc = outputBinding ? graph.GetTextureDescriptor(outputBinding->Texture) : std::nullopt;
    const auto shadowDesc = graph.GetTextureDescriptor(shadows.Texture);
    if (!outputBinding || !outputDesc || !shadowDesc) return false;
    const bool historyValid = temporal && colorHistory.PreviousValid && depthHistory.PreviousValid && view.PreviousViewValid;
    vector<ForwardOutputSurface> activeSurfaces;
    vector<RgTextureValue> surfaceValues;
    HdrTemplateKey key;
    key.ViewId = sourceView.StateId.Value;
    key.Size = size;
    key.Output = {*outputDesc};
    key.Shadow = {*shadowDesc};
    key.Settings = viewSettings;
    key.Settings.RenderScale = 1;
    key.Settings.Exposure = key.Settings.ShadowDistance = key.Settings.AoRadius = key.Settings.BloomStrength = 0;
    key.Auxiliary = auxiliary;
    key.FirstOutput = firstOutputView;
    key.HistoryValid = historyValid;
    for (size_t index = 0; index < programs.Programs.size(); ++index) {
        key.Programs[index] = programs.Programs[index].Get();
        key.Generations[index] = key.Programs[index]->GetGeneration();
    }
    for (const auto& surface : surfaces) {
        if (surface.Destination != family.OutputId || !(surface.LayerMask & view.LayerMask)) continue;
        const auto source = FindGraphOutput(outputs, surface.Source);
        const auto descriptor = source ? graph.GetTextureDescriptor(source->Texture) : std::nullopt;
        if (!source || !descriptor) return false;
        key.Surfaces.push_back({surface.Source, {*descriptor}});
        activeSurfaces.push_back(surface);
        surfaceValues.push_back(source->Texture);
    }
    auto& cache = templates.Get();
    Nullable<HdrTemplate*> found{nullptr};
    for (auto& entry : cache.Views)
        if (entry.Key == key) {
            found = &entry;
            break;
        }
    if (!found) {
        for (const auto format : {TextureFormat::RGBA16_FLOAT, TextureFormat::R32_FLOAT}) {
            const auto supported = device.QueryTextureSupport({render::TextureDimension::Dim2D, format, format == TextureFormat::RGBA16_FLOAT ? kHdrUsage : kScalarUsage});
            if (!supported.Supported) {
                RADRAY_ERR_LOG("Forward HDR requires storage/sampled format {}", format);
                return false;
            }
        }
        HdrTemplate next;
        next.Key = std::move(key);
        auto declaration = graph.CreateTemplateBuilder("Forward.HdrView");
        auto& graph = declaration;
        graph.SetResourceView(sourceView.StateId.Value);
        next.Effects = graph.DeclareTemplateSlot<EffectFrame>();
        next.WorkSlot = graph.DeclareTemplateSlot<HdrDrawWork>();
        next.LightUpload = graph.DeclareTemplateSlot<RgUploadData>();
        next.Work = graph.AddTemplateWork("Forward.MainViewWork", next.WorkSlot, PrepareHdrDrawWork);
        EffectGraphBuilder effects{graph, next.Effects, next.Work};
        work.Main.Work = next.Work;
        next.OutputInput = graph.DeclareTexturePort(*outputDesc, "Forward.OutputInput");
        next.ShadowInput = graph.DeclareTexturePort(*shadowDesc, "Forward.ShadowInput");
        vector<RgTextureValue> surfaceInputs;
        for (const auto& surface : next.Key.Surfaces) {
            const auto port = graph.DeclareTexturePort(surface.Texture.Desc, "Forward.SurfaceInput");
            next.SurfaceInputs.push_back(port);
            surfaceInputs.push_back(graph.Value(port));
        }
        for (auto& stage : next.Stages) stage = graph.DeclareTemplateSlot<ForwardGraphFrameData>();
        auto depth = Texture(graph, size, TextureFormat::D32_FLOAT, TextureUse::DepthStencilWrite | TextureUse::DepthStencilRead | (msaa || auxiliary ? TextureUses{} : TextureUses{TextureUse::Resource}), "Forward.Depth", samples);
        auto hdr = Texture(graph, size, TextureFormat::RGBA16_FLOAT, msaa ? TextureUse::RenderTarget | TextureUse::CopySource : kHdrUsage | TextureUse::RenderTarget, "Forward.HDR", samples);
        auto normals = msaa || auxiliary ? RgTextureValue{} : Texture(graph, size, TextureFormat::RGBA16_FLOAT, kHdrUsage | TextureUse::RenderTarget, "Forward.Normals");
        const auto motion = msaa || auxiliary ? RgTextureValue{} : Texture(graph, size, TextureFormat::RGBA16_FLOAT, kHdrUsage | TextureUse::RenderTarget, "Forward.Motion");
        const auto shadowTexture = graph.Value(next.ShadowInput);
        if (!msaa && !auxiliary) {
            RADRAY_PROFILE_SCOPE_N("HdrPrepass");
            const ForwardGraphView draw = [&] {
                RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.PassViewInputs");
                return ForwardGraphView{.View = view, .List = &work.Main.DepthOnly, .Work = work.Main.Work, .WorkMask = kForwardWorkDepth};
            }();
            const RgTextureValue prepassAux[]{motion};
            const auto prepassStage = ForwardGraph::DeclareTemplate(graph, ForwardGraphStage::Opaque, next.Stages[0],
                                                                    {.Name = "Forward.DepthNormalsMotion", .Backend = device.GetBackend(), .Views = std::span{&draw, 1}, .Color = normals, .Depth = depth, .Execution = &work.Execution, .PreserveEmptyPass = true, .AuxiliaryColors = prepassAux});
            work.PassesSucceeded &= prepassStage.Success;
            normals = prepassStage.Color;
            depth = prepassStage.Depth;
        }
        // Neutral resources remain real graph producers so disabled effects have explicit valid inputs.
        const auto neutral = Texture(graph, {1, 1}, TextureFormat::RGBA16_FLOAT, kHdrUsage | TextureUse::RenderTarget, "Forward.Neutral");
        struct Empty {};
        graph.AddTemplateRasterPass<Empty>("Forward.Neutral", next.Effects, [=](Empty&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, neutral, {.Clear = {1, 1, 1, 1}}); }, +[](const Empty&, EffectFrame&, RenderGraphPrepareContext&) { return true; }, +[](const Empty&, const EffectFrame&, RenderGraphRasterContext&) {});
        RgTextureValue linearDepth = neutral, ao = neutral, pyramid = neutral;
        uint32_t pyramidLevels = 1;
        if (!msaa && !auxiliary) {
            RADRAY_PROFILE_SCOPE_N("HdrDepthPyramidAo");
            linearDepth = ScalarEffect(effects, programs, 0, "Forward.LinearDepth", size, size, depth);
            const auto halfSize = Half(size);
            for (auto dimension = std::max(halfSize.Width, halfSize.Height); dimension > 1; dimension >>= 1) ++pyramidLevels;
            pyramid = graph.CreateTexture({render::TextureDimension::Dim2D, halfSize.Width, halfSize.Height, 1, pyramidLevels, 1, TextureFormat::R32_FLOAT, render::MemoryType::Device, kScalarUsage, {}}, "Forward.DepthPyramid");
            auto previousSize = size;
            for (uint32_t mip = 0; mip < pyramidLevels; ++mip) {
                const RenderExtent mipSize{std::max(1u, halfSize.Width >> mip), std::max(1u, halfSize.Height >> mip)};
                auto& program = *programs.Programs[1].Get();
                const auto values = EffectValues(mipSize, previousSize);
                const ForwardPassResource resources[]{
                    {.Declaration = "InputA", .Texture = mip == 0 ? linearDepth : pyramid, .TextureView = {.Range = {0, 1, mip == 0 ? 0 : mip - 1, 1}}},
                    {.Declaration = "OutputScalar", .Write = true, .Texture = pyramid, .TextureView = {.Range = {0, 1, mip, 1}}}};
                Compute(effects, fmt::format("Forward.DepthPyramid.{}", mip), program, values, resources, mipSize);
                previousSize = mipSize;
            }
            if (viewSettings.AmbientOcclusion) {
                auto& program = *programs.Programs[2].Get();
                auto values = EffectValues(halfSize, halfSize);
                values.Options = Eigen::Vector4f{settings.AoRadius, 0, 0, 0};
                ao = Texture(graph, halfSize, TextureFormat::R32_FLOAT, kScalarUsage, "Forward.AO");
                const ForwardPassResource resources[]{
                    {.Declaration = "InputA", .Texture = pyramid, .TextureView = {.Range = {0, 1, 0, 1}}},
                    {.Declaration = "InputB", .Texture = normals},
                    {.Declaration = "InputC", .Texture = pyramid, .TextureView = {.Range = {0, 1, std::min(1u, pyramidLevels - 1), 1}}},
                    {.Declaration = "OutputScalar", .Write = true, .Texture = ao}};
                Compute(effects, "Forward.AO", program, values, resources, halfSize, EffectValueMode::AmbientOcclusion);
                ao = ScalarEffect(effects, programs, 3, "Forward.AO.Horizontal", halfSize, halfSize, ao, linearDepth, {1, 0, 0, 0});
                ao = ScalarEffect(effects, programs, 3, "Forward.AO.Vertical", halfSize, halfSize, ao, linearDepth, {0, 1, 0, 0});
            }
        }
        RgBufferValue lights;
        uint32_t count = 0;
        RgBufferValue headers, indices;
        {
            RADRAY_PROFILE_SCOPE_N("HdrLights");
            lights = graph.UploadBuffer("Forward.LocalLights", uint64_t{settings.MaxLocalLights} * sizeof(LocalLightGpu), render::BufferUse::Resource,
                                        next.LightUpload, next.Work, kForwardWorkLights);
            if (!lights.IsValid()) return false;
            const RenderExtent tiles{(size.Width + 15) / 16, (size.Height + 15) / 16};
            const auto tileCount = uint64_t{tiles.Width} * tiles.Height;
            headers = graph.CreateBuffer({tileCount * 8, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Resource, {}}, "Forward.TileHeaders");
            indices = graph.CreateBuffer({tileCount * settings.MaxLightsPerTile * 4, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Resource, {}}, "Forward.TileIndices");
            auto& tileProgram = *programs.Programs[9].Get();
            auto tileValues = EffectValues(tiles, size);
            tileValues.LocalLightCount = count;
            tileValues.TileCapacity = settings.MaxLightsPerTile;
            const ForwardPassResource tileBindings[]{
                {.Declaration = "Lights", .Buffer = lights, .StructureByteStride = sizeof(LocalLightGpu)},
                {.Declaration = "Headers", .Write = true, .Buffer = headers, .StructureByteStride = 8},
                {.Declaration = "Indices", .Write = true, .Buffer = indices, .StructureByteStride = 4}};
            Compute(effects, "Forward.TileLightCull", tileProgram, tileValues, tileBindings, tiles);
        }
        {
            RADRAY_PROFILE_SCOPE_N("HdrOpaqueSky");
            auto opaqueBindings = LitResources(shadowTexture, lights, headers, indices, ao, neutral);
            const ForwardGraphView opaqueView = [&] {
                RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.PassViewInputs");
                return ForwardGraphView{.View = view, .List = &work.Main.Opaque, .Resources = opaqueBindings, .PassValues = ForwardGraphPassValues{shadows.Values.Get(), &work.LightCount, size, settings.MaxLightsPerTile, viewSettings.ForwardPlus, viewSettings.AmbientOcclusion, false}, .Work = work.Main.Work, .WorkMask = kForwardWorkOpaque};
            }();
            const auto opaqueStage = ForwardGraph::DeclareTemplate(graph, ForwardGraphStage::Opaque, next.Stages[1],
                                                                   {.Name = "Forward.Opaque", .Backend = device.GetBackend(), .Views = std::span{&opaqueView, 1}, .Color = hdr, .Depth = depth, .ColorAttachment = {.Clear = {.035f, .06f, .1f, 1}}, .DepthAttachment = {.Load = writeDepthInOpaque ? render::LoadAction::Clear : render::LoadAction::Load, .ReadOnly = !writeDepthInOpaque}, .Execution = &work.Execution});
            work.PassesSucceeded &= opaqueStage.Success;
            hdr = opaqueStage.Color;
            depth = opaqueStage.Depth;
            DrawSky(effects, programs, size, hdr, depth, device.GetBackend());
        }
        auto current = hdr;
        auto currentHdrDebug = hdr, historyHdrDebug = hdr;
        if (settings.DebugView == ForwardDebugView::CurrentHdr && !msaa) {
            currentHdrDebug = Texture(graph, size, TextureFormat::RGBA16_FLOAT, kHdrUsage, "Forward.CurrentHdrDebug");
            graph.AddCopyTexturePass("Forward.CopyCurrentHdrDebug", hdr, currentHdrDebug);
        }
        if (temporal) {
            RADRAY_PROFILE_SCOPE_N("HdrTaa");
            auto& program = *programs.Programs[4].Get();
            auto values = EffectValues(size, size);
            const bool valid = historyValid;
            values.HistoryValid = valid ? 1u : 0u;
            next.PreviousColor = valid ? graph.DeclareExternalTexture(colorHistory.Previous->Desc, "Forward.PreviousColor", RenderGraphExternalAccess::ReadOnly) : RgTextureValue{};
            const auto previous = valid ? next.PreviousColor : hdr;
            next.PreviousDepth = valid ? graph.DeclareExternalTexture(depthHistory.Previous->Desc, "Forward.PreviousDepth", RenderGraphExternalAccess::ReadOnly) : RgTextureValue{};
            const auto previousDepth = valid ? next.PreviousDepth : depth;
            next.CurrentColor = graph.DeclareExternalTexture(colorHistory.Current->Desc, "Forward.CurrentColor", RenderGraphExternalAccess::ReadWrite);
            const auto colorOut = graph.NextVersion(next.CurrentColor);
            next.CurrentDepth = graph.DeclareExternalTexture(depthHistory.Current->Desc, "Forward.CurrentDepth", RenderGraphExternalAccess::ReadWrite);
            const auto depthOut = graph.NextVersion(next.CurrentDepth);
            const ForwardPassResource taa[]{
                {.Declaration = "InputA", .Texture = hdr},
                {.Declaration = "InputB", .Texture = previous},
                {.Declaration = "InputC", .Texture = motion},
                {.Declaration = "InputD", .Texture = depth},
                {.Declaration = "InputE", .Texture = previousDepth},
                {.Declaration = "OutputColor", .Write = true, .Texture = colorOut},
                {.Declaration = "OutputScalar", .Write = true, .Texture = depthOut}};
            Compute(effects, "Forward.TAA", program, values, taa, size);
            graph.ExportTexture(colorOut, render::TextureState::ShaderRead);
            graph.ExportTexture(depthOut, render::TextureState::ShaderRead);
            historyHdrDebug = valid ? previous : colorOut;
            current = colorOut;
        }
        RgTextureValue opaqueCopy = neutral;
        if (current != hdr) {
            hdr = graph.NextVersion(hdr);
            graph.AddCopyTexturePass("Forward.CopyTemporalToMutableHDR", current, hdr);
        }
        work.PassesSucceeded &= BuildOutputSurfaces(effects, *programs.Programs[14].Get(), hdr, depth, device.GetBackend(), surfaceInputs);
        if (!writeDepthInOpaque) {
            opaqueCopy = Texture(graph, size, TextureFormat::RGBA16_FLOAT, kHdrUsage, "Forward.OpaqueColor");
            graph.AddCopyTexturePass("Forward.CopyOpaque", hdr, opaqueCopy);
        }
        {
            RADRAY_PROFILE_SCOPE_N("HdrTransparent");
            auto transparentBindings = LitResources(shadowTexture, lights, headers, indices, ao, opaqueCopy);
            const ForwardGraphView transparentView = [&] {
                RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.PassViewInputs");
                return ForwardGraphView{.View = view, .List = &work.Main.Transparent, .Resources = transparentBindings, .PassValues = ForwardGraphPassValues{shadows.Values.Get(), &work.LightCount, size, settings.MaxLightsPerTile, viewSettings.ForwardPlus, viewSettings.AmbientOcclusion, !writeDepthInOpaque}, .Work = work.Main.Work, .WorkMask = kForwardWorkTransparent};
            }();
            const auto transparentStage = ForwardGraph::DeclareTemplate(graph, ForwardGraphStage::Transparent, next.Stages[2],
                                                                        {.Name = "Forward.Transparent", .Backend = device.GetBackend(), .Views = std::span{&transparentView, 1}, .Color = hdr, .Depth = depth, .ColorAttachment = {.Load = render::LoadAction::Load}, .DepthAttachment = {.Load = render::LoadAction::Load, .ReadOnly = true}, .Execution = &work.Execution});
            work.PassesSucceeded &= transparentStage.Success;
            hdr = transparentStage.Color;
            depth = transparentStage.Depth;
        }
        {
            RADRAY_PROFILE_SCOPE_N("HdrBloomComposite");
            if (viewSettings.Fireflies) Fireflies(effects, programs, size, hdr, depth, device.GetBackend());
            current = hdr;
            if (msaa) {
                current = Texture(graph, size, TextureFormat::RGBA16_FLOAT, kHdrUsage, "Forward.ResolvedHDR");
                graph.AddResolveTexturePass("Forward.ResolveColor4x", hdr, current);
            }
            RgTextureValue bloom = neutral;
            if (viewSettings.Bloom) {
                vector<std::pair<RgTextureValue, RenderExtent>> levels;
                RenderExtent levelSize = Half(size);
                auto last = current;
                auto lastSize = size;
                for (uint32_t level = 0; level < 5; ++level) {
                    last = ColorEffect(effects, programs, level == 0 ? 5 : 6, fmt::format("Forward.Bloom.Down{}", level), levelSize, lastSize, last);
                    levels.push_back({last, levelSize});
                    lastSize = levelSize;
                    levelSize = Half(levelSize);
                    if (lastSize.Width == 1 && lastSize.Height == 1) break;
                }
                for (size_t i = levels.size() - 1; i > 0; --i) {
                    last = ColorEffect(effects, programs, 7, fmt::format("Forward.Bloom.Up{}", i - 1), levels[i - 1].second, lastSize, last, levels[i - 1].first);
                    lastSize = levels[i - 1].second;
                }
                bloom = last;
            }
            uint32_t debug = 0;
            RgTextureViewDesc debugInputView;
            if (settings.DebugView == ForwardDebugView::TileOccupancy || settings.DebugView == ForwardDebugView::Shadows) {
                auto& program = *programs.Programs[13].Get();
                auto values = EffectValues(size, size);
                values.DebugMode = uint32_t(settings.DebugView);
                values.TileCapacity = settings.MaxLightsPerTile;
                current = Texture(graph, size, TextureFormat::RGBA16_FLOAT, kHdrUsage, "Forward.Debug");
                const ForwardPassResource resources[]{
                    {.Declaration = "DebugHeaders", .Buffer = headers, .StructureByteStride = 8},
                    {.Declaration = "DebugShadows", .Texture = shadowTexture},
                    {.Declaration = "OutputColor", .Write = true, .Texture = current}};
                Compute(effects, "Forward.Debug", program, values, resources, size);
                debug = 4;
            }
            switch (settings.DebugView) {
                case ForwardDebugView::LinearDepth:
                    current = linearDepth;
                    debug = 1;
                    break;
                case ForwardDebugView::Normals:
                    current = normals.IsValid() ? normals : neutral;
                    debug = 2;
                    break;
                case ForwardDebugView::Motion:
                    current = motion.IsValid() ? motion : neutral;
                    debug = 3;
                    break;
                case ForwardDebugView::AmbientOcclusion:
                    current = ao;
                    debug = 4;
                    break;
                case ForwardDebugView::Bloom:
                    current = bloom;
                    debug = 4;
                    break;
                case ForwardDebugView::CurrentHdr:
                    if (!msaa) current = currentHdrDebug;
                    break;
                case ForwardDebugView::HistoryHdr: current = historyHdrDebug; break;
                case ForwardDebugView::DepthPyramid:
                    current = pyramid;
                    debugInputView.Range = {0, 1, pyramidLevels - 1, 1};
                    debug = 1;
                    break;
                default: break;
            }
            auto& outputProgram = *programs.Programs[8].Get();
            auto outputValues = EffectValues(size, size);
            auto output = graph.Value(next.OutputInput);
            const auto outputFormat = graph.GetTextureDescriptor(output)->Format;
            const bool srgbAttachment = outputFormat == TextureFormat::RGBA8_UNORM_SRGB || outputFormat == TextureFormat::BGRA8_UNORM_SRGB || outputFormat == TextureFormat::RGBA16_FLOAT;
            outputValues.Options = Eigen::Vector4f{settings.Exposure, viewSettings.Bloom && settings.DebugView == ForwardDebugView::Final ? settings.BloomStrength : 0.f, srgbAttachment ? 0.f : 1.f, 0};
            outputValues.DebugMode = debug;
            const ForwardPassResource outputInputs[]{
                {.Declaration = "InputA", .Texture = current, .TextureView = debugInputView},
                {.Declaration = "InputB", .Texture = bloom}};
            next.Completion = Composite(effects, outputProgram, outputValues, outputInputs, output,
                                        firstOutputView ? render::LoadAction::Clear : render::LoadAction::Load, device.GetBackend(), "Forward.ToneMapAndComposite", EffectValueMode::ToneMap);
            next.Output = output;
        }
        if (!work.PassesSucceeded || !next.Completion.IsValid()) return false;
        next.ReadyCount = effects.ReadyCount;
        next.Graph = graph.FreezeTemplate();
        if (!next.Graph) return false;
        found = &InsertTemplate(cache.Views, std::move(next), 32);
    }
    auto& recipe = *found;
    recipe.Used = ++cache.UseSerial;
    const auto instance = graph.Instantiate(recipe.Graph);
    if (!instance.IsValid() || !graph.Connect(instance.Value(recipe.OutputInput), outputBinding->Texture) ||
        !graph.Connect(instance.Value(recipe.ShadowInput), shadows.Texture)) return false;
    for (size_t index = 0; index < surfaceValues.size(); ++index)
        if (!graph.Connect(instance.Value(recipe.SurfaceInputs[index]), surfaceValues[index])) return false;
    if (temporal) {
        if (!instance.Bind(recipe.CurrentColor, *colorHistory.Current) || !instance.Bind(recipe.CurrentDepth, *depthHistory.Current)) return false;
        if (historyValid && (!instance.Bind(recipe.PreviousColor, *colorHistory.Previous) || !instance.Bind(recipe.PreviousDepth, *depthHistory.Previous))) return false;
    }
    auto frame = [&] {
        RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.EffectFrameInputs");
        auto value = make_shared<EffectFrame>();
        value->Work = &work;
        value->Settings = viewSettings;
        value->View = view;
        value->Serial = context.FrameSerial();
        value->HistoryValid = historyValid;
        value->Ready.resize(recipe.ReadyCount);
        value->Surfaces = std::move(activeSurfaces);
        const auto scaleX = [&](uint32_t x) { return uint32_t(uint64_t{x} * family.OutputSize.Width / family.RenderSize.Width); };
        const auto scaleY = [&](uint32_t y) { return uint32_t(uint64_t{y} * family.OutputSize.Height / family.RenderSize.Height); };
        const auto convert = [&](Rect r) { return Rect{int32_t(scaleX(r.X)), int32_t(scaleY(r.Y)), scaleX(r.X + r.Width) - scaleX(r.X), scaleY(r.Y + r.Height) - scaleY(r.Y)}; };
        value->OutputViewport = convert(sourceView.ViewRect);
        value->OutputScissor = convert(sourceView.ScissorRect);
        return value;
    }();
    if (!sharedLit) {
        RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.Processor");
        sharedLit = make_shared<ForwardLitMeshPassProcessor>(draws, bindings, lightOverflowWarned, objects, temporal ? &context : nullptr);
    }
    work.Main.Work = instance.Value(recipe.Work);
    if (!instance.Bind(recipe.WorkSlot, [&] {
            RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.EffectFrameInputs");
            return make_shared<HdrDrawWork>(HdrDrawWork{&work, &scene, &context, std::move(sharedLit), settings.MaxLocalLights,
                                                        graph.GetRuntimeOptions().Validation, temporal, !writeDepthInOpaque, &lightOverflowWarned, frame});
        }()) ||
        !instance.Bind(recipe.Effects, frame) || !instance.Bind(recipe.LightUpload, shared_ptr<RgUploadData>{frame, &work.LightUpload})) return false;
    const ForwardGraphView stageViews[]{
        [&] {
            RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.PassViewInputs");
            return ForwardGraphView{.View = view, .List = &work.Main.DepthOnly};
        }(),
        [&] {
            RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.PassViewInputs");
            return ForwardGraphView{.View = view, .List = &work.Main.Opaque, .PassValues = ForwardGraphPassValues{shadows.Values.Get(), &work.LightCount, size, settings.MaxLightsPerTile, viewSettings.ForwardPlus, viewSettings.AmbientOcclusion, false}};
        }(),
        [&] {
            RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.PassViewInputs");
            return ForwardGraphView{.View = view, .List = &work.Main.Transparent, .PassValues = ForwardGraphPassValues{shadows.Values.Get(), &work.LightCount, size, settings.MaxLightsPerTile, viewSettings.ForwardPlus, viewSettings.AmbientOcclusion, !writeDepthInOpaque}};
        }()};
    for (size_t index = 0; index < recipe.Stages.size(); ++index)
        if (!instance.Bind(recipe.Stages[index], ForwardGraph::MakeFrame({.Backend = device.GetBackend(), .Views = std::span{&stageViews[index], 1}, .Execution = &work.Execution}))) return false;
    outputBinding->Texture = instance.Value(recipe.Output);
    work.ContentValid = true;
    work.TemporalHistory = temporal;
    work.HistoryValid = historyValid;
    work.Completion = context.RegisterViewCompletion(graph, sourceView.StateId, instance.Value(recipe.Completion), outputBinding->Texture);
    return work.Completion.IsValid();
}

bool BuildForwardOutputOverlay(RenderGraph& graph, ForwardEffectTemplates& templates, RenderPipelineContext& context, const ForwardEffectPrograms& programs,
                               const ForwardOutputOverlay& overlay, render::RenderBackend backend, std::span<RenderGraphOutputBinding> outputs) {
    Nullable<const ResolvedRenderViewFamily*> source, destination;
    for (const auto& family : context.ViewFamilies()) {
        if (family.OutputId == overlay.Source) source = &family;
        if (family.OutputId == overlay.Destination) destination = &family;
    }
    if (!source || !destination) {
        RADRAY_ERR_LOG("Forward overlay requires both outputs in this workload");
        return false;
    }
    if (!source->OutputAvailable || !destination->OutputAvailable) return true;
    if (source->Views.empty()) return false;
    auto sourceOutput = FindGraphOutput(outputs, overlay.Source), destinationOutput = FindGraphOutput(outputs, overlay.Destination);
    if (!sourceOutput || !destinationOutput) return false;
    const auto sourceDesc = graph.GetTextureDescriptor(sourceOutput->Texture), outputDesc = graph.GetTextureDescriptor(destinationOutput->Texture);
    if (!sourceDesc || !outputDesc) return false;
    const auto& r = overlay.Rectangle;
    const auto size = destination->OutputSize;
    const Rect rectangle = [&] {
        RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.ObserverFrameInputs");
        return Rect{int32_t(r.X * size.Width), int32_t(r.Y * size.Height), uint32_t(r.Width * size.Width), uint32_t(r.Height * size.Height)};
    }();
    if (rectangle.Width == 0 || rectangle.Height == 0) return true;
    auto& program = *programs.Programs[8].Get();
    auto& cache = templates.Get();
    Nullable<OverlayTemplate*> found{nullptr};
    for (auto& entry : cache.Overlays)
        if (entry.SourceDesc == TexturePoolKey{*sourceDesc} && entry.OutputDesc == TexturePoolKey{*outputDesc} &&
            entry.Program == &program && entry.ProgramGeneration == program.GetGeneration()) {
            found = &entry;
            break;
        }
    if (!found) {
        OverlayTemplate next;
        next.SourceDesc = {*sourceDesc};
        next.OutputDesc = {*outputDesc};
        next.Program = &program;
        next.ProgramGeneration = program.GetGeneration();
        auto declaration = graph.CreateTemplateBuilder("Forward.ObserverComposite");
        next.Frame = declaration.DeclareTemplateSlot<EffectFrame>();
        const auto work = declaration.AddTemplateWork("Forward.ObserverInputs", next.Frame, +[](EffectFrame& frame, uint64_t) { PrepareEffectInputs(frame); return true; });
        EffectGraphBuilder effects{declaration, next.Frame, work};
        next.Source = declaration.DeclareTexturePort(*sourceDesc, "Forward.ObserverSource");
        next.Destination = declaration.DeclareTexturePort(*outputDesc, "Forward.ObserverDestination");
        const auto input = declaration.Value(next.Source);
        auto output = declaration.Value(next.Destination);
        const auto isSrgb = [](TextureFormat format) { return format == TextureFormat::RGBA8_UNORM_SRGB || format == TextureFormat::BGRA8_UNORM_SRGB || format == TextureFormat::RGBA16_FLOAT; };
        auto values = EffectValues(destination->OutputSize, source->OutputSize);
        values.DebugMode = 5;
        values.Options = Eigen::Vector4f{1, 0, isSrgb(outputDesc->Format) ? 0.f : 1.f, isSrgb(sourceDesc->Format) ? 0.f : 1.f};
        const ForwardPassResource resources[]{{.Declaration = "InputA", .Texture = input}, {.Declaration = "InputB", .Texture = input}};
        if (!Composite(effects, program, values, resources, output, render::LoadAction::Load, backend, "Forward.ObserverComposite", EffectValueMode::Fixed).IsValid()) return false;
        next.Output = output;
        next.ReadyCount = effects.ReadyCount;
        next.Graph = declaration.FreezeTemplate();
        if (!next.Graph) return false;
        found = &InsertTemplate(cache.Overlays, std::move(next), 16);
    }
    auto& recipe = *found;
    recipe.Used = ++cache.UseSerial;
    const auto instance = graph.Instantiate(recipe.Graph);
    auto frame = [&] {
        RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.ObserverFrameInputs");
        auto value = make_shared<EffectFrame>();
        value->View = source->Views.front();
        value->OutputViewport = value->OutputScissor = rectangle;
        value->Ready.resize(recipe.ReadyCount);
        return value;
    }();
    if (!instance.IsValid() || !instance.Bind(recipe.Frame, frame) ||
        !graph.Connect(instance.Value(recipe.Source), sourceOutput->Texture) ||
        !graph.Connect(instance.Value(recipe.Destination), destinationOutput->Texture)) return false;
    destinationOutput->Texture = instance.Value(recipe.Output);
    return true;
}

}  // namespace radray::forward_detail
