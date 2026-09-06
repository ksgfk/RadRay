#include <radray/runtime/forward_pipeline/forward_pipeline.h>

#include "depth_only_mesh_pass_processor.h"
#include "forward_lit_mesh_pass_processor.h"
#include "forward_frame.h"
#include "forward_effects.h"
#include "forward_capture.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <radray/logger.h>
#include <radray/runtime/forward_pipeline/forward_graph.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/viewport.h>
#include <radray/runtime/render_system.h>
#ifdef RADRAY_ENABLE_IMGUI
#include <radray/runtime/imgui/imgui_graph.h>
#endif

namespace radray {
using namespace forward_detail;

ForwardPipelineSettings ForwardPipelineSettings::Temporal() noexcept {
    ForwardPipelineSettings settings;
    settings.Hdr = settings.Shadows = settings.ForwardPlus = settings.AmbientOcclusion = settings.Bloom = true;
    settings.Antialiasing = ForwardAntialiasing::Temporal;
    return settings;
}
ForwardPipelineSettings ForwardPipelineSettings::Msaa() noexcept {
    auto settings = Temporal();
    settings.Antialiasing = ForwardAntialiasing::Msaa4;
    settings.AmbientOcclusion = false;
    return settings;
}
bool ForwardPipelineSettings::IsValid() const noexcept {
    if (!EnumContains(Antialiasing) || !EnumContains(DebugView) || !std::isfinite(RenderScale) || RenderScale < .25f || RenderScale > 1 ||
        !std::isfinite(Exposure) || Exposure <= 0 || !std::isfinite(ShadowDistance) || ShadowDistance <= 0 ||
        !std::isfinite(AoRadius) || AoRadius <= 0 || !std::isfinite(BloomStrength) || BloomStrength < 0 ||
        ShadowResolution < 16 || ShadowResolution > 8192 || MaxLocalLights == 0 || MaxLocalLights > 256 ||
        MaxLightsPerTile == 0 || MaxLightsPerTile > 64) return false;
    if (!Hdr && (Shadows || ForwardPlus || AmbientOcclusion || Bloom || Fireflies || Antialiasing != ForwardAntialiasing::None || RenderScale != 1 || DebugView != ForwardDebugView::Final)) return false;
    if (DebugView == ForwardDebugView::HistoryHdr && Antialiasing != ForwardAntialiasing::Temporal) return false;
    if (Antialiasing == ForwardAntialiasing::Msaa4 && (AmbientOcclusion || DebugView == ForwardDebugView::LinearDepth ||
                                                       DebugView == ForwardDebugView::Normals || DebugView == ForwardDebugView::Motion ||
                                                       DebugView == ForwardDebugView::AmbientOcclusion || DebugView == ForwardDebugView::DepthPyramid)) return false;
    return true;
}

namespace {

constexpr render::TextureFormat kForwardDepthCandidates[]{render::TextureFormat::D32_FLOAT, render::TextureFormat::D24_UNORM_S8_UINT, render::TextureFormat::D16_UNORM};

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
        vector<unique_ptr<ForwardHdrView>> HdrViews;
        ForwardPipelineSettings Settings;
        vector<ForwardOutputOverlay> Overlays;
        bool ProgramsReady{false};
        ForwardCapture Capture;
        ForwardStageBStats Stats;
    };

    Impl(Application* application, Scene* renderScene, CameraComponent* viewCamera)
        : RenderScene(renderScene), ViewCamera(viewCamera), Device(application->GetDevice()), System(application->GetRenderSystem()) {
        Flights.resize(application->GetGpuSystem()->GetFlightDataCount());
    }

    Scene* RenderScene;
    CameraComponent* ViewCamera;
    render::Device* Device;
    RenderSystem* System;
    ForwardPipelineSettings Settings;
    ForwardEffectPrograms Effects;
    vector<ForwardViewSource> Sources;
    vector<ForwardOutputOverlay> Overlays;
    unordered_map<ViewStateId, ForwardViewSignature, ViewStateIdHash> Signatures;
    uint64_t PreparedSerial{0};
    std::filesystem::path CaptureDirectory;
    string CaptureName;
    std::atomic_bool Error{false};
    ForwardBindingCache Bindings;
    DepthOnlyBindingCache DepthBindings;
    vector<FlightResources> Flights;
    unordered_map<RenderOutputId, ViewStateId, RenderOutputIdHash> ViewIds;
    bool LightOverflowWarned{false}, InvalidBoundsWarned{false}, CullingFailureWarned{false};

    bool BeginFrame(RenderPipelineContext& frame) {
        if (frame.FlightIndex() >= Flights.size()) return false;
        auto& flight = Flights[frame.FlightIndex()];
        for (auto& view : flight.HdrViews) view->Reset();
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
};

ForwardPipeline::ForwardPipeline(Application* app, Scene* scene, CameraComponent* camera)
    : _impl(make_unique<Impl>(app, scene, camera)) {}
ForwardPipeline::~ForwardPipeline() noexcept = default;

bool ForwardPipeline::SetSettings(const ForwardPipelineSettings& settings) noexcept {
    if (!settings.IsValid()) return false;
    _impl->Settings = settings;
    return true;
}
const ForwardPipelineSettings& ForwardPipeline::GetSettings() const noexcept { return _impl->Settings; }
void ForwardPipeline::RequestCapture(std::filesystem::path directory, string name) {
    _impl->CaptureDirectory = std::move(directory);
    _impl->CaptureName = std::move(name);
}
bool ForwardPipeline::CompleteCaptures(uint32_t flightIndex) {
    if (flightIndex >= _impl->Flights.size() || !_impl->Flights[flightIndex].Capture.Complete()) _impl->Error = true;
    return !_impl->Error;
}
bool ForwardPipeline::Failed() const noexcept { return _impl->Error; }
bool ForwardPipeline::SetViews(std::span<const ForwardViewSource> views) {
    vector<ForwardViewSource> sources{views.begin(), views.end()};
    unordered_set<ViewStateId, ViewStateIdHash> ids;
    for (auto& source : sources) {
        string reason;
        if (!source.Output.IsValid() || !ValidateRenderView(source.View, reason)) return false;
        if (!source.View.StateId.IsValid()) source.View.StateId = AllocateViewStateId();
        if (!ids.insert(source.View.StateId).second) return false;
    }
    _impl->Sources = std::move(sources);
    return true;
}

bool ForwardPipeline::SetOutputOverlays(std::span<const ForwardOutputOverlay> overlays) {
    for (const auto& overlay : overlays) {
        const auto& r = overlay.Rectangle;
        if (!overlay.Source.IsValid() || !overlay.Destination.IsValid() || overlay.Source == overlay.Destination ||
            !std::isfinite(r.X) || !std::isfinite(r.Y) || !std::isfinite(r.Width) || !std::isfinite(r.Height) ||
            r.X < 0 || r.Y < 0 || r.Width <= 0 || r.Height <= 0 || r.X + r.Width > 1 || r.Y + r.Height > 1) return false;
    }
    _impl->Overlays.assign(overlays.begin(), overlays.end());
    return true;
}

void ForwardPipeline::PrepareFrame(RenderPrepareContext& ctx) {
    const uint32_t index = ctx.App.FlightIndex;
    RADRAY_ASSERT(index < _impl->Flights.size());
    auto& flight = _impl->Flights[index];
    flight.Settings = _impl->Settings;
    flight.Overlays = _impl->Overlays;
    flight.Capture.Directory = _impl->CaptureDirectory;
    flight.Capture.Name = std::exchange(_impl->CaptureName, {});
    flight.ProgramsReady = !flight.Settings.Hdr || _impl->Effects.Initialize(*_impl->System);
    ++_impl->PreparedSerial;
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
    const auto jitter = [&](RenderViewDesc& view) {
        if (flight.Settings.Antialiasing != ForwardAntialiasing::Temporal) return;
        const auto radicalInverse = [](uint64_t value, uint32_t base) {
            float sum = 0, factor = 1;
            while (value) {
                factor /= float(base);
                sum += factor * float(value % base);
                value /= base;
            }
            return sum;
        };
        const auto sample = (_impl->PreparedSerial - 1) % 8 + 1;
        view.JitterPixels = {radicalInverse(sample, 2) - .5f, radicalInverse(sample, 3) - .5f};
    };
    if (!_impl->Sources.empty()) {
        for (const auto& output : ctx.Outputs) {
            if (!output.Active) continue;
            vector<RenderViewDesc> views;
            for (const auto& source : _impl->Sources)
                if (source.Output == output.Id) {
                    views.push_back(source.View);
                    jitter(views.back());
                }
            if (!views.empty()) ctx.Workloads.AddViewFamily({"Forward " + output.Name, output.Id, flight.Settings.RenderScale, std::move(views)});
        }
        return;
    }
    for (const auto& output : ctx.Outputs) {
        if (!output.Active || output.Kind != RenderOutputKind::Presentation || output.Usage != RenderOutputUsage::Scene) continue;
        auto& id = _impl->ViewIds[output.Id];
        if (!id.IsValid()) id = AllocateViewStateId();
        RenderViewDesc view = CollectRenderView(*_impl->ViewCamera);
        view.StateId = id;
        jitter(view);
        ctx.Workloads.AddViewFamily({"Forward " + output.Name, output.Id, flight.Settings.RenderScale, {std::move(view)}});
    }
}

void ForwardPipeline::Render(RenderPipelineContext& ctx) {
    if (!_impl->BeginFrame(ctx)) {
        _impl->Error = true;
        return;
    }
    auto& flight = _impl->Flights[ctx.FlightIndex()];
    if (!flight.ProgramsReady) {
        RADRAY_ERR_LOG("Forward required effect programs are unavailable");
        _impl->Error = true;
        return;
    }
    auto graph = ctx.CreateRenderGraph("Forward");
#ifdef RADRAY_ENABLE_IMGUI
    auto ui = _impl->System->GetApplication()->GetImGuiSystem();
    const auto uiScenes = ui ? ImGuiGraph::PrepareSceneOutputs(graph, ctx) : vector<ImGuiSceneOutput>{};
#endif
    vector<ViewStateId> rendered;
    if (flight.Settings.Hdr) {
        bool overlaysSucceeded = true;
        size_t viewIndex = 0;
        for (const auto& family : ctx.ViewFamilies()) {
            bool firstOutput = true;
            for (const auto& view : family.Views) {
                if (!family.OutputAvailable) continue;
                const ForwardViewSignature signature{{view.ViewRect.Width, view.ViewRect.Height}, view.ViewRect, family.OutputFormat, flight.Settings};
                const auto previous = _impl->Signatures.find(view.StateId);
                if (previous == _impl->Signatures.end() || !previous->second.Matches(signature)) ctx.InvalidateView(view.StateId);
                _impl->Signatures.insert_or_assign(view.StateId, signature);
                if (viewIndex == flight.HdrViews.size()) flight.HdrViews.push_back(make_unique<ForwardHdrView>());
                auto& work = *flight.HdrViews[viewIndex++];
                if (BuildForwardHdrView(graph, ctx, *_impl->Device, _impl->Effects, flight.Settings, family, view, flight.Scene,
                                        *flight.DrawResources, _impl->Bindings, work, firstOutput, _impl->LightOverflowWarned))
                    firstOutput = false;
                else
                    _impl->Error = true;
            }
        }
        for (const auto& overlay : flight.Overlays)
            overlaysSucceeded &= BuildForwardOutputOverlay(graph, ctx, _impl->Effects, overlay, _impl->Device->GetBackend(), overlaysSucceeded);
        if (!flight.Capture.Build(graph, ctx, *_impl->Device)) _impl->Error = true;
#ifdef RADRAY_ENABLE_IMGUI
        if (ui) ImGuiGraph::BuildGraph(graph, ctx, *ui.Get(), uiScenes);
#endif
        const auto result = ctx.ExecuteGraph(graph);
#ifdef RADRAY_ENABLE_IMGUI
        if (ui) ImGuiGraph::CompleteGraph(graph, ctx, *ui.Get(), result.Success);
#endif
        flight.Capture.Report = graph.GetReport().ToJson();
        flight.Capture.Dot = graph.GetReport().ToDot();
        if (result.Success) {
            for (size_t i = 0; i < viewIndex; ++i) {
                auto& work = *flight.HdrViews[i];
                const bool content = overlaysSucceeded && work.ContentValid && work.PassesSucceeded && work.Execution.Succeeded();
                if (!content) {
                    _impl->Error = true;
                    const auto describe = [](const RendererList& list) { const auto& s = list.Stats; return fmt::format("valid={} required={} bindings={} geometry={} prepare={} rejected={}", s.Valid, s.MissingRequiredPass, s.InvalidBindings, s.InvalidGeometry, s.PrepareResourceFailed, s.ProcessorRejected); };
                    RADRAY_ERR_LOG("Forward view '{}' incomplete: content={} passes={} pso={} bind={} skip={} depth[{}] opaque[{}] transparent[{}] shadows[{}/{}/{}/{}]",
                                   work.Main.View.Name, work.ContentValid, work.PassesSucceeded, work.Execution.PsoFailure, work.Execution.BindingFailure, work.Execution.Skipped,
                                   describe(work.Main.DepthOnly), describe(work.Main.Opaque), describe(work.Main.Transparent), describe(work.Cascades[0].DepthOnly), describe(work.Cascades[1].DepthOnly), describe(work.Cascades[2].DepthOnly), describe(work.Cascades[3].DepthOnly));
                }
                ctx.CommitView(work.Main.View.StateId, work.Completion, content);
                flight.Stats.CullCalls += 1 + (flight.Settings.Shadows ? 4 : 0);
                flight.Stats.DepthCommands += work.Main.DepthOnly.Commands.size();
                flight.Stats.OpaqueCommands += work.Main.Opaque.Commands.size();
                flight.Stats.TransparentCommands += work.Main.Transparent.Commands.size();
                flight.Stats.Execution.Commands += work.Execution.Commands;
                flight.Stats.Execution.Draws += work.Execution.Draws;
                flight.Stats.Execution.PsoFailure += work.Execution.PsoFailure;
                flight.Stats.Execution.BindingFailure += work.Execution.BindingFailure;
                flight.Stats.Execution.Skipped += work.Execution.Skipped;
            }
        } else {
            _impl->Error = true;
            RADRAY_ERR_LOG("Forward graph failed: {}", graph.GetReport().ToText());
        }
        return;
    }
    for (const auto& family : ctx.ViewFamilies()) {
        for (const auto& view : family.Views)
            if (_impl->Signatures.erase(view.StateId)) ctx.InvalidateView(view.StateId);
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
        vector<ForwardGraphView> depthViews, opaqueViews, transparentViews;
        for (const auto& view : work.Views) {
            if (!view.Culling.Stats.Valid) continue;
            depthViews.push_back({view.View, &view.DepthOnly});
            opaqueViews.push_back({view.View, &view.Opaque});
            transparentViews.push_back({view.View, &view.Transparent});
        }
        auto& execution = _impl->Flights[ctx.FlightIndex()].Stats.Execution;
        const ForwardGraphStageOutput depthStage = ForwardGraph::BuildGraph(
            graph, ForwardGraphStage::Depth,
            {.Name = "Forward.DepthPrepass",
             .Backend = _impl->Device->GetBackend(),
             .Views = depthViews,
             .Color = {},
             .Depth = depth,
             .ColorAttachment = {},
             .DepthAttachment = {.Load = render::LoadAction::Clear},
             .Execution = &execution});
        const ForwardGraphStageOutput opaqueStage = ForwardGraph::BuildGraph(
            graph, ForwardGraphStage::Opaque,
            {.Name = "Forward.Opaque",
             .Backend = _impl->Device->GetBackend(),
             .Views = opaqueViews,
             .Color = color,
             .Depth = depth,
             .ColorAttachment = {.Load = render::LoadAction::Clear,
                                 .Clear = {{.025f, .030f, .040f, 1}}},
             .DepthAttachment = {.Load = depthStage.Pass.IsValid()
                                             ? render::LoadAction::Load
                                             : render::LoadAction::Clear},
             .Execution = &execution});
        const ForwardGraphStageOutput transparentStage = ForwardGraph::BuildGraph(
            graph, ForwardGraphStage::Transparent,
            {.Name = "Forward.Transparent",
             .Backend = _impl->Device->GetBackend(),
             .Views = transparentViews,
             .Color = color,
             .Depth = depth,
             .ColorAttachment = {.Load = render::LoadAction::Load},
             .DepthAttachment = {.Load = render::LoadAction::Load},
             .Execution = &execution});
        if (!depthStage.Success || !opaqueStage.Success || !transparentStage.Success) {
            RADRAY_ERR_LOG("Forward graph stage declaration failed for '{}'", family.Name);
        }
        for (const auto& view : work.Views)
            if (view.Culling.Stats.Valid) rendered.push_back(view.View.StateId);
    }
    if (!flight.Capture.Build(graph, ctx, *_impl->Device)) _impl->Error = true;
#ifdef RADRAY_ENABLE_IMGUI
    if (ui) ImGuiGraph::BuildGraph(graph, ctx, *ui.Get(), uiScenes);
#endif
    const auto result = ctx.ExecuteGraph(graph);
#ifdef RADRAY_ENABLE_IMGUI
    if (ui) ImGuiGraph::CompleteGraph(graph, ctx, *ui.Get(), result.Success);
#endif
    flight.Capture.Report = graph.GetReport().ToJson();
    flight.Capture.Dot = graph.GetReport().ToDot();
    if (result.Success && flight.Stats.Execution.Succeeded())
        for (const auto view : rendered) ctx.CommitView(view);
    else {
        _impl->Error = true;
        RADRAY_ERR_LOG("Forward frame failed: {}", graph.GetReport().ToText());
    }
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
