#include <radray/runtime/forward_pipeline/forward_pipeline.h>

#include "depth_only_mesh_pass_processor.h"
#include "forward_lit_mesh_pass_processor.h"
#include "forward_frame.h"

#include <algorithm>
#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/viewport.h>
#include <radray/runtime/render_system.h>

namespace radray {
using namespace forward_detail;
namespace {

constexpr render::TextureFormat kForwardDepthCandidates[]{render::TextureFormat::D32_FLOAT, render::TextureFormat::D24_UNORM_S8_UINT, render::TextureFormat::D16_UNORM};
enum class ForwardPass { Depth,
                         Opaque,
                         Transparent };

render::ShaderProgramLayoutRecipe MakeLayoutRecipe(std::span<const std::string_view> names) {
    render::ShaderProgramLayoutRecipe recipe;
    for (const auto name : names) {
        const render::ShaderLayoutSelector selector{.DeclarationName = string{name}, .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
        recipe.D3D12.BufferPlacements.push_back({.Selector = selector, .Placement = render::D3D12BufferPlacement::RootDescriptor});
        recipe.Vulkan.BufferDescriptors.push_back({.Selector = selector, .Placement = render::VulkanBufferDescriptorPlacement::Dynamic});
    }
    return recipe;
}

}  // namespace

struct ForwardPipeline::Impl {
    struct FlightResources {
        RenderSceneSnapshot Scene;
        unique_ptr<FrameDrawResources> DrawResources;
        vector<ForwardFamilyDrawWork> Families;
        ForwardStageBStats Stats;
    };

    Impl(Application* application, Scene* renderScene, CameraComponent* viewCamera)
        : RenderScene(renderScene), ViewCamera(viewCamera), Device(application->GetDevice()) {
        Flights.resize(application->GetGpuSystem()->GetFlightDataCount());
    }

    Scene* RenderScene;
    CameraComponent* ViewCamera;
    render::Device* Device;
    ForwardBindingCache Bindings;
    DepthOnlyBindingCache DepthBindings;
    vector<FlightResources> Flights;
    unordered_map<RenderOutputId, ViewStateId, RenderOutputIdHash> ViewIds;
    bool LightOverflowWarned{false}, InvalidBoundsWarned{false}, CullingFailureWarned{false};

    bool BeginFrame(RenderPipelineContext& frame) {
        if (frame.FlightIndex() >= Flights.size()) return false;
        auto& flight = Flights[frame.FlightIndex()];
        for (auto& family : flight.Families)
            for (auto& view : family.Views) view.ResetForReuse();
        flight.Families.resize(frame.ViewFamilies().size());
        if (!flight.DrawResources) {
            DynamicCBufferArena::Descriptor descriptor;
            descriptor.BasicSize = descriptor.MaxResetSize = 1024 * 1024;
            descriptor.NamePrefix = "FrameDrawResources";
            flight.DrawResources = make_unique<FrameDrawResources>(Device, descriptor);
        }
        return flight.DrawResources->BeginFrame(frame.HostWrites());
    }

    bool PrepareFamily(uint32_t flightIndex, const ResolvedRenderViewFamily& family) {
        auto& flight = Flights[flightIndex];
        auto& work = flight.Families[family.FrameLocalIndex];
        work.Views.resize(family.Views.size());
        bool valid = false;
        for (uint32_t index = 0; index < family.Views.size(); ++index) {
            auto& view = work.Views[index];
            view.View = family.Views[index];
            ++flight.Stats.CullCalls;
            if (!Cull({&flight.Scene, &view.View}, view.Culling)) {
                ++flight.Stats.CullFailures;
                if (!CullingFailureWarned) {
                    RADRAY_ERR_LOG("Forward culling rejected an invalid view; its draw lists are skipped");
                    CullingFailureWarned = true;
                }
                continue;
            }
            valid = true;
            DepthOnlyMeshPassProcessor depth{*flight.DrawResources, DepthBindings};
            ForwardLitMeshPassProcessor opaque{*flight.DrawResources, Bindings, LightOverflowWarned};
            ForwardLitMeshPassProcessor transparent{*flight.DrawResources, Bindings, LightOverflowWarned};
            BuildRendererList({"DepthOnly", "DepthOnly", &view.Culling, &view.View, RenderQueueRange::Opaque(), 0xffffffffu, RendererListSorting::FrontToBack}, depth, view.DepthOnly);
            BuildRendererList({"Opaque", "ForwardLit", &view.Culling, &view.View, RenderQueueRange::Opaque()}, opaque, view.Opaque);
            BuildRendererList({"Transparent", "ForwardLit", &view.Culling, &view.View, RenderQueueRange::Transparent(), 0xffffffffu, RendererListSorting::BackToFront}, transparent, view.Transparent);
            flight.Stats.DepthCommands += view.DepthOnly.Commands.size();
            flight.Stats.OpaqueCommands += view.Opaque.Commands.size();
            flight.Stats.TransparentCommands += view.Transparent.Commands.size();
        }
        return valid;
    }

    void Execute(uint32_t flightIndex, uint32_t familyIndex, ForwardPass pass, RenderGraphRasterContext& ctx) {
        auto& flight = Flights[flightIndex];
        for (const auto& work : flight.Families[familyIndex].Views) {
            if (!work.Culling.Stats.Valid) continue;
            const auto& view = work.View;
            ctx.Encoder().SetViewport(MakeViewport(Device->GetBackend(), static_cast<float>(view.ViewRect.X), static_cast<float>(view.ViewRect.Y),
                                                   static_cast<float>(view.ViewRect.Width), static_cast<float>(view.ViewRect.Height)));
            ctx.Encoder().SetScissor(view.ScissorRect);
            const auto& list = pass == ForwardPass::Depth ? work.DepthOnly : (pass == ForwardPass::Opaque ? work.Opaque : work.Transparent);
            SubmitRendererList(list, ctx, ctx.PassState(), flight.Stats.Execution);
        }
    }
};

ForwardPipeline::ForwardPipeline(Application* app, Scene* scene, CameraComponent* camera)
    : _impl(make_unique<Impl>(app, scene, camera)) {}
ForwardPipeline::~ForwardPipeline() noexcept = default;

void ForwardPipeline::PrepareFrame(RenderPrepareContext& ctx) {
    const uint32_t index = ctx.App.FlightIndex;
    RADRAY_ASSERT(index < _impl->Flights.size());
    auto& flight = _impl->Flights[index];
    flight.Stats = {};
    ++flight.Stats.SnapshotBuilds;
    if (!BuildRenderSceneSnapshot(*_impl->RenderScene, flight.Scene, ctx.RetainedAssets)) {
        RADRAY_ERR_LOG("Forward scene snapshot exceeded its frame-local index capacity");
        return;
    }
    if (flight.Scene.Stats.InvalidBounds && !_impl->InvalidBoundsWarned) {
        RADRAY_WARN_LOG("Forward scene contains invalid bounds; these primitives remain conservatively visible");
        _impl->InvalidBoundsWarned = true;
    }
    for (const auto& output : ctx.Outputs) {
        if (!output.Active || output.Kind != RenderOutputKind::Presentation) continue;
        auto& id = _impl->ViewIds[output.Id];
        if (!id.IsValid()) id = AllocateViewStateId();
        RenderViewDesc view = CollectRenderView(*_impl->ViewCamera);
        view.StateId = id;
        ctx.Workloads.AddViewFamily({"Forward " + output.Name, output.Id, 1, {std::move(view)}});
    }
}

void ForwardPipeline::Render(RenderPipelineContext& ctx) {
    if (!_impl->BeginFrame(ctx)) return;
    auto graph = ctx.CreateRenderGraph("Forward");
    struct PassData {
        Impl* Pipeline;
        uint32_t Flight, Family;
        ForwardPass Pass;
    };
    vector<ViewStateId> rendered;
    for (const auto& family : ctx.ViewFamilies()) {
        if (!_impl->PrepareFamily(ctx.FlightIndex(), family) || !family.OutputAvailable || family.RenderSize != family.OutputSize) continue;
        const auto usage = render::TextureUse::DepthStencilWrite | render::TextureUse::DepthStencilRead;
        const auto format = SelectFirstSupportedFormat(*_impl->Device, kForwardDepthCandidates, render::TextureDimension::Dim2D, usage, family.SampleCount);
        if (!format) continue;
        RuntimeTextureDesc depthDesc;
        depthDesc.Extent.Mode = RenderExtentMode::RelativeToFamilyRenderExtent;
        depthDesc.Format = *format;
        depthDesc.Usage = usage;
        depthDesc.SampleCount = family.SampleCount;
        string reason;
        const auto descriptor = ResolveRuntimeTextureDesc(depthDesc, family, *_impl->Device, reason);
        if (!descriptor) continue;
        const auto color = ctx.ImportOutput(graph, family.OutputId);
        const auto depth = graph.CreateTexture(*descriptor, "Forward.Depth");
        const auto& work = _impl->Flights[ctx.FlightIndex()].Families[family.FrameLocalIndex];
        const bool hasDepth = std::any_of(work.Views.begin(), work.Views.end(), [](const auto& view) { return !view.DepthOnly.Commands.empty(); });
        const bool hasTransparent = std::any_of(work.Views.begin(), work.Views.end(), [](const auto& view) { return !view.Transparent.Commands.empty(); });
        for (const auto pass : {ForwardPass::Depth, ForwardPass::Opaque, ForwardPass::Transparent}) {
            if ((pass == ForwardPass::Depth && !hasDepth) || (pass == ForwardPass::Transparent && !hasTransparent)) continue;
            const auto name = pass == ForwardPass::Depth ? "Forward.DepthPrepass" : (pass == ForwardPass::Opaque ? "Forward.Opaque" : "Forward.Transparent");
            graph.AddRasterPass<PassData>(name, [&](PassData& data, RenderGraphRasterBuilder& builder) {
                data = {_impl.get(), ctx.FlightIndex(), family.FrameLocalIndex, pass};
                if (pass != ForwardPass::Depth) {
                    builder.SetColorAttachment(0, color, {.Load = pass == ForwardPass::Opaque ? render::LoadAction::Clear : render::LoadAction::Load,
                                                         .Clear = {{.025f, .030f, .040f, 1}}});
                }
                builder.SetDepthAttachment(depth, {.Load = pass == ForwardPass::Depth || (pass == ForwardPass::Opaque && !hasDepth) ? render::LoadAction::Clear : render::LoadAction::Load,
                                                    .ReadOnly = pass == ForwardPass::Transparent}); }, +[](const PassData& data, RenderGraphRasterContext& context) { data.Pipeline->Execute(data.Flight, data.Family, data.Pass, context); });
        }
        for (const auto& view : work.Views)
            if (view.Culling.Stats.Valid) rendered.push_back(view.View.StateId);
    }
    if (ctx.ExecuteGraph(graph).Success)
        for (const auto view : rendered) ctx.CommitView(view);
}

const RenderSceneSnapshot& ForwardPipeline::GetSceneSnapshot(uint32_t flightIndex) const noexcept { return _impl->Flights[flightIndex].Scene; }
const ForwardStageBStats& ForwardPipeline::GetStageBStats(uint32_t flightIndex) const noexcept { return _impl->Flights[flightIndex].Stats; }
std::span<const forward_detail::ForwardViewDrawWork> ForwardPipeline::GetViewWork(uint32_t flightIndex, uint32_t familyIndex) const noexcept { return _impl->Flights[flightIndex].Families[familyIndex].Views; }

render::ShaderProgramLayoutRecipe ForwardPipeline::GetLayoutRecipe() noexcept {
    static constexpr std::string_view names[]{"ForwardView", "ForwardMaterial", "ForwardObject"};
    return MakeLayoutRecipe(names);
}
render::ShaderProgramLayoutRecipe ForwardPipeline::GetDepthOnlyLayoutRecipe() noexcept {
    static constexpr std::string_view names[]{"ForwardView", "ForwardObject"};
    return MakeLayoutRecipe(names);
}

}  // namespace radray
