#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/render_graph_runtime.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/application.h>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <algorithm>

namespace radray {

bool RenderPrepareContext::RegisterScene(const Scene& scene) {
    if (std::find(RegisteredScenes.begin(), RegisteredScenes.end(), &scene) != RegisteredScenes.end()) return true;
    if (ScenesFrozen) return false;
    RegisteredScenes.push_back(&scene);
    return true;
}

bool RenderPrepareContext::FreezeRegisteredScenes() {
    if (ScenesFrozen) return true;
    if (!PolicyRegistrationValid) return false;
    for (const auto* scene : RegisteredScenes) {
        vector<PassPolicy> policies;
        for (const auto& registration : ScenePolicies)
            if (registration.Source == scene) policies.push_back(registration.Policy);
        if (!scene->GetDrawStore().SetActivePolicies(PrepareSerial, policies)) return false;
        if (!scene->GetRenderState().PrepareShared(*scene, PrepareSerial, App.FlightIndex, RetainedAssets, RuntimeOptions.Validation)) return false;
    }
    ScenesFrozen = true;
    return true;
}

bool RenderPrepareContext::RegisterScenePolicy(const Scene& scene, const PassPolicy& policy) {
    if (ScenesFrozen || !policy.Id.IsValid() || policy.Revision == 0 || policy.PassName.empty() || !policy.CompileStatic || !RegisterScene(scene)) {
        PolicyRegistrationValid = false;
        return false;
    }
    for (const auto& registration : ScenePolicies)
        if (registration.Source == &scene && registration.Policy.Id == policy.Id) {
            const bool same = registration.Policy == policy;
            PolicyRegistrationValid &= same;
            return same;
        }
    ScenePolicies.push_back({&scene, policy});
    return true;
}

Nullable<shared_ptr<const RenderSceneSnapshot>> RenderPrepareContext::PrepareScene(const Scene& scene) const {
    if (!ScenesFrozen || std::find(RegisteredScenes.begin(), RegisteredScenes.end(), &scene) == RegisteredScenes.end()) return nullptr;
    return scene.GetRenderState().PrepareShared(scene, PrepareSerial, App.FlightIndex, RetainedAssets, RuntimeOptions.Validation);
}

struct RenderPipelineContext::ImportedOutput {
    uint32_t SurfaceIndex;
    array<render::TextureStates, 1> States;
    array<uint8_t, 1> Valid;
    std::optional<RenderExternalTexture> External;
    RgTextureValue Handle;
};
RenderPipelineContext::RenderPipelineContext(AppFrameContext& frame, RenderGraphFrameResources& graphResources, render::RenderPassRegistry& registry,
                                             ViewStateRegistry& views, uint64_t serial, std::span<const ResolvedRenderViewFamily> families, std::span<RenderSurfaceFrame> surfaces, RenderGraphExecutionReport& report,
                                             RenderGraphRuntimeOptions runtime)
    : _frame(frame), _graphResources(graphResources), _registry(registry), _views(views), _serial(serial), _families(families), _surfaces(surfaces), _report(report), _runtimeOptions(runtime) {}
RenderPipelineContext::~RenderPipelineContext() = default;
shared_ptr<FrameGraphTemplateCache>& RenderPipelineContext::DefaultCompositionCacheStorage() noexcept {
    return _graphResources.DefaultCompositionCacheStorage();
}
uint32_t RenderPipelineContext::FlightIndex() const noexcept { return _frame.FlightIndex(); }
const render::RenderDeviceCapabilities& RenderPipelineContext::Capabilities() const noexcept { return _frame.GetDevice()->GetCapabilities(); }
render::RenderBackend RenderPipelineContext::Backend() const noexcept { return _frame.GetDevice()->GetBackend(); }
HostWriteBatch& RenderPipelineContext::HostWrites() const noexcept { return _frame.GetHostWrites(); }
RenderGraph RenderPipelineContext::CreateRenderGraph(std::string_view name) {
    if (_graphGeneration != 0) RADRAY_ABORT("Only one RenderGraph may be created per Render invocation");
    return RenderGraph{*_frame.GetDevice(), _graphResources, _registry, name, _graphGeneration, _report, _runtimeOptions};
}
RgTextureValue RenderPipelineContext::ImportOutputTarget(RenderGraph& graph, RenderOutputId output) {
    if (_executed || _graphGeneration == 0) return {};
    if (_graphGeneration != graph.GetGeneration()) return {};
    for (const auto& imported : _imports)
        if (_surfaces[imported->SurfaceIndex].Id == output) return imported->Handle;
    for (uint32_t index = 0; index < _surfaces.size(); ++index) {
        auto& surface = _surfaces[index];
        if (surface.Id != output) continue;
        auto imported = make_shared<ImportedOutput>();
        imported->SurfaceIndex = index;
        imported->States = {surface.CurrentState};
        imported->Valid = {surface.PreserveContents ? uint8_t{1} : uint8_t{0}};
        imported->External.emplace(RenderExternalTexture{surface.Texture, surface.Desc, imported->States, imported->Valid, surface.ColorAttachmentView});
        imported->Handle = graph.ImportTexture(*imported->External, fmt::format("Output.{}", output.Value), RenderGraphExternalAccess::ObservableOutput);
        const auto handle = imported->Handle;
        _imports.push_back(std::move(imported));
        return handle;
    }
    return {};
}
RgTextureValue RenderPipelineContext::BindOutputTarget(RenderGraph& graph, const RenderGraphTemplateInstance& instance, RgTextureValue slot, RenderOutputId output) {
    if (_executed || !_graphGeneration || _graphGeneration != graph.GetGeneration()) return {};
    const auto value = instance.Value(slot);
    if (value.Generation != _graphGeneration) return {};
    for (const auto& imported : _imports)
        if (_surfaces[imported->SurfaceIndex].Id == output)
            return instance.Bind(slot, *imported->External) ? value : RgTextureValue{};
    for (uint32_t index = 0; index < _surfaces.size(); ++index) {
        auto& surface = _surfaces[index];
        if (surface.Id != output) continue;
        auto imported = make_shared<ImportedOutput>();
        imported->SurfaceIndex = index;
        imported->States = {surface.CurrentState};
        imported->Valid = {surface.PreserveContents ? uint8_t{1} : uint8_t{0}};
        imported->External.emplace(RenderExternalTexture{surface.Texture, surface.Desc, imported->States, imported->Valid, surface.ColorAttachmentView});
        if (!instance.Bind(slot, *imported->External)) return {};
        imported->Handle = value;
        _imports.push_back(std::move(imported));
        return value;
    }
    return {};
}
RenderGraphExecutionResult RenderPipelineContext::ExecuteGraph(RenderGraph& graph) {
    if (_executed || _graphGeneration != graph.GetGeneration()) return {};
    _graphGeneration = graph.GetGeneration();
    _executed = true;
    vector<RenderGraph::PresentCommandTarget> presentTargets;
    presentTargets.reserve(_surfaces.size());
    render::CommandBuffer* shared = _frame.GetCommandBuffer();
    for (const RenderSurfaceFrame& surface : _surfaces) {
        render::CommandBuffer* commands = _frame.GetCommandBufferForTexture(surface.Texture);
        if (commands != nullptr && commands != shared) {
            presentTargets.push_back({surface.Texture, commands});
        }
    }
    _executingGraph = &graph;
    struct ExecuteScope {
        Nullable<const RenderGraph*>& Active;
        ~ExecuteScope() { Active = nullptr; }
    } scope{_executingGraph};
    const auto result = graph.Execute(*shared, presentTargets);
    _success = result.Success;
    _submission = result.Submission;
    if (_submission) {
        // External output wrappers and their state spans survive the Submit callback.
        const auto imports = _imports;
        const auto commitGraph = _submission->OnSubmitted;
        _submission->OnSubmitted = [imports, commitGraph] { if (commitGraph) commitGraph(); };
        _frame.TrackSubmission(_submission);
    }
    for (const auto& imported : _imports) {
        auto& surface = _surfaces[imported->SurfaceIndex];
        if (auto state = graph.RecordedTextureState(imported->Handle)) surface.CurrentState = *state;
        surface.Written = result.Success && graph.WasWritten(imported->Handle);
    }
    if (result.Success)
        for (const auto& history : _histories)
            if (history.CommitToken.CommitMode == HistoryCommitMode::Independent && history.Current && _submission) {
                const auto token = history.CommitToken;
                auto* views = &_views;
                auto* current = history.Current.Get();
                const auto prior = _submission->OnSubmitted;
                _submission->OnSubmitted = [prior, token, views, current] { if (prior) prior(); if (current->Written) views->CommitHistory(token); };
            }
    for (auto& completion : _completions)
        completion.Executed = result.Success && graph.PassWroteTexture(completion.Pass, completion.Output);
    for (const auto& history : _histories) _recordedHistories.push_back(history.Current && graph.WasWritten(*history.Current));
    if (!result.Success) RADRAY_ERR_LOG("{}", _report.ToText());
    return result;
}
bool RenderPipelineContext::CommitView(ViewStateId id) {
    RADRAY_PROFILE_SCOPE_N("RenderPipelineContext::QueueViewCommit");
    if (!_success || std::find(_failedTemporalViews.begin(), _failedTemporalViews.end(), id) != _failedTemporalViews.end() || std::find(_queuedViews.begin(), _queuedViews.end(), id) != _queuedViews.end()) return false;
    for (const auto& family : _families)
        for (const auto& view : family.Views) {
            if (view.StateId != id) continue;
            for (const auto& surface : _surfaces)
                if (surface.Id == family.OutputId && surface.Written && _submission) {
                    for (const auto& history : _histories)
                        if (history.CommitToken.View == id && history.CommitToken.CommitMode == HistoryCommitMode::WithView) return false;
                    _queuedViews.push_back(id);
                    auto* views = &_views;
                    const auto prior = _submission->OnSubmitted;
                    _submission->OnSubmitted = [prior, views, id] { if (prior) prior(); views->CommitView(id); };
                    return true;
                }
        }
    return false;
}
ViewCompletionToken RenderPipelineContext::RegisterViewCompletion(RenderGraph& graph, ViewStateId id, RgPassHandle pass, RgTextureValue output) {
    if (_executed || _graphGeneration != graph.GetGeneration() || pass.Generation != _graphGeneration ||
        pass.Index >= graph.GetPassCount() || !id.IsValid()) return {};
    for (const auto& completion : _completions)
        if (completion.View == id || completion.Pass == pass) return {};
    for (const auto& family : _families) {
        if (!family.OutputAvailable) continue;
        for (const auto& view : family.Views) {
            if (view.StateId != id) continue;
            if (!output.IsValid()) return {};
            ViewCompletionToken token;
            token._view = id;
            token._graph = _graphGeneration;
            token._serial = _serial;
            token._index = static_cast<uint32_t>(_completions.size());
            _completions.push_back({id, pass, output});
            return token;
        }
    }
    return {};
}
bool RenderPipelineContext::CommitView(ViewStateId id, const ViewCompletionToken& token, bool requiredDrawsSucceeded) {
    RADRAY_PROFILE_SCOPE_N("RenderPipelineContext::QueueViewCommit");
    if (!_success || token._view != id || token._graph != _graphGeneration ||
        token._serial != _serial || token._index >= _completions.size() ||
        std::find(_queuedViews.begin(), _queuedViews.end(), id) != _queuedViews.end()) return false;
    auto& completion = _completions[token._index];
    if (completion.View != id || !completion.Executed || completion.Consumed || std::find(_failedTemporalViews.begin(), _failedTemporalViews.end(), id) != _failedTemporalViews.end()) return false;
    completion.Consumed = true;
    if (!requiredDrawsSucceeded) {
        _failedTemporalViews.push_back(id);
        return false;
    }
    vector<HistoryWriteToken> tokens;
    for (size_t i = 0; i < _histories.size(); ++i) {
        const auto& history = _histories[i];
        if (history.CommitToken.View == id && history.CommitToken.CommitMode == HistoryCommitMode::WithView) {
            if (!_recordedHistories[i]) return false;
            tokens.push_back(history.CommitToken);
        }
    }
    if (!_submission) return false;
    _queuedViews.push_back(id);
    auto* views = &_views;
    const auto prior = _submission->OnSubmitted;
    _submission->OnSubmitted = [prior, views, id, tokens = std::move(tokens)] { if (prior) prior(); views->CommitViewWithHistory(id, tokens); };
    return true;
}
void RenderPipelineContext::InvalidateView(ViewStateId id) {
    if (_executed) return;
    _views.InvalidateTemporal(id);
}
bool RenderPipelineContext::PreparePrimitiveHistory(ResolvedRenderView& view, const RenderSceneSnapshot& snapshot) {
    if (_executed && (!_executingGraph || !_executingGraph->IsPreparingWork())) return false;
    const bool success = _views.PreparePrimitiveHistory(view, snapshot);
    if (!success) _failedTemporalViews.push_back(view.StateId);
    return success;
}
PrimitiveMotionData RenderPipelineContext::GetPrimitiveMotion(ViewStateId id, const RenderPrimitiveData& primitive) const noexcept {
    return _views.GetPrimitiveMotion(id, primitive);
}
PrimitiveHistoryStamp RenderPipelineContext::GetPrimitiveHistoryStamp(ViewStateId id) const noexcept {
    return _views.GetPrimitiveHistoryStamp(id);
}
HistoryTexturePair RenderPipelineContext::AcquireHistoryTexture(const ResolvedRenderView& view, const ResolvedRenderViewFamily& family,
                                                                const HistoryTextureRequest& request, string& reason) {
    if (_executed) {
        reason = "History cannot be acquired after graph execution";
        return {};
    }
    auto result = _views.AcquireHistoryTexture(view, family, request, reason);
    if (result.Current)
        _histories.push_back(result);
    else if (request.CommitMode == HistoryCommitMode::WithView)
        _failedTemporalViews.push_back(view.StateId);
    return result;
}
RenderPipeline::~RenderPipeline() noexcept = default;
void RenderPipeline::PrepareFrame(RenderPrepareContext& ctx) { ctx.Workloads.AddPresentationOutputs(); }

}  // namespace radray
