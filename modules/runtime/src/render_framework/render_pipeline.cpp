#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/render_graph_runtime.h>
#include <radray/logger.h>
#include <algorithm>

namespace radray {

struct RenderPipelineContext::ImportedOutput {
    uint32_t SurfaceIndex;
    array<render::TextureStates, 1> States;
    array<uint8_t, 1> Valid;
    std::optional<RenderExternalTexture> External;
    RgTextureHandle Handle;
};
RenderPipelineContext::RenderPipelineContext(AppFrameContext& frame, RenderGraphFrameResources& graphResources, render::RenderPassRegistry& registry,
                                             ViewStateRegistry& views, uint64_t serial, std::span<const ResolvedRenderViewFamily> families, std::span<RenderSurfaceFrame> surfaces, RenderGraphExecutionReport& report)
    : _frame(frame), _graphResources(graphResources), _registry(registry), _views(views), _serial(serial), _families(families), _surfaces(surfaces), _report(report) {}
RenderPipelineContext::~RenderPipelineContext() = default;
uint32_t RenderPipelineContext::FlightIndex() const noexcept { return _frame.FlightIndex(); }
const render::RenderDeviceCapabilities& RenderPipelineContext::Capabilities() const noexcept { return _frame.GetDevice()->GetCapabilities(); }
HostWriteBatch& RenderPipelineContext::HostWrites() const noexcept { return _frame.GetHostWrites(); }
RenderGraph RenderPipelineContext::CreateRenderGraph(std::string_view name) {
    if (_graphGeneration != 0) RADRAY_ABORT("Only one RenderGraph may be created per Render invocation");
    return RenderGraph{*_frame.GetDevice(), _graphResources, _registry, name, _graphGeneration};
}
RgTextureHandle RenderPipelineContext::ImportOutput(RenderGraph& graph, RenderOutputId output) {
    if (_executed || _graphGeneration != graph.GetGeneration()) return {};
    for (const auto& entry : _intermediates)
        if (entry.first == output) return entry.second;
    return ImportOutputTarget(graph, output);
}
bool RenderPipelineContext::SetOutputIntermediate(RenderGraph& graph, RenderOutputId output, RgTextureHandle texture) {
    if (_executed || _graphGeneration != graph.GetGeneration()) return false;
    auto desc = graph.GetTextureDescriptor(texture);
    if (!desc) return false;
    for (const auto& entry : _intermediates)
        if (entry.first == output) return false;
    for (const auto& surface : _surfaces) {
        if (surface.Id != output) continue;
        if (desc->Width != surface.Desc.Width || desc->Height != surface.Desc.Height || desc->Format != surface.Desc.Format || desc->SampleCount != 1) return false;
        _intermediates.emplace_back(output, texture);
        return true;
    }
    return false;
}
RgTextureHandle RenderPipelineContext::ImportOutputTarget(RenderGraph& graph, RenderOutputId output) {
    if (_executed || _graphGeneration == 0) return {};
    if (_graphGeneration != graph.GetGeneration()) return {};
    for (const auto& imported : _imports)
        if (_surfaces[imported->SurfaceIndex].Id == output) return imported->Handle;
    for (uint32_t index = 0; index < _surfaces.size(); ++index) {
        auto& surface = _surfaces[index];
        if (surface.Id != output) continue;
        auto imported = make_unique<ImportedOutput>();
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
RenderGraphExecutionResult RenderPipelineContext::ExecuteGraph(RenderGraph& graph) {
    if (_executed || _graphGeneration != graph.GetGeneration()) return {};
    _graphGeneration = graph.GetGeneration();
    _executed = true;
    const auto result = graph.Execute(*_frame.GetCommandBuffer());
    _success = result.Success;
    for (const auto& imported : _imports) {
        auto& surface = _surfaces[imported->SurfaceIndex];
        surface.CurrentState = imported->States[0];
        surface.Written = result.Success && imported->External->Written && imported->Valid[0] != 0;
    }
    if (result.Success)
        for (const auto& history : _histories)
            if (history.CommitToken.CommitMode == HistoryCommitMode::Independent && history.Current && history.Current->Written) _views.CommitHistory(history.CommitToken);
    for (auto& completion : _completions)
        completion.Executed = result.Success && graph.PassWroteTexture(completion.Pass, completion.Output);
    _report = graph.GetReport();
    if (!result.Success) RADRAY_ERR_LOG("{}", _report.ToText());
    return result;
}
bool RenderPipelineContext::CommitView(ViewStateId id) {
    if (!_success || std::find(_failedTemporalViews.begin(), _failedTemporalViews.end(), id) != _failedTemporalViews.end()) return false;
    for (const auto& family : _families)
        for (const auto& view : family.Views) {
            if (view.StateId != id) continue;
            for (const auto& surface : _surfaces)
                if (surface.Id == family.OutputId && surface.Written) return _views.CommitView(id);
        }
    return false;
}
ViewCompletionToken RenderPipelineContext::RegisterViewCompletion(RenderGraph& graph, ViewStateId id, RgPassHandle pass) {
    if (_executed || _graphGeneration != graph.GetGeneration() || pass.Generation != _graphGeneration ||
        pass.Index >= graph.GetReport().Passes.size() || !id.IsValid()) return {};
    for (const auto& completion : _completions)
        if (completion.View == id || completion.Pass == pass) return {};
    for (const auto& family : _families) {
        if (!family.OutputAvailable) continue;
        for (const auto& view : family.Views) {
            if (view.StateId != id) continue;
            const auto output = ImportOutput(graph, family.OutputId);
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
    if (!_success || token._view != id || token._graph != _graphGeneration ||
        token._serial != _serial || token._index >= _completions.size()) return false;
    const auto& completion = _completions[token._index];
    if (completion.View != id || !completion.Executed || std::find(_failedTemporalViews.begin(), _failedTemporalViews.end(), id) != _failedTemporalViews.end()) return false;
    if (!requiredDrawsSucceeded) {
        _failedTemporalViews.push_back(id);
        return false;
    }
    vector<HistoryWriteToken> tokens;
    for (const auto& history : _histories)
        if (history.CommitToken.View == id && history.CommitToken.CommitMode == HistoryCommitMode::WithView) tokens.push_back(history.CommitToken);
    return _views.CommitViewWithHistory(id, tokens);
}
void RenderPipelineContext::InvalidateView(ViewStateId id) {
    if (_executed) return;
    _views.InvalidateTemporal(id);
}
bool RenderPipelineContext::PreparePrimitiveHistory(ResolvedRenderView& view, const RenderSceneSnapshot& snapshot) {
    if (_executed) return false;
    const bool success = _views.PreparePrimitiveHistory(view, snapshot);
    if (!success) _failedTemporalViews.push_back(view.StateId);
    return success;
}
PrimitiveMotionData RenderPipelineContext::GetPrimitiveMotion(ViewStateId id, const RenderPrimitiveData& primitive) const noexcept {
    return _views.GetPrimitiveMotion(id, primitive);
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
