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
Forward_EffectsData EffectValues(const ResolvedRenderView& view, RenderExtent output, RenderExtent input) {
    Forward_EffectsData values{};
    values.Extent = Eigen::Vector4f{float(output.Width), float(output.Height), float(input.Width), float(input.Height)};
    values.InverseProjection = view.Projection.inverse().eval();
    values.InverseViewProjection = view.ViewProjection.inverse().eval();
    values.PreviousViewProjection = view.PreviousViewValid ? view.PreviousViewProjection : view.ViewProjection;
    values.WorldToView = view.View;
    values.Projection = UnjitteredProjection(view);
    values.Eye = Eigen::Vector4f{view.WorldPosition.x(), view.WorldPosition.y(), view.WorldPosition.z(), 1};
    return values;
}
// Graph parameter sets still bind by declaration name; only the numeric payload is a POD now.
vector<RgParameterBinding> EffectBindings(ShaderProgram& program, const Forward_EffectsData& values, std::span<const RgParameterBinding> resources) {
    vector<RgParameterBinding> bindings;
    for (const auto& buffer : program.GetParameterLayout().Buffers())
        bindings.push_back({buffer.Name, 0, RgCBufferParameterBinding{AsCBufferBytes(values)}});
    for (const auto& resource : resources)
        if (program.GetArtifact().FindBindingInfo(resource.Declaration)) bindings.push_back(resource);
    if (program.GetArtifact().FindBindingInfo("ClampSampler")) bindings.push_back({"ClampSampler", 0, RgSamplerParameterBinding{ClampSampler()}});
    return bindings;
}
void Compute(RenderGraph& graph, std::string_view name, ShaderProgram& program, const Forward_EffectsData& values,
             std::span<const RgParameterBinding> bindings, RenderExtent size) {
    struct Data {
        RgComputeProgramHandle Program;
        RgParameterSetHandle Set;
        RenderExtent Size;
    };
    graph.AddComputePass<Data>(name, [&](Data& data, RenderGraphComputeBuilder& builder) {
        data.Program = builder.UseComputeProgram(program); data.Size = size;
        data.Set = builder.CreateParameterSet(program, 0, EffectBindings(program, values, bindings)); }, +[](const Data& data, RenderGraphComputeContext& ctx) {
        ctx.BindComputeProgram(data.Program); ctx.BindParameterSet(data.Set);
        ctx.Encoder().Dispatch((data.Size.Width + 7) / 8, (data.Size.Height + 7) / 8, 1); });
}
RgTextureValue ScalarEffect(RenderGraph& graph, const ForwardEffectPrograms& programs, uint32_t effect, std::string_view name,
                            const ResolvedRenderView& view, RenderExtent size, RenderExtent inputSize,
                            RgTextureValue a, RgTextureValue b = {}, Eigen::Vector4f options = Eigen::Vector4f::Zero()) {
    auto& program = *programs.Programs[effect].Get();
    const auto output = Texture(graph, size, TextureFormat::R32_FLOAT, kScalarUsage, name);
    auto values = EffectValues(view, size, inputSize);
    values.Options = options;
    const RgParameterBinding bindings[]{
        {"InputA", 0, RgTextureParameterBinding{a}}, {"InputB", 0, RgTextureParameterBinding{b}}, {"OutputScalar", 0, RgTextureParameterBinding{output, {}, RgParameterAccess::Write}}};
    Compute(graph, name, program, values, bindings, size);
    return output;
}
RgTextureValue ColorEffect(RenderGraph& graph, const ForwardEffectPrograms& programs, uint32_t effect, std::string_view name,
                           const ResolvedRenderView& view, RenderExtent size, RenderExtent inputSize,
                           RgTextureValue a, RgTextureValue b = {}) {
    auto& program = *programs.Programs[effect].Get();
    const auto output = Texture(graph, size, TextureFormat::RGBA16_FLOAT, kHdrUsage, name);
    auto values = EffectValues(view, size, inputSize);
    const RgParameterBinding bindings[]{
        {"InputA", 0, RgTextureParameterBinding{a}}, {"InputB", 0, RgTextureParameterBinding{b}}, {"OutputColor", 0, RgTextureParameterBinding{output, {}, RgParameterAccess::Write}}};
    Compute(graph, name, program, values, bindings, size);
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

RgPassHandle Composite(RenderGraph& graph, ShaderProgram& program, const Forward_EffectsData& values,
                       std::span<const RgParameterBinding> inputs, RgTextureValue& output, Rect viewport, Rect scissor,
                       render::LoadAction load, render::RenderBackend backend, bool& success, std::string_view name) {
    struct Data {
        RgGraphicsProgramHandle Program;
        RgParameterSetHandle Set;
        render::RenderBackend Backend;
        Rect Viewport, Scissor;
        bool* Success;
    };
    output = graph.NextVersion(output);
    return graph.AddRasterPass<Data>(name, [&](Data& data, RenderGraphRasterBuilder& builder) {
        data = {builder.UseGraphicsProgram(program, EffectRasterState()), builder.CreateParameterSet(program, 0, EffectBindings(program, values, inputs)), backend, viewport, scissor, &success};
        builder.SetColorAttachment(0, output, {.Load = load}); }, +[](const Data& data, RenderGraphRasterContext& ctx) {
        ctx.BindGraphicsProgram(data.Program); ctx.BindParameterSet(data.Set);
        ctx.Encoder().SetViewport(MakeViewport(data.Backend, float(data.Viewport.X), float(data.Viewport.Y), float(data.Viewport.Width), float(data.Viewport.Height)));
        ctx.Encoder().SetScissor(data.Scissor); ctx.Encoder().Draw(3, 1, 0, 0); });
}

void DrawSky(RenderGraph& graph, const ForwardEffectPrograms& programs, const ResolvedRenderView& view, RenderExtent size,
             RgTextureValue& hdr, RgTextureValue depth, render::RenderBackend backend, bool& success) {
    auto& program = *programs.Programs[10].Get();
    auto values = EffectValues(view, size, size);
    struct Data {
        RgGraphicsProgramHandle Program;
        RgParameterSetHandle Set;
        ResolvedRenderView View;
        render::RenderBackend Backend;
        bool* Success;
    };
    hdr = graph.NextVersion(hdr);
    graph.AddRasterPass<Data>("Forward.Sky", [&](Data& data, RenderGraphRasterBuilder& builder) {
        data = {builder.UseGraphicsProgram(program, EffectRasterState(true)), builder.CreateParameterSet(program, 0, EffectBindings(program, values, {})), view, backend, &success};
        builder.SetColorAttachment(0, hdr, {.Load = render::LoadAction::Load});
        builder.SetDepthAttachment(depth, {.Load = render::LoadAction::Load, .ReadOnly = true}); }, +[](const Data& data, RenderGraphRasterContext& context) {
        context.BindGraphicsProgram(data.Program); context.BindParameterSet(data.Set);
        context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, float(data.View.ViewRect.Width), float(data.View.ViewRect.Height)));
        context.Encoder().SetScissor(data.View.ScissorRect); context.Encoder().Draw(3, 1, 0, 0); });
}
void Fireflies(RenderGraph& graph, const ForwardEffectPrograms& programs, const ResolvedRenderView& view, RenderExtent size,
               RgTextureValue& hdr, RgTextureValue depth, render::RenderBackend backend, uint64_t serial, bool& success) {
    constexpr uint32_t count = 128;
    const auto particles = graph.CreateBuffer({count * 16, render::MemoryType::Device, render::BufferUse::Resource | render::BufferUse::UnorderedAccess, {}}, "Forward.Fireflies.Instances");
    const auto arguments = graph.CreateBuffer({16, render::MemoryType::Device, render::BufferUse::Indirect | render::BufferUse::UnorderedAccess, {}}, "Forward.Fireflies.Arguments");
    auto& update = *programs.Programs[11].Get();
    auto values = EffectValues(view, {count, 1}, size);
    values.Options = Eigen::Vector4f{float(serial % 360000) / 60, float(count), 0, 0};
    const RgParameterBinding writes[]{
        {"Particles", 0, RgBufferParameterBinding{particles, render::BufferRange::AllRange(), 16, TextureFormat::UNKNOWN, RgParameterAccess::Write}},
        {"Arguments", 0, RgBufferParameterBinding{arguments, render::BufferRange::AllRange(), 4, TextureFormat::UNKNOWN, RgParameterAccess::Write}}};
    Compute(graph, "Forward.Fireflies.Update", update, values, writes, {count, 1});
    auto& draw = *programs.Programs[12].Get();
    auto constants = EffectValues(view, size, size);
    const RgParameterBinding reads[]{{"Particles", 0, RgBufferParameterBinding{particles, render::BufferRange::AllRange(), 16}}};
    struct Data {
        RgGraphicsProgramHandle Program;
        RgParameterSetHandle Set;
        RgIndirectArgumentsHandle Arguments;
        ResolvedRenderView View;
        render::RenderBackend Backend;
        bool* Success;
    };
    hdr = graph.NextVersion(hdr);
    graph.AddRasterPass<Data>("Forward.Fireflies.Draw", [&](Data& data, RenderGraphRasterBuilder& builder) {
        data = {builder.UseGraphicsProgram(draw, EffectRasterState(true, true)), builder.CreateParameterSet(draw, 0, EffectBindings(draw, constants, reads)), builder.ReadIndirectArguments(arguments, RgIndirectCommand::Draw, 0, 1), view, backend, &success};
        builder.SetColorAttachment(0, hdr, {.Load = render::LoadAction::Load});
        builder.SetDepthAttachment(depth, {.Load = render::LoadAction::Load, .ReadOnly = true}); }, +[](const Data& data, RenderGraphRasterContext& context) {
        context.BindGraphicsProgram(data.Program); context.BindParameterSet(data.Set);
        context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, float(data.View.ViewRect.Width), float(data.View.ViewRect.Height)));
        context.Encoder().SetScissor(data.View.ScissorRect); context.Encoder().DrawIndirect(data.Arguments); });
}

struct LocalLightGpu {
    float PositionRadius[4], ColorType[4], DirectionCosOuter[4], Cone[4];
};
static_assert(sizeof(LocalLightGpu) == 64 && std::is_trivially_copyable_v<LocalLightGpu>);
uint32_t UploadLights(RenderGraph& graph, RgBufferValue& lights, const CullingResults& culling,
                      uint32_t maxCount, bool& warned) {
    RADRAY_PROFILE_SCOPE_N("UploadLights");
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
    vector<LocalLightGpu> gpu(std::max<size_t>(1, selected.size()));
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
    lights = graph.UploadBuffer("Forward.LocalLights", std::as_bytes(std::span{gpu}), render::BufferUse::Resource);
    if (!lights.IsValid()) return UINT32_MAX;
    return static_cast<uint32_t>(selected.size());
}

struct ShadowData {
    array<Eigen::Matrix4f, 4> Matrices;
    array<Eigen::Vector4f, 4> Spheres, Bias;
    Eigen::Vector4f Params{Eigen::Vector4f::Zero()};
};
ShadowData BuildShadows(RenderGraph& graph, const ForwardPipelineSettings& settings, const ResolvedRenderView& main,
                        const RenderSceneSnapshot& scene, const PackedCBufferTable& objects, FrameDrawResources& draws, ForwardBindingCache& bindings,
                        ForwardHdrView& work, RgTextureValue& texture, render::RenderBackend backend, bool& warned) {
    RADRAY_PROFILE_SCOPE_N("BuildShadows");
    ShadowData shadow;
    const RenderLightData* sun = nullptr;
    for (const auto& light : scene.Lights) {
        if (light.Type == LightType::Directional) {
            sun = &light;
            break;
        }
    }
    const bool enabled = settings.Shadows && sun && sun->CastShadow;
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
    // One processor for all cascades: material and view-independent object groups (ShadowCaster has no motion data)
    // are prepared once per primitive instead of once per cascade; only the view group is rebuilt per cascade.
    ForwardLitMeshPassProcessor processor{draws, bindings, warned, objects};
    for (uint32_t cascade = 0; cascade < 4; ++cascade) {
        auto& draw = work.Cascades[cascade];
        draw.View = main;
        const float ratio = float(cascade + 1) / 4;
        const float split = .65f * nearZ * std::pow(farZ / nearZ, ratio) + .35f * (nearZ + (farZ - nearZ) * ratio);
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
        shadow.Matrices[cascade] = draw.View.ViewProjection;
        shadow.Spheres[cascade] = {center.x(), center.y(), center.z(), radius * radius};
        shadow.Bias[cascade] = {texel, texel * 2, 0, 0};
        if (enabled) {
            RADRAY_PROFILE_SCOPE_N("ShadowCascadeCullAndList");
            Cull({&scene, &draw.View}, draw.Culling);
            processor.ResetView();
            BuildRendererList({"ShadowCaster", "ShadowCaster", &draw.Culling, &draw.View, RenderQueueRange::Opaque(), 0xffffffffu, RendererListSorting::FrontToBack, true}, processor, draw.DepthOnly);
            work.ContentValid = work.ContentValid && draw.DepthOnly.Stats.ContentSucceeded();
        }
        const ForwardGraphView shadowView{draw.View, &draw.DepthOnly};
        RgDepthAttachmentDesc attachment;
        attachment.View.Range = {cascade, 1, 0, 1};
        const auto stage = ForwardGraph::BuildGraph(graph, ForwardGraphStage::Depth,
                                                    {.Name = fmt::format("Forward.Shadow.{}", cascade), .Backend = backend, .Views = std::span{&shadowView, 1}, .Depth = texture, .DepthAttachment = attachment, .Execution = &work.Execution, .PreserveEmptyPass = true});
        work.PassesSucceeded &= stage.Success;
        texture = stage.Depth;
        previous = split;
    }
    shadow.Params = {enabled ? 1.f : 0.f, float(settings.ShadowResolution), 4, 1};
    return shadow;
}

struct LitBindings {
    // Reserved to the program count before any binding is built: the graph holds spans into these
    // rows, so the vector must not reallocate while MakeLitBindings runs.
    vector<Forward_PassData> Constants;
    vector<vector<RgParameterBinding>> Resources;
    vector<RendererListProgramParameters> Programs;
};
LitBindings MakeLitBindings(const RendererList& list, const ShadowData& shadow, RenderExtent extent, const ForwardPipelineSettings& settings,
                            RgTextureValue shadows, RgBufferValue lights, uint32_t count, RgBufferValue headers, RgBufferValue indices,
                            RgTextureValue ao, RgTextureValue opaqueColor, bool transparent, bool& valid) {
    RADRAY_PROFILE_SCOPE_N("MakeLitBindings");
    LitBindings result;
    vector<ShaderProgram*> programs;
    for (const auto& draw : list.Commands)
        if (std::find(programs.begin(), programs.end(), draw.Program.Get()) == programs.end()) programs.push_back(draw.Program.Get());
    result.Constants.reserve(programs.size());
    result.Resources.reserve(programs.size());
    for (auto* program : programs) {
        const auto binding = ResolveProgramBindings(*program);
        if (!binding || !binding->PassGroup) {
            valid = false;
            continue;
        }
        auto& values = result.Constants.emplace_back();
        values.ShadowMatrix0 = shadow.Matrices[0];
        values.ShadowMatrix1 = shadow.Matrices[1];
        values.ShadowMatrix2 = shadow.Matrices[2];
        values.ShadowMatrix3 = shadow.Matrices[3];
        for (uint32_t i = 0; i < 4; ++i) {
            values.ShadowSphere[i] = shadow.Spheres[i];
            values.ShadowBias[i] = shadow.Bias[i];
        }
        values.ShadowParams = shadow.Params;
        values.Extent = Eigen::Vector4f{float(extent.Width), float(extent.Height), float((extent.Width + 15) / 16), float(settings.MaxLightsPerTile)};
        values.LocalLightCount = count;
        values.UseTiles = settings.ForwardPlus ? 1u : 0u;
        values.UseAo = settings.AmbientOcclusion ? 1u : 0u;
        values.Transparent = transparent ? 1u : 0u;
        auto& resources = result.Resources.emplace_back();
        resources.push_back({"ForwardPass", 0, RgCBufferParameterBinding{AsCBufferBytes(values)}});
        auto compare = ClampSampler();
        compare.Compare = render::CompareFunction::LessEqual;
        resources.push_back({"ShadowMap", 0, RgTextureParameterBinding{shadows}});
        resources.push_back({"ShadowSampler", 0, RgSamplerParameterBinding{compare}});
        resources.push_back({"LocalLights", 0, RgBufferParameterBinding{lights, render::BufferRange::AllRange(), sizeof(LocalLightGpu)}});
        resources.push_back({"TileHeaders", 0, RgBufferParameterBinding{headers, render::BufferRange::AllRange(), 8}});
        resources.push_back({"TileIndices", 0, RgBufferParameterBinding{indices, render::BufferRange::AllRange(), 4}});
        resources.push_back({"AmbientOcclusion", 0, RgTextureParameterBinding{ao}});
        resources.push_back({"OpaqueColor", 0, RgTextureParameterBinding{opaqueColor}});
        resources.push_back({"ScreenSampler", 0, RgSamplerParameterBinding{ClampSampler()}});
        result.Programs.push_back({program, *binding->PassGroup, resources});
    }
    return result;
}

}  // namespace

bool ForwardEffectPrograms::Initialize(RenderSystem& system) {
    static constexpr std::string_view sources[]{"linear_depth", "depth_pyramid", "ambient_occlusion", "ao_blur", "temporal_resolve", "bloom", "bloom", "bloom", "output", "tile_lights", "sky", "firefly_update", "firefly_draw", "debug", "output_surface"};
    for (uint32_t effect = 0; effect < Programs.size(); ++effect) {
        if (!Programs[effect]) Programs[effect] = system.GetOrCreateShaderProgram({.SourceName = fmt::format("shaderlib/pipelines/forward/{}.hlsl", sources[effect]), .Defines = {{"FORWARD_EFFECT", std::to_string(effect)}}});
        if (!Programs[effect]) return false;
    }
    return true;
}
bool ForwardViewSignature::Matches(const ForwardViewSignature& other) const noexcept {
    // Display-only controls do not change opaque history contents.
    auto a = Settings, b = other.Settings;
    a.DebugView = b.DebugView;
    a.Exposure = b.Exposure;
    a.Bloom = b.Bloom;
    a.BloomStrength = b.BloomStrength;
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
}

bool DeclareForwardSharedShadows(RenderGraph& graph, const ForwardPipelineSettings& settings, const ResolvedRenderView& primary,
                                  const RenderSceneSnapshot& scene, const PackedCBufferTable& objects, FrameDrawResources& draws, ForwardBindingCache& bindings,
                                  ForwardHdrView& work, render::RenderBackend backend, bool& warned, ForwardShadowAtlas& out) {
    RADRAY_PROFILE_SCOPE_N("DeclareSharedShadows");
    const auto previousScope = graph.SetResourceView(primary.StateId.Value);
    struct RestoreScope {
        RenderGraph& Graph;
        uint64_t Previous;
        ~RestoreScope() { Graph.SetResourceView(Previous); }
    } restore{graph, previousScope};
    out = {};
    work.ContentValid = true;
    out.Texture = Texture(graph, {settings.ShadowResolution, settings.ShadowResolution}, TextureFormat::D32_FLOAT,
                           TextureUse::DepthStencilWrite | TextureUse::Resource, "Forward.Shadows", 1, 4);
    auto shadow = BuildShadows(graph, settings, primary, scene, objects, draws, bindings, work, out.Texture, backend, warned);
    out.Matrices = shadow.Matrices;
    out.Spheres = shadow.Spheres;
    out.Bias = shadow.Bias;
    out.Params = shadow.Params;
    return out.Texture.IsValid() && work.PassesSucceeded;
}

namespace {
bool BuildOutputSurfaces(RenderGraph& graph, ShaderProgram& program,
                         const ResolvedRenderViewFamily& family, const ResolvedRenderView& view,
                         RgTextureValue& color, RgTextureValue depth, render::RenderBackend backend,
                         std::span<const ForwardOutputSurface> surfaces, std::span<RenderGraphOutputBinding> outputs) {
    for (const auto& surface : surfaces) {
        if (surface.Destination != family.OutputId || !(surface.LayerMask & view.LayerMask)) continue;
        const auto source = FindGraphOutput(outputs, surface.Source);
        const auto texture = source ? source->Texture : RgTextureValue{};
        const auto descriptor = graph.GetTextureDescriptor(texture);
        if (!descriptor || descriptor->SampleCount != 1 || descriptor->Dim != render::TextureDimension::Dim2D ||
            !descriptor->Usage.HasFlag(TextureUse::Resource)) {
            graph.AddDiagnostic("ForwardOutputSurface", "Screen source requires a sampleable single-sample 2D output");
            return false;
        }
        const auto format = descriptor->Format;
        const bool srgb = format == TextureFormat::RGBA8_UNORM_SRGB || format == TextureFormat::BGRA8_UNORM_SRGB;
        if (!srgb && format != TextureFormat::RGBA8_UNORM && format != TextureFormat::BGRA8_UNORM && format != TextureFormat::RGBA16_FLOAT) {
            graph.AddDiagnostic("ForwardOutputSurface", "Screen source must contain an SDR scene output");
            return false;
        }
        Forward_OutputSurfaceData values{};
        values.LocalToClip = (view.ViewProjection * surface.LocalToWorld).eval();
        values.Options = Eigen::Vector4f{surface.Brightness, srgb || format == TextureFormat::RGBA16_FLOAT ? 0.f : 1.f, 0, 0};
        const RgParameterBinding bindings[]{
            {"OutputSurface", 0, RgCBufferParameterBinding{AsCBufferBytes(values)}},
            {"SceneOutput", 0, RgTextureParameterBinding{texture}},
            {"OutputSampler", 0, RgSamplerParameterBinding{ClampSampler()}}};
        struct Data {
            RgGraphicsProgramHandle Program;
            RgParameterSetHandle Set;
            Rect Viewport, Scissor;
            render::RenderBackend Backend;
        };
        color = graph.NextVersion(color);
        graph.AddRasterPass<Data>("Forward.OutputSurface", [&](Data& data, RenderGraphRasterBuilder& builder) {
            data = {builder.UseGraphicsProgram(program, EffectRasterState(true)), builder.CreateParameterSet(program, 0, bindings), view.ViewRect, view.ScissorRect, backend};
            builder.SetColorAttachment(0, color, {.Load = render::LoadAction::Load});
            builder.SetDepthAttachment(depth, {.Load = render::LoadAction::Load, .ReadOnly = true}); }, +[](const Data& data, RenderGraphRasterContext& ctx) {
            auto& encoder = ctx.Encoder();
            ctx.BindGraphicsProgram(data.Program);
            ctx.BindParameterSet(data.Set);
            encoder.SetViewport(MakeViewport(data.Backend, float(data.Viewport.X), float(data.Viewport.Y), float(data.Viewport.Width), float(data.Viewport.Height)));
            encoder.SetScissor(data.Scissor);
            encoder.Draw(6, 1, 0, 0); });
    }
    return true;
}
}  // namespace

// The top-level ForwardPipeline appends this view's stages to the same frame graph.
bool BuildForwardHdrView(RenderGraph& graph, RenderPipelineContext& context, render::Device& device,
                         const ForwardEffectPrograms& programs, const ForwardPipelineSettings& settings,
                         const ResolvedRenderViewFamily& family, const ResolvedRenderView& sourceView,
                         const RenderSceneSnapshot& scene, const PackedCBufferTable& objects, FrameDrawResources& draws, ForwardBindingCache& bindings,
                         ForwardHdrView& work, bool firstOutputView, bool& lightOverflowWarned,
                         std::span<const ForwardOutputSurface> surfaces, std::span<RenderGraphOutputBinding> outputs,
                         const ForwardShadowAtlas& shadows, bool auxiliary, ForwardLitMeshPassProcessor* sharedLit) {
    RADRAY_PROFILE_SCOPE_N("BuildForwardHdrView");
    const auto previousScope = graph.SetResourceView(sourceView.StateId.Value);
    struct RestoreScope {
        RenderGraph& Graph;
        uint64_t Previous;
        ~RestoreScope() { Graph.SetResourceView(Previous); }
    } restore{graph, previousScope};
    const bool temporal = !auxiliary && settings.Antialiasing == ForwardAntialiasing::Temporal;
    const bool msaa = !auxiliary && settings.Antialiasing == ForwardAntialiasing::Msaa4;
    const bool writeDepthInOpaque = msaa || auxiliary;
    auto viewSettings = settings;
    if (auxiliary) viewSettings.AmbientOcclusion = viewSettings.Bloom = viewSettings.Fireflies = false;
    const uint32_t samples = msaa ? 4 : 1;
    const RenderExtent size{sourceView.ViewRect.Width, sourceView.ViewRect.Height};
    if (!size.Width || !size.Height || !family.OutputAvailable || family.SampleCount != 1) return false;
    for (const auto format : {TextureFormat::RGBA16_FLOAT, TextureFormat::R32_FLOAT}) {
        const auto supported = device.QueryTextureSupport({render::TextureDimension::Dim2D, format, format == TextureFormat::RGBA16_FLOAT ? kHdrUsage : kScalarUsage});
        if (!supported.Supported) {
            RADRAY_ERR_LOG("Forward HDR requires storage/sampled format {}", format);
            return false;
        }
    }
    work.Main.View = sourceView;
    auto& view = work.Main.View;
    view.ViewRect = {0, 0, size.Width, size.Height};
    view.ScissorRect = {sourceView.ScissorRect.X - sourceView.ViewRect.X, sourceView.ScissorRect.Y - sourceView.ViewRect.Y,
                        sourceView.ScissorRect.Width, sourceView.ScissorRect.Height};
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
            if (!context.PreparePrimitiveHistory(view, scene)) return false;
        }
    }
    auto cullProjection = UnjitteredProjection(view);
    cullProjection.row(0) *= float(size.Width) / float(size.Width + 2);
    cullProjection.row(1) *= float(size.Height) / float(size.Height + 2);
    const auto cullMatrix = (cullProjection * view.View).eval();
    {
        RADRAY_PROFILE_SCOPE_N("MainViewCull");
        if (!Cull({&scene, &view, 0xffffffffu, cullMatrix}, work.Main.Culling)) return false;
    }
    // One processor for prepass/opaque/transparent: same view, so view and object groups are shared across lists.
    std::optional<ForwardLitMeshPassProcessor> localProcessor;
    ForwardLitMeshPassProcessor* processor = sharedLit;
    if (processor == nullptr) {
        localProcessor.emplace(draws, bindings, lightOverflowWarned, objects, temporal ? &context : nullptr);
        processor = &*localProcessor;
    } else {
        processor->ResetView();
    }
    work.ContentValid = true;
    {
        RADRAY_PROFILE_SCOPE_N("MainViewRendererLists");
        if (!msaa && !auxiliary) {
            const RendererListDesc descs[] = {
                {"DepthNormalsMotion", "DepthNormalsMotion", &work.Main.Culling, &view, RenderQueueRange::Opaque(), 0xffffffffu, RendererListSorting::FrontToBack, true},
                {"Opaque", "ForwardLit", &work.Main.Culling, &view, RenderQueueRange::Opaque(), 0xffffffffu, RendererListSorting::StateThenFrontToBack, true},
                {"Transparent", "ForwardLit", &work.Main.Culling, &view, RenderQueueRange::Transparent(), 0xffffffffu, RendererListSorting::BackToFront, true},
            };
            RendererList* outs[] = {&work.Main.DepthOnly, &work.Main.Opaque, &work.Main.Transparent};
            BuildRendererLists(descs, *processor, outs);
            work.ContentValid &= work.Main.DepthOnly.Stats.ContentSucceeded();
        } else {
            const RendererListDesc descs[] = {
                {"Opaque", "ForwardLit", &work.Main.Culling, &view, RenderQueueRange::Opaque(), 0xffffffffu, RendererListSorting::StateThenFrontToBack, true},
                {"Transparent", "ForwardLit", &work.Main.Culling, &view, RenderQueueRange::Transparent(), 0xffffffffu, RendererListSorting::BackToFront, true},
            };
            RendererList* outs[] = {&work.Main.Opaque, &work.Main.Transparent};
            BuildRendererLists(descs, *processor, outs);
        }
        work.ContentValid &= work.Main.Opaque.Stats.ContentSucceeded() && work.Main.Transparent.Stats.ContentSucceeded();
    }
    if (!writeDepthInOpaque)
        for (auto& command : work.Main.Opaque.Commands) command.PipelineState.DepthStencil.DepthWriteEnable = false;
    {
        RADRAY_PROFILE_SCOPE_N("DeclareHdrGraph");
        auto depth = Texture(graph, size, TextureFormat::D32_FLOAT, TextureUse::DepthStencilWrite | TextureUse::DepthStencilRead | (msaa || auxiliary ? TextureUses{} : TextureUses{TextureUse::Resource}), "Forward.Depth", samples);
        auto hdr = Texture(graph, size, TextureFormat::RGBA16_FLOAT, msaa ? TextureUse::RenderTarget | TextureUse::CopySource : kHdrUsage | TextureUse::RenderTarget, "Forward.HDR", samples);
        auto normals = msaa || auxiliary ? RgTextureValue{} : Texture(graph, size, TextureFormat::RGBA16_FLOAT, kHdrUsage | TextureUse::RenderTarget, "Forward.Normals");
        const auto motion = msaa || auxiliary ? RgTextureValue{} : Texture(graph, size, TextureFormat::RGBA16_FLOAT, kHdrUsage | TextureUse::RenderTarget, "Forward.Motion");
        if (!shadows.Texture.IsValid()) return false;
        const auto shadowTexture = shadows.Texture;
        ShadowData shadow;
        shadow.Matrices = shadows.Matrices;
        shadow.Spheres = shadows.Spheres;
        shadow.Bias = shadows.Bias;
        shadow.Params = shadows.Params;
        if (!msaa && !auxiliary) {
            RADRAY_PROFILE_SCOPE_N("HdrPrepass");
            const ForwardGraphView draw{view, &work.Main.DepthOnly};
            const RgTextureValue prepassAux[]{motion};
            const auto prepassStage = ForwardGraph::BuildGraph(graph, ForwardGraphStage::Opaque,
                                                               {.Name = "Forward.DepthNormalsMotion", .Backend = device.GetBackend(), .Views = std::span{&draw, 1}, .Color = normals, .Depth = depth, .Execution = &work.Execution, .PreserveEmptyPass = true, .AuxiliaryColors = prepassAux});
            work.PassesSucceeded &= prepassStage.Success;
            normals = prepassStage.Color;
            depth = prepassStage.Depth;
        }
        // Neutral resources remain real graph producers so disabled effects have explicit valid inputs.
        const auto neutral = Texture(graph, {1, 1}, TextureFormat::RGBA16_FLOAT, kHdrUsage | TextureUse::RenderTarget, "Forward.Neutral");
        struct Empty {};
        graph.AddRasterPass<Empty>("Forward.Neutral", [=](Empty&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, neutral, {.Clear = {1, 1, 1, 1}}); }, +[](const Empty&, RenderGraphRasterContext&) {});
        RgTextureValue linearDepth = neutral, ao = neutral, pyramid = neutral;
        uint32_t pyramidLevels = 1;
        if (!msaa && !auxiliary) {
            RADRAY_PROFILE_SCOPE_N("HdrDepthPyramidAo");
            linearDepth = ScalarEffect(graph, programs, 0, "Forward.LinearDepth", view, size, size, depth);
            const auto halfSize = Half(size);
            for (auto dimension = std::max(halfSize.Width, halfSize.Height); dimension > 1; dimension >>= 1) ++pyramidLevels;
            pyramid = graph.CreateTexture({render::TextureDimension::Dim2D, halfSize.Width, halfSize.Height, 1, pyramidLevels, 1, TextureFormat::R32_FLOAT, render::MemoryType::Device, kScalarUsage, {}}, "Forward.DepthPyramid");
            auto previousSize = size;
            for (uint32_t mip = 0; mip < pyramidLevels; ++mip) {
                const RenderExtent mipSize{std::max(1u, halfSize.Width >> mip), std::max(1u, halfSize.Height >> mip)};
                auto& program = *programs.Programs[1].Get();
                const auto values = EffectValues(view, mipSize, previousSize);
                const RgParameterBinding resources[]{
                    {"InputA", 0, RgTextureParameterBinding{mip == 0 ? linearDepth : pyramid, {.Range = {0, 1, mip == 0 ? 0 : mip - 1, 1}}}},
                    {"OutputScalar", 0, RgTextureParameterBinding{pyramid, {.Range = {0, 1, mip, 1}}, RgParameterAccess::Write}}};
                Compute(graph, fmt::format("Forward.DepthPyramid.{}", mip), program, values, resources, mipSize);
                previousSize = mipSize;
            }
            if (viewSettings.AmbientOcclusion) {
                auto& program = *programs.Programs[2].Get();
                auto values = EffectValues(view, halfSize, halfSize);
                values.Options = Eigen::Vector4f{settings.AoRadius, 0, 0, 0};
                ao = Texture(graph, halfSize, TextureFormat::R32_FLOAT, kScalarUsage, "Forward.AO");
                const RgParameterBinding resources[]{
                    {"InputA", 0, RgTextureParameterBinding{pyramid, {.Range = {0, 1, 0, 1}}}},
                    {"InputB", 0, RgTextureParameterBinding{normals}},
                    {"InputC", 0, RgTextureParameterBinding{pyramid, {.Range = {0, 1, std::min(1u, pyramidLevels - 1), 1}}}},
                    {"OutputScalar", 0, RgTextureParameterBinding{ao, {}, RgParameterAccess::Write}}};
                Compute(graph, "Forward.AO", program, values, resources, halfSize);
                ao = ScalarEffect(graph, programs, 3, "Forward.AO.Horizontal", view, halfSize, halfSize, ao, linearDepth, {1, 0, 0, 0});
                ao = ScalarEffect(graph, programs, 3, "Forward.AO.Vertical", view, halfSize, halfSize, ao, linearDepth, {0, 1, 0, 0});
            }
        }
        RgBufferValue lights;
        uint32_t count = 0;
        RgBufferValue headers, indices;
        {
            RADRAY_PROFILE_SCOPE_N("HdrLights");
            count = UploadLights(graph, lights, work.Main.Culling, settings.MaxLocalLights, lightOverflowWarned);
            if (count == UINT32_MAX) return false;
            const RenderExtent tiles{(size.Width + 15) / 16, (size.Height + 15) / 16};
            const auto tileCount = uint64_t{tiles.Width} * tiles.Height;
            headers = graph.CreateBuffer({tileCount * 8, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Resource, {}}, "Forward.TileHeaders");
            indices = graph.CreateBuffer({tileCount * settings.MaxLightsPerTile * 4, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Resource, {}}, "Forward.TileIndices");
            auto& tileProgram = *programs.Programs[9].Get();
            auto tileValues = EffectValues(view, tiles, size);
            tileValues.LocalLightCount = count;
            tileValues.TileCapacity = settings.MaxLightsPerTile;
            const RgParameterBinding tileBindings[]{
                {"Lights", 0, RgBufferParameterBinding{lights, render::BufferRange::AllRange(), sizeof(LocalLightGpu)}},
                {"Headers", 0, RgBufferParameterBinding{headers, render::BufferRange::AllRange(), 8, TextureFormat::UNKNOWN, RgParameterAccess::Write}},
                {"Indices", 0, RgBufferParameterBinding{indices, render::BufferRange::AllRange(), 4, TextureFormat::UNKNOWN, RgParameterAccess::Write}}};
            Compute(graph, "Forward.TileLightCull", tileProgram, tileValues, tileBindings, tiles);
        }
        {
            RADRAY_PROFILE_SCOPE_N("HdrOpaqueSky");
            auto opaqueBindings = MakeLitBindings(work.Main.Opaque, shadow, size, viewSettings, shadowTexture, lights, count, headers, indices, ao, neutral, false, work.ContentValid);
            const ForwardGraphView opaqueView{view, &work.Main.Opaque, opaqueBindings.Programs};
            const auto opaqueStage = ForwardGraph::BuildGraph(graph, ForwardGraphStage::Opaque,
                                                              {.Name = "Forward.Opaque", .Backend = device.GetBackend(), .Views = std::span{&opaqueView, 1}, .Color = hdr, .Depth = depth, .ColorAttachment = {.Clear = {.035f, .06f, .1f, 1}}, .DepthAttachment = {.Load = writeDepthInOpaque ? render::LoadAction::Clear : render::LoadAction::Load, .ReadOnly = !writeDepthInOpaque}, .Execution = &work.Execution});
            work.PassesSucceeded &= opaqueStage.Success;
            hdr = opaqueStage.Color;
            depth = opaqueStage.Depth;
            DrawSky(graph, programs, view, size, hdr, depth, device.GetBackend(), work.PassesSucceeded);
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
            auto values = EffectValues(view, size, size);
            const bool valid = colorHistory.PreviousValid && depthHistory.PreviousValid && view.PreviousViewValid;
            values.HistoryValid = valid ? 1u : 0u;
            const auto previous = valid ? graph.ImportTexture(*colorHistory.Previous.Get(), "Forward.PreviousColor", RenderGraphExternalAccess::ReadOnly) : hdr;
            const auto previousDepth = valid ? graph.ImportTexture(*depthHistory.Previous.Get(), "Forward.PreviousDepth", RenderGraphExternalAccess::ReadOnly) : depth;
            const auto colorOut = graph.NextVersion(graph.ImportTexture(*colorHistory.Current.Get(), "Forward.CurrentColor", RenderGraphExternalAccess::ReadWrite));
            const auto depthOut = graph.NextVersion(graph.ImportTexture(*depthHistory.Current.Get(), "Forward.CurrentDepth", RenderGraphExternalAccess::ReadWrite));
            const RgParameterBinding taa[]{
                {"InputA", 0, RgTextureParameterBinding{hdr}}, {"InputB", 0, RgTextureParameterBinding{previous}}, {"InputC", 0, RgTextureParameterBinding{motion}}, {"InputD", 0, RgTextureParameterBinding{depth}}, {"InputE", 0, RgTextureParameterBinding{previousDepth}}, {"OutputColor", 0, RgTextureParameterBinding{colorOut, {}, RgParameterAccess::Write}}, {"OutputScalar", 0, RgTextureParameterBinding{depthOut, {}, RgParameterAccess::Write}}};
            Compute(graph, "Forward.TAA", program, values, taa, size);
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
        work.PassesSucceeded &= BuildOutputSurfaces(graph, *programs.Programs[14].Get(), family, view, hdr, depth, device.GetBackend(), surfaces, outputs);
        if (!writeDepthInOpaque) {
            opaqueCopy = Texture(graph, size, TextureFormat::RGBA16_FLOAT, kHdrUsage, "Forward.OpaqueColor");
            graph.AddCopyTexturePass("Forward.CopyOpaque", hdr, opaqueCopy);
        }
        {
            RADRAY_PROFILE_SCOPE_N("HdrTransparent");
            auto transparentBindings = MakeLitBindings(work.Main.Transparent, shadow, size, viewSettings, shadowTexture, lights, count, headers, indices, ao, opaqueCopy, !writeDepthInOpaque, work.ContentValid);
            const ForwardGraphView transparentView{view, &work.Main.Transparent, transparentBindings.Programs};
            const auto transparentStage = ForwardGraph::BuildGraph(graph, ForwardGraphStage::Transparent,
                                                               {.Name = "Forward.Transparent", .Backend = device.GetBackend(), .Views = std::span{&transparentView, 1}, .Color = hdr, .Depth = depth, .ColorAttachment = {.Load = render::LoadAction::Load}, .DepthAttachment = {.Load = render::LoadAction::Load, .ReadOnly = true}, .Execution = &work.Execution});
            work.PassesSucceeded &= transparentStage.Success;
            hdr = transparentStage.Color;
            depth = transparentStage.Depth;
        }
        {
            RADRAY_PROFILE_SCOPE_N("HdrBloomComposite");
            if (viewSettings.Fireflies) Fireflies(graph, programs, view, size, hdr, depth, device.GetBackend(), context.FrameSerial(), work.PassesSucceeded);
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
                    last = ColorEffect(graph, programs, level == 0 ? 5 : 6, fmt::format("Forward.Bloom.Down{}", level), view, levelSize, lastSize, last);
                    levels.push_back({last, levelSize});
                    lastSize = levelSize;
                    levelSize = Half(levelSize);
                    if (lastSize.Width == 1 && lastSize.Height == 1) break;
                }
                for (size_t i = levels.size() - 1; i > 0; --i) {
                    last = ColorEffect(graph, programs, 7, fmt::format("Forward.Bloom.Up{}", i - 1), view, levels[i - 1].second, lastSize, last, levels[i - 1].first);
                    lastSize = levels[i - 1].second;
                }
                bloom = last;
            }
            uint32_t debug = 0;
            RgTextureViewDesc debugInputView;
            if (settings.DebugView == ForwardDebugView::TileOccupancy || settings.DebugView == ForwardDebugView::Shadows) {
                auto& program = *programs.Programs[13].Get();
                auto values = EffectValues(view, size, size);
                values.DebugMode = uint32_t(settings.DebugView);
                values.TileCapacity = settings.MaxLightsPerTile;
                current = Texture(graph, size, TextureFormat::RGBA16_FLOAT, kHdrUsage, "Forward.Debug");
                const RgParameterBinding resources[]{
                    {"DebugHeaders", 0, RgBufferParameterBinding{headers, render::BufferRange::AllRange(), 8}}, {"DebugShadows", 0, RgTextureParameterBinding{shadowTexture}}, {"OutputColor", 0, RgTextureParameterBinding{current, {}, RgParameterAccess::Write}}};
                Compute(graph, "Forward.Debug", program, values, resources, size);
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
            auto outputValues = EffectValues(view, size, size);
            auto outputBinding = FindGraphOutput(outputs, family.OutputId);
            if (!outputBinding) return false;
            auto& output = outputBinding->Texture;
            const auto outputFormat = graph.GetTextureDescriptor(output)->Format;
            const bool srgbAttachment = outputFormat == TextureFormat::RGBA8_UNORM_SRGB || outputFormat == TextureFormat::BGRA8_UNORM_SRGB || outputFormat == TextureFormat::RGBA16_FLOAT;
            outputValues.Options = Eigen::Vector4f{settings.Exposure, viewSettings.Bloom && settings.DebugView == ForwardDebugView::Final ? settings.BloomStrength : 0.f, srgbAttachment ? 0.f : 1.f, 0};
            outputValues.DebugMode = debug;
            const RgParameterBinding outputInputs[]{{"InputA", 0, RgTextureParameterBinding{current, debugInputView}}, {"InputB", 0, RgTextureParameterBinding{bloom}}};
            const auto scaleX = [&](uint32_t x) { return uint32_t(uint64_t{x} * family.OutputSize.Width / family.RenderSize.Width); };
            const auto scaleY = [&](uint32_t y) { return uint32_t(uint64_t{y} * family.OutputSize.Height / family.RenderSize.Height); };
            const auto convert = [&](Rect r) { return Rect{int32_t(scaleX(r.X)), int32_t(scaleY(r.Y)), scaleX(r.X + r.Width) - scaleX(r.X), scaleY(r.Y + r.Height) - scaleY(r.Y)}; };
            const auto pass = Composite(graph, outputProgram, outputValues, outputInputs, output, convert(sourceView.ViewRect), convert(sourceView.ScissorRect),
                                        firstOutputView ? render::LoadAction::Clear : render::LoadAction::Load, device.GetBackend(), work.PassesSucceeded, "Forward.ToneMapAndComposite");
            work.Completion = context.RegisterViewCompletion(graph, sourceView.StateId, pass, output);
            return work.Completion.IsValid();
        }
    }
}

bool BuildForwardOutputOverlay(RenderGraph& graph, RenderPipelineContext& context, const ForwardEffectPrograms& programs,
                               const ForwardOutputOverlay& overlay, render::RenderBackend backend, bool& success, std::span<RenderGraphOutputBinding> outputs) {
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
    auto& program = *programs.Programs[8].Get();
    auto values = EffectValues(source->Views.front(), destination->OutputSize, source->OutputSize);
    const auto isSrgb = [](TextureFormat f) { return f == TextureFormat::RGBA8_UNORM_SRGB || f == TextureFormat::BGRA8_UNORM_SRGB || f == TextureFormat::RGBA16_FLOAT; };
    values.DebugMode = 5;
    auto sourceOutput = FindGraphOutput(outputs, overlay.Source), destinationOutput = FindGraphOutput(outputs, overlay.Destination);
    if (!sourceOutput || !destinationOutput) return false;
    values.Options = Eigen::Vector4f{1, 0, isSrgb(graph.GetTextureDescriptor(destinationOutput->Texture)->Format) ? 0.f : 1.f, isSrgb(graph.GetTextureDescriptor(sourceOutput->Texture)->Format) ? 0.f : 1.f};
    const auto input = sourceOutput->Texture;
    auto& output = destinationOutput->Texture;
    const RgParameterBinding resources[]{{"InputA", 0, RgTextureParameterBinding{input}}, {"InputB", 0, RgTextureParameterBinding{input}}};
    const auto& r = overlay.Rectangle;
    const auto size = destination->OutputSize;
    const Rect rectangle{int32_t(r.X * size.Width), int32_t(r.Y * size.Height), uint32_t(r.Width * size.Width), uint32_t(r.Height * size.Height)};
    if (rectangle.Width == 0 || rectangle.Height == 0) return true;
    return Composite(graph, program, values, resources, output, rectangle, rectangle, render::LoadAction::Load, backend, success, "Forward.ObserverComposite").IsValid();
}

}  // namespace radray::forward_detail
