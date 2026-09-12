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
#include <radray/profiler.h>
#include <radray/runtime/forward_pipeline/forward_graph.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/viewport.h>
#include <radray/runtime/render_system.h>

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
        shared_ptr<const RenderSceneSnapshot> Scene{make_shared<RenderSceneSnapshot>()};
        // Object cbuffer rows frozen on the game thread, indexed by snapshot primitive.
        ForwardObjectDataCache Objects;
        unique_ptr<FrameDrawResources> DrawResources;
        vector<ForwardFamilyDrawWork> Families;
        vector<unique_ptr<ForwardHdrView>> HdrViews;
        ForwardPipelineSettings Settings;
        vector<ForwardOutputOverlay> Overlays;
        vector<ForwardOutputSurface> Surfaces;
        vector<RenderOutputId> SurfaceOrder;
        bool ProgramsReady{false};
        ForwardCapture Capture;
        ForwardStageBStats Stats;
        size_t HdrViewCount{0};
        bool OverlaysSucceeded{true};
        vector<ViewStateId> RenderedViews;
        unordered_set<ViewStateId, ViewStateIdHash> AuxiliaryViews;
        unique_ptr<ForwardHdrView> SharedShadowView;
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
    ForwardEffectTemplates EffectTemplates;
    vector<ForwardViewSource> Sources;
    vector<ForwardOutputOverlay> Overlays;
    vector<ForwardOutputSurface> Surfaces;
    vector<RenderOutputId> SurfaceOrder;
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
        RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.BeginFrame");
        if (frame.FlightIndex() >= Flights.size()) return false;
        auto& flight = Flights[frame.FlightIndex()];
        for (auto& view : flight.HdrViews) view->Reset();
        if (flight.SharedShadowView) flight.SharedShadowView->Reset();
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

    struct ViewWorkRequest {
        FlightResources* Flight;
        ForwardViewDrawWork* Output;
        shared_ptr<DepthOnlyMeshPassProcessor> Depth;
        shared_ptr<ForwardLitMeshPassProcessor> Lit;
        RenderValidationMode Validation;
    };
    struct LdrFamilyTemplate {
        render::TextureDescriptor ColorDesc, DepthDesc;
        size_t ViewCount;
        shared_ptr<const RenderGraphTemplate> Graph;
        RgTexturePort ColorInput;
        RgTextureValue ColorOutput;
        vector<RgTemplateSlot<ViewWorkRequest>> WorkSlots;
        vector<RgWorkHandle> Works;
        array<RgTemplateSlot<ForwardGraphFrameData>, 3> StageSlots;
        uint64_t Used{0};
    };
    vector<LdrFamilyTemplate> LdrTemplates;
    uint64_t TemplateUseSerial{0};
    static bool PrepareViewWork(ViewWorkRequest& request, uint64_t mask) {
        RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.Prepare");
        auto& flight = *request.Flight;
        auto& view = *request.Output;
        view.RequestedRoles = mask;
        ++view.CullCalls;
        ++flight.Stats.CullCalls;
        if (!Cull({flight.Scene.get(), &view.View}, view.Culling)) {
            ++flight.Stats.CullFailures;
            return false;
        }
        if (mask & kForwardWorkDepth) {
            request.Depth->ResetView();
            if (!BuildRendererList({"DepthOnly", "DepthOnly", &view.Culling, &view.View, RenderQueueRange::Opaque(), 0xffffffffu,
                                    RendererListSorting::FrontToBack, false, request.Validation, kDepthOnlyPolicy},
                                   *request.Depth, view.DepthOnly) ||
                !view.DepthOnly.Stats.ContentSucceeded()) return false;
        }
        array<RendererListDesc, 2> descriptions;
        array<RendererList*, 2> outputs{};
        uint32_t count = 0;
        if (mask & kForwardWorkOpaque) {
            descriptions[count] = {"Opaque", "ForwardLit", &view.Culling, &view.View, RenderQueueRange::Opaque(), 0xffffffffu,
                                   RendererListSorting::StateThenFrontToBack, false, request.Validation, kForwardLitPolicy};
            outputs[count++] = &view.Opaque;
        }
        if (mask & kForwardWorkTransparent) {
            descriptions[count] = {"Transparent", "ForwardLit", &view.Culling, &view.View, RenderQueueRange::Transparent(), 0xffffffffu,
                                   RendererListSorting::BackToFront, false, request.Validation, kForwardLitPolicy};
            outputs[count++] = &view.Transparent;
        }
        request.Lit->ResetView();
        if (count && !BuildRendererLists(std::span{descriptions.data(), count}, *request.Lit, std::span{outputs.data(), count})) return false;
        for (uint32_t i = 0; i < count; ++i)
            if (!outputs[i]->Stats.ContentSucceeded()) return false;
        flight.Stats.DepthCommands += view.DepthOnly.GetDrawCount();
        flight.Stats.OpaqueCommands += view.Opaque.GetDrawCount();
        flight.Stats.TransparentCommands += view.Transparent.GetDrawCount();
        return true;
    }
    bool BuildLdrFamily(RenderGraph& graph, RenderPipelineContext& context, const ResolvedRenderViewFamily& family,
                        const render::TextureDescriptor& depthDesc, RgTextureValue& color) {
        const auto colorDesc = graph.GetTextureDescriptor(color);
        if (!colorDesc) return false;
        LdrFamilyTemplate* cached = nullptr;
        for (auto& candidate : LdrTemplates)
            if (candidate.ViewCount == family.Views.size() && TexturePoolKey{candidate.ColorDesc} == TexturePoolKey{*colorDesc} && TexturePoolKey{candidate.DepthDesc} == TexturePoolKey{depthDesc}) {
                cached = &candidate;
                break;
            }
        if (!cached) {
            auto builder = graph.CreateTemplateBuilder("Forward.LdrFamily");
            LdrFamilyTemplate next;
            next.ColorDesc = *colorDesc;
            next.DepthDesc = depthDesc;
            next.ViewCount = family.Views.size();
            next.ColorInput = builder.DeclareTexturePort(*colorDesc, "Forward.ColorInput");
            auto depth = builder.CreateTexture(depthDesc, "Forward.Depth");
            for (size_t index = 0; index < family.Views.size(); ++index) {
                const auto slot = builder.DeclareTemplateSlot<ViewWorkRequest>();
                next.WorkSlots.push_back(slot);
                next.Works.push_back(builder.AddTemplateWork("Forward.MainViewWork", slot, PrepareViewWork));
            }
            array<vector<ForwardGraphView>, 3> views;
            constexpr uint64_t masks[]{kForwardWorkDepth, kForwardWorkOpaque, kForwardWorkTransparent};
            for (size_t stage = 0; stage < views.size(); ++stage) {
                next.StageSlots[stage] = builder.DeclareTemplateSlot<ForwardGraphFrameData>();
                for (const auto work : next.Works) views[stage].push_back({.Work = work, .WorkMask = masks[stage]});
            }
            const auto depthStage = ForwardGraph::DeclareTemplate(builder, ForwardGraphStage::Depth, next.StageSlots[0],
                                                                  {.Name = "Forward.DepthPrepass", .Views = views[0], .Depth = depth, .DepthAttachment = {.Load = render::LoadAction::Clear}});
            const auto opaqueStage = ForwardGraph::DeclareTemplate(builder, ForwardGraphStage::Opaque, next.StageSlots[1],
                                                                   {.Name = "Forward.Opaque", .Views = views[1], .Color = builder.Value(next.ColorInput), .Depth = depthStage.Depth, .ColorAttachment = {.Load = render::LoadAction::Clear, .Clear = {{.025f, .030f, .040f, 1}}}, .DepthAttachment = {.Load = render::LoadAction::Load}});
            const auto transparentStage = ForwardGraph::DeclareTemplate(builder, ForwardGraphStage::Transparent, next.StageSlots[2],
                                                                        {.Name = "Forward.Transparent", .Views = views[2], .Color = opaqueStage.Color, .Depth = opaqueStage.Depth, .ColorAttachment = {.Load = render::LoadAction::Load}, .DepthAttachment = {.Load = render::LoadAction::Load}});
            if (!depthStage.Success || !opaqueStage.Success || !transparentStage.Success) return false;
            next.ColorOutput = transparentStage.Color;
            next.Graph = builder.FreezeTemplate();
            if (!next.Graph) return false;
            if (LdrTemplates.size() < 4) {
                LdrTemplates.push_back(std::move(next));
                cached = &LdrTemplates.back();
            } else {
                cached = &*std::min_element(LdrTemplates.begin(), LdrTemplates.end(), [](const auto& a, const auto& b) { return a.Used < b.Used; });
                *cached = std::move(next);
            }
        }
        cached->Used = ++TemplateUseSerial;
        const auto instance = graph.Instantiate(cached->Graph);
        if (!instance.IsValid() || !graph.Connect(instance.Value(cached->ColorInput), color)) return false;
        auto& flight = Flights[context.FlightIndex()];
        auto& work = flight.Families[family.FrameLocalIndex];
        work.Views.resize(family.Views.size());
        auto depth = make_shared<DepthOnlyMeshPassProcessor>(*flight.DrawResources, DepthBindings, flight.Objects.Rows());
        auto lit = make_shared<ForwardLitMeshPassProcessor>(*flight.DrawResources, Bindings, LightOverflowWarned, flight.Objects.Rows());
        array<vector<ForwardGraphView>, 3> views;
        for (size_t index = 0; index < family.Views.size(); ++index) {
            auto& view = work.Views[index];
            view.View = family.Views[index];
            view.Work = instance.Value(cached->Works[index]);
            if (!instance.Bind(cached->WorkSlots[index], make_shared<ViewWorkRequest>(ViewWorkRequest{&flight, &view, depth, lit, context.GetRuntimeOptions().Validation}))) return false;
            views[0].push_back({.View = view.View, .List = &view.DepthOnly});
            views[1].push_back({.View = view.View, .List = &view.Opaque});
            views[2].push_back({.View = view.View, .List = &view.Transparent});
        }
        for (size_t stage = 0; stage < views.size(); ++stage)
            if (!instance.Bind(cached->StageSlots[stage], ForwardGraph::MakeFrame({.Backend = Device->GetBackend(), .Views = views[stage], .Execution = &flight.Stats.Execution}))) return false;
        color = instance.Value(cached->ColorOutput);
        for (const auto& view : work.Views) flight.RenderedViews.push_back(view.View.StateId);
        return true;
    }
};

ForwardPipeline::ForwardPipeline(Application* app, Scene* scene, CameraComponent* camera)
    : _impl(make_unique<Impl>(app, scene, camera)) {}
ForwardPipeline::~ForwardPipeline() noexcept = default;

bool ForwardPipeline::SetSettings(const ForwardPipelineSettings& settings) noexcept {
    if (!settings.IsValid() || (!settings.Hdr && !_impl->Surfaces.empty())) return false;
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

bool ForwardPipeline::SetOutputSurfaces(std::span<const ForwardOutputSurface> surfaces) {
    if (!surfaces.empty() && !_impl->Settings.Hdr) return false;
    vector<RenderOutputId> pending, order;
    for (const auto& surface : surfaces) {
        const auto& matrix = surface.LocalToWorld;
        if (!surface.Source.IsValid() || !surface.Destination.IsValid() || surface.Source == surface.Destination ||
            !matrix.allFinite() || !matrix.row(3).isApprox(Eigen::RowVector4f{0, 0, 0, 1}) ||
            !std::isfinite(surface.Brightness) || surface.Brightness < 0) return false;
        for (const auto output : {surface.Source, surface.Destination})
            if (std::find(pending.begin(), pending.end(), output) == pending.end()) pending.push_back(output);
    }
    while (!pending.empty()) {
        const auto ready = std::find_if(pending.begin(), pending.end(), [&](RenderOutputId output) {
            return std::none_of(surfaces.begin(), surfaces.end(), [&](const auto& surface) {
                return surface.Destination == output && std::find(pending.begin(), pending.end(), surface.Source) != pending.end();
            });
        });
        if (ready == pending.end()) return false;
        order.push_back(*ready);
        pending.erase(ready);
    }
    _impl->Surfaces.assign(surfaces.begin(), surfaces.end());
    _impl->SurfaceOrder = std::move(order);
    return true;
}

void ForwardPipeline::CollectScenePolicies(RenderPrepareContext& ctx) {
    if (!RegisterForwardPassPolicies(ctx, *_impl->RenderScene)) _impl->Error = true;
}

void ForwardPipeline::PrepareFrame(RenderPrepareContext& ctx) {
    RADRAY_PROFILE_SCOPE_N("ForwardPipeline::PrepareFrame");
    const uint32_t index = ctx.App.FlightIndex;
    RADRAY_ASSERT(index < _impl->Flights.size());
    auto& flight = _impl->Flights[index];
    flight.Settings = _impl->Settings;
    flight.Overlays = _impl->Overlays;
    flight.Surfaces = _impl->Surfaces;
    flight.SurfaceOrder = _impl->SurfaceOrder;
    flight.Capture.Directory = _impl->CaptureDirectory;
    flight.Capture.Name = std::exchange(_impl->CaptureName, {});
    flight.ProgramsReady = !flight.Settings.Hdr || _impl->Effects.Initialize(*_impl->System);
    ++_impl->PreparedSerial;
    flight.Stats = {};
    ++flight.Stats.SnapshotBuilds;
    bool snapshotOk = false;
    {
        RADRAY_PROFILE_SCOPE_N("SceneSnapshotBuild");
        auto snapshot = ctx.PrepareScene(*_impl->RenderScene);
        snapshotOk = snapshot.HasValue();
        if (snapshotOk) flight.Scene = snapshot.Release();
    }
    if (!snapshotOk) {
        RADRAY_ERR_LOG("Forward scene snapshot exceeded its frame-local index capacity");
        flight.Objects.Clear();
        return;
    }
    flight.Stats.ObjectValueUpdates = flight.Objects.Update(*flight.Scene);
    if (flight.Scene->Stats.InvalidBounds && !_impl->InvalidBoundsWarned) {
        RADRAY_WARN_LOG("Forward scene contains invalid bounds; these primitives remain conservatively visible");
        _impl->InvalidBoundsWarned = true;
    }
    flight.AuxiliaryViews.clear();
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
                    if (source.Auxiliary)
                        flight.AuxiliaryViews.insert(views.back().StateId);
                    else
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

void ForwardPipeline::BuildGraph(RenderPipelineContext& ctx, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) {
    RADRAY_PROFILE_SCOPE_N("ForwardPipeline::BuildGraph");
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
    flight.RenderedViews.clear();
    flight.HdrViewCount = 0;
    if (flight.Settings.Hdr) {
        bool overlaysSucceeded = true;
        size_t viewIndex = 0;
        vector<const ResolvedRenderViewFamily*> families;
        for (const auto& family : ctx.ViewFamilies()) families.push_back(&family);
        for (const auto& surface : flight.Surfaces) {
            const auto source = std::find_if(families.begin(), families.end(), [&](const auto* family) { return family->OutputId == surface.Source; });
            const auto destination = std::find_if(families.begin(), families.end(), [&](const auto* family) { return family->OutputId == surface.Destination; });
            if (source == families.end() || destination == families.end() ||
                ((*destination)->OutputAvailable && !(*source)->OutputAvailable)) {
                _impl->Error = true;
                RADRAY_ERR_LOG("Forward output surface requires both camera outputs in this frame");
                return;
            }
        }
        const auto rank = [&](RenderOutputId output) { return std::find(flight.SurfaceOrder.begin(), flight.SurfaceOrder.end(), output) - flight.SurfaceOrder.begin(); };
        std::stable_sort(families.begin(), families.end(), [&](const auto* a, const auto* b) { return rank(a->OutputId) < rank(b->OutputId); });
        const ResolvedRenderView* primary = nullptr;
        for (const auto* family : families) {
            if (!family->OutputAvailable) continue;
            for (const auto& view : family->Views) {
                if (flight.AuxiliaryViews.contains(view.StateId)) continue;
                primary = &view;
                break;
            }
            if (primary) break;
        }
        if (!primary) {
            for (const auto* family : families) {
                if (!family->OutputAvailable || family->Views.empty()) continue;
                primary = &family->Views.front();
                break;
            }
        }
        ForwardShadowAtlas atlas{};
        if (primary) {
            if (!flight.SharedShadowView) flight.SharedShadowView = make_unique<ForwardHdrView>();
            if (!DeclareForwardSharedShadows(graph, _impl->EffectTemplates, flight.Settings, *primary, *flight.Scene, flight.Objects.Rows(), *flight.DrawResources, _impl->Bindings,
                                             *flight.SharedShadowView, _impl->Device->GetBackend(), _impl->LightOverflowWarned, atlas)) {
                _impl->Error = true;
                return;
            }
        }
        auto hdrLit = [&] {
            RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.Processor");
            return make_shared<ForwardLitMeshPassProcessor>(*flight.DrawResources, _impl->Bindings, _impl->LightOverflowWarned, flight.Objects.Rows(), &ctx);
        }();
        for (const auto* familyPointer : families) {
            const auto& family = *familyPointer;
            bool firstOutput = true;
            for (const auto& view : family.Views) {
                if (!family.OutputAvailable) continue;
                const bool auxiliary = flight.AuxiliaryViews.contains(view.StateId);
                const ForwardViewSignature signature{{view.ViewRect.Width, view.ViewRect.Height}, view.ViewRect, family.OutputFormat, flight.Settings, auxiliary};
                const auto previous = _impl->Signatures.find(view.StateId);
                if (previous == _impl->Signatures.end() || !previous->second.Matches(signature)) ctx.InvalidateView(view.StateId);
                _impl->Signatures.insert_or_assign(view.StateId, signature);
                if (viewIndex == flight.HdrViews.size()) flight.HdrViews.push_back(make_unique<ForwardHdrView>());
                auto& work = *flight.HdrViews[viewIndex++];
                if (BuildForwardHdrView(graph, _impl->EffectTemplates, ctx, *_impl->Device, _impl->Effects, flight.Settings, family, view, *flight.Scene, flight.Objects.Rows(),
                                        *flight.DrawResources, _impl->Bindings, work, firstOutput, _impl->LightOverflowWarned, flight.Surfaces, outputs,
                                        atlas, auxiliary, hdrLit))
                    firstOutput = false;
                else
                    _impl->Error = true;
            }
        }
        for (const auto& overlay : flight.Overlays)
            overlaysSucceeded &= BuildForwardOutputOverlay(graph, _impl->EffectTemplates, ctx, _impl->Effects, overlay, _impl->Device->GetBackend(), outputs);
        if (!flight.Capture.Build(graph, ctx, *_impl->Device, *_impl->System, outputs)) _impl->Error = true;
        flight.HdrViewCount = viewIndex;
        RADRAY_PROFILE_PLOT("Forward.HdrViews", static_cast<int64_t>(viewIndex));
        flight.OverlaysSucceeded = overlaysSucceeded;
        return;
    }
    for (const auto& family : ctx.ViewFamilies()) {
        for (const auto& view : family.Views)
            if (_impl->Signatures.erase(view.StateId)) ctx.InvalidateView(view.StateId);
        if (!family.OutputAvailable || family.RenderSize != family.OutputSize) continue;
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
        auto target = FindGraphOutput(outputs, family.OutputId);
        if (!target) continue;
        if (!_impl->BuildLdrFamily(graph, ctx, family, *descriptor, target->Texture)) {
            _impl->Error = true;
            RADRAY_ERR_LOG("Forward graph stage declaration failed for '{}'", family.Name);
        }
    }
    if (!flight.Capture.Build(graph, ctx, *_impl->Device, *_impl->System, outputs)) _impl->Error = true;
}

void ForwardPipeline::GraphRecorded(RenderPipelineContext& ctx, const RenderGraph& graph, RenderGraphExecutionResult result) {
    RADRAY_PROFILE_SCOPE_N("ForwardPipeline::GraphRecorded");
    auto& flight = _impl->Flights[ctx.FlightIndex()];
    if (flight.Settings.Hdr) {
        flight.Capture.CaptureReport(graph.GetReport());
        if (result.Success) {
            auto* sharedShadows = flight.SharedShadowView.get();
            const bool shadowsOk = !sharedShadows || (sharedShadows->ContentValid && sharedShadows->PassesSucceeded && sharedShadows->Execution.Succeeded());
            for (size_t i = 0; i < flight.HdrViewCount; ++i) {
                auto& work = *flight.HdrViews[i];
                const bool content = flight.OverlaysSucceeded && work.ContentValid && work.PassesSucceeded && work.Execution.Succeeded() && shadowsOk;
                if (!content) {
                    _impl->Error = true;
                    const auto describe = [](const RendererList& list) { const auto& s = list.Stats; return fmt::format("valid={} required={} bindings={} geometry={} prepare={} rejected={}", s.Valid, s.MissingRequiredPass, s.InvalidBindings, s.InvalidGeometry, s.PrepareResourceFailed, s.ProcessorRejected); };
                    auto& cascades = sharedShadows ? sharedShadows->Cascades : work.Cascades;
                    RADRAY_ERR_LOG("Forward view '{}' incomplete: content={} passes={} pso={} bind={} skip={} depth[{}] opaque[{}] transparent[{}] shadows[{}/{}/{}/{}]",
                                   work.Main.View.Name, work.ContentValid, work.PassesSucceeded, work.Execution.PsoFailure, work.Execution.BindingFailure, work.Execution.Skipped,
                                   describe(work.Main.DepthOnly), describe(work.Main.Opaque), describe(work.Main.Transparent), describe(cascades[0].DepthOnly), describe(cascades[1].DepthOnly), describe(cascades[2].DepthOnly), describe(cascades[3].DepthOnly));
                }
                ctx.CommitView(work.Main.View.StateId, work.Completion, content);
                flight.Stats.TemporalViews += work.TemporalHistory;
                flight.Stats.ValidTemporalHistories += work.TemporalHistory && work.HistoryValid;
                flight.Stats.CullCalls += work.Main.CullCalls;
                flight.Stats.DepthCommands += work.Main.DepthOnly.GetDrawCount();
                flight.Stats.OpaqueCommands += work.Main.Opaque.GetDrawCount();
                flight.Stats.TransparentCommands += work.Main.Transparent.GetDrawCount();
                flight.Stats.Execution.Commands += work.Execution.Commands;
                flight.Stats.Execution.Draws += work.Execution.Draws;
                flight.Stats.Execution.PsoFailure += work.Execution.PsoFailure;
                flight.Stats.Execution.BindingFailure += work.Execution.BindingFailure;
                flight.Stats.Execution.Skipped += work.Execution.Skipped;
            }
            if (sharedShadows && flight.HdrViewCount > 0) {
                for (const auto& cascade : sharedShadows->Cascades) flight.Stats.CullCalls += cascade.CullCalls;
                flight.Stats.Execution.Commands += sharedShadows->Execution.Commands;
                flight.Stats.Execution.Draws += sharedShadows->Execution.Draws;
                flight.Stats.Execution.PsoFailure += sharedShadows->Execution.PsoFailure;
                flight.Stats.Execution.BindingFailure += sharedShadows->Execution.BindingFailure;
                flight.Stats.Execution.Skipped += sharedShadows->Execution.Skipped;
            }
        } else {
            _impl->Error = true;
            RADRAY_ERR_LOG("Forward graph failed: {}", graph.GetReport().ToText());
        }
        return;
    }
    flight.Capture.CaptureReport(graph.GetReport());
    if (result.Success && flight.Stats.Execution.Succeeded())
        for (const auto view : flight.RenderedViews) ctx.CommitView(view);
    else {
        _impl->Error = true;
        RADRAY_ERR_LOG("Forward frame failed: {}", graph.GetReport().ToText());
    }
}

const RenderSceneSnapshot& ForwardPipeline::GetSceneSnapshot(uint32_t flightIndex) const noexcept { return *_impl->Flights[flightIndex].Scene; }
FrameDrawResourceStats ForwardPipeline::GetFrameDrawResourceStats(uint32_t flightIndex) const noexcept {
    const auto& resources = _impl->Flights[flightIndex].DrawResources;
    return resources ? resources->GetStats() : FrameDrawResourceStats{};
}
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
