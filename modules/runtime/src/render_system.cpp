#include <radray/runtime/render_system.h>

#include "shader_program_cache.h"

#include <limits>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>

namespace radray {
RenderSystem::RenderSystem(Application* app, uint32_t flightCount) : _app(app), _frameUpdates(flightCount) {
    if (flightCount == 0) RADRAY_ABORT("Scene delivery requires at least one flight");
}
RenderSystem::~RenderSystem() noexcept { OnShutdown(); }

void RenderSystem::CheckCanModifyGT() const {
    if (std::this_thread::get_id() != _ownerThread || _collecting) RADRAY_ABORT("Scene mutation requires GT outside collection");
}
void RenderSystem::SetCollecting(bool collecting) {
    if (collecting) CheckCanModifyGT();
    _collecting = collecting;
    _app->SetCollecting(collecting);
}

void RenderSystem::OnShutdown() noexcept {
    for (const auto& record : _scenesGT.Values()) {
        if (record.Writer->_claimed) RADRAY_ABORT("Disconnect all Worlds before shutting down RenderSystem");
    }
    BeginStoppingGT();
    AbandonUnpublishedFramesGT();
    _scenesRT.clear();
    _scenesGT.Clear();
    _frameUpdates.clear();
    _shaderCache.reset();
    _renderPassRegistry.reset();
}

bool RenderSystem::OnInitialize() {
    auto gpu = _gpuSystem;
    if (_app == nullptr || !gpu || gpu->GetDevice() == nullptr) {
        RADRAY_ERR_LOG("initialize RenderSystem failed: {}", "Application, GpuSystem or Device is missing");
        return false;
    }
    render::Device* device = gpu->GetDevice();
    if (_frameUpdates.size() != gpu->GetFlightDataCount()) {
        RADRAY_ERR_LOG("initialize RenderSystem failed: {}", "Scene flight count does not match GpuSystem");
        return false;
    }
    _renderPassRegistry = make_unique<render::RenderPassRegistry>(device);
    _shaderCache = make_unique<ShaderProgramCache>(*device, _app->GetShaderSourceRoot(), _app->GetShaderIncludePaths());
    return true;
}

SceneId RenderSystem::CreateSceneGT() {
    CheckCanModifyGT();
    if (_frameUpdates.empty() || _stopping) RADRAY_ABORT("RenderSystem is stopping or shut down");
    const auto handle = _scenesGT.Emplace();
    const SceneId id{handle.Index, handle.Generation};
    _scenesGT.Get(handle).Writer = make_unique<SceneWriter>(id, static_cast<uint32_t>(_frameUpdates.size()));
    return id;
}

Nullable<SceneWriter*> RenderSystem::GetSceneWriterGT(SceneId id) noexcept {
    auto record = _scenesGT.TryGet({id.Index, id.Generation});
    if (!record || record->Writer->_closing || record->Writer->_claimed) return nullptr;
    return record->Writer.get();
}

SceneWriter& RenderSystem::ClaimSceneWriterGT(SceneId id) {
    auto writer = GetSceneWriterGT(id);
    if (!writer) RADRAY_ABORT("Scene already claimed or unavailable");
    writer->_claimed = true;
    return *writer.Get();
}

void RenderSystem::ReleaseSceneWriterGT(SceneId id) {
    auto record = _scenesGT.TryGet({id.Index, id.Generation});
    if (!record || !record->Writer->_claimed) RADRAY_ABORT("Scene is not claimed");
    record->Writer->_claimed = false;
}

void RenderSystem::DestroySceneGT(SceneId id) {
    CheckCanModifyGT();
    auto record = _scenesGT.TryGet({id.Index, id.Generation});
    if (!record || record->Writer->_closing || record->Writer->_claimed) RADRAY_ABORT("Invalid scene destruction");
    record->Writer->_closing = true;
}

RenderSystem::FrameUpdates& RenderSystem::GetFrameUpdates(uint32_t flightIndex) {
    if (flightIndex >= _frameUpdates.size()) RADRAY_ABORT("Invalid scene flight index");
    return _frameUpdates[flightIndex];
}

std::span<const SceneFrameUpdate> RenderSystem::GetFrameUpdatesRT(uint32_t flightIndex) const {
    if (flightIndex >= _frameUpdates.size()) RADRAY_ABORT("Invalid scene flight index");
    const auto& frame = _frameUpdates[flightIndex];
    return {frame.Scenes.data(), frame.Count};
}

void RenderSystem::SealFrameGT(uint32_t flightIndex) {
    RADRAY_PROFILE_SCOPE_N("RenderSystem::SealFrameGT");
    CheckCanModifyGT();
    auto& frame = GetFrameUpdates(flightIndex);
    if (_stopping) RADRAY_ABORT("Cannot seal after stopping scene delivery");
    if (frame.Phase != SceneFlightPhase::Writable) RADRAY_ABORT("Scene flight is still occupied");
    if (_nextUpdateSequence == std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Scene update sequence exhausted");
    for (auto& record : _scenesGT.Values()) {
        if (record.DestroySealed) continue;
        if (frame.Count == frame.Scenes.size()) frame.Scenes.emplace_back();
        auto& entry = frame.Scenes[frame.Count++];
        entry.Id = record.Writer->GetSceneId();
        entry.Create = record.CreatePending;
        entry.Destroy = record.Writer->_closing;
        record.Writer->Flush(entry.Updates, flightIndex);
        record.CreatePending = false;
        if (entry.Destroy) record.DestroySealed = true;
    }
    frame.UpdateSequence = _nextUpdateSequence++;
    frame.FrameSerial = 0;
    frame.Phase = SceneFlightPhase::Sealed;
}

void RenderSystem::PublishFrameGT(uint32_t flightIndex) {
    CheckCanModifyGT();
    auto& frame = GetFrameUpdates(flightIndex);
    if (_stopping || frame.Phase != SceneFlightPhase::Sealed || frame.UpdateSequence != _lastPublishedSequence + 1) RADRAY_ABORT("Invalid scene publication order");
    _lastPublishedSequence = frame.UpdateSequence;
    frame.Phase = SceneFlightPhase::Published;
}

uint64_t RenderSystem::GetUpdateSequence(uint32_t flightIndex) const {
    if (flightIndex >= _frameUpdates.size()) RADRAY_ABORT("Invalid scene flight index");
    return _frameUpdates[flightIndex].UpdateSequence;
}
uint64_t RenderSystem::GetFrameSerial(uint32_t flightIndex) const {
    if (flightIndex >= _frameUpdates.size()) RADRAY_ABORT("Invalid scene flight index");
    return _frameUpdates[flightIndex].FrameSerial;
}
void RenderSystem::BeginStoppingGT() noexcept {
    CheckCanModifyGT();
    _stopping = true;
}

void RenderSystem::ConsumeRenderUpdates(uint32_t flightIndex, uint64_t frameSerial) {
    RADRAY_PROFILE_SCOPE_N("RenderSystem::ConsumeRenderUpdates");
    auto& frame = GetFrameUpdates(flightIndex);
    if (frame.Phase != SceneFlightPhase::Published || frame.UpdateSequence != _lastConsumedSequence + 1 || frameSerial == 0 || frameSerial <= _lastFrameSerial) RADRAY_ABORT("Duplicate or out-of-order scene consumption");
    for (const auto& entry : GetFrameUpdatesRT(flightIndex)) {
        const auto id = entry.Id;
        if (entry.Create) {
            if (id.Index >= _scenesRT.size()) _scenesRT.resize(static_cast<size_t>(id.Index) + 1);
            auto& slot = _scenesRT[id.Index];
            if (slot.Scene || id.Generation < slot.Generation) RADRAY_ABORT("Invalid scene creation");
            slot.Generation = id.Generation;
            slot.Scene = make_unique<RenderScene>();
        }
        if (id.Index >= _scenesRT.size()) RADRAY_ABORT("Missing scene");
        auto& slot = _scenesRT[id.Index];
        if (!slot.Scene || slot.Generation != id.Generation) RADRAY_ABORT("Stale scene update");
        slot.Scene->Apply(entry.Updates);
        if (entry.Destroy) {
            slot.Scene.reset();
            if (slot.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Scene generation exhausted");
            ++slot.Generation;
        }
    }
    frame.FrameSerial = frameSerial;
    frame.Phase = SceneFlightPhase::Consumed;
    _lastConsumedSequence = frame.UpdateSequence;
    _lastFrameSerial = frameSerial;
}

Nullable<const RenderScene*> RenderSystem::GetSceneRT(SceneId id) const noexcept {
    if (!id.IsValid() || id.Index >= _scenesRT.size()) return nullptr;
    const auto& slot = _scenesRT[id.Index];
    return slot.Generation == id.Generation ? slot.Scene.get() : nullptr;
}

void RenderSystem::OnFlightCompletedGT(const FlightCompletion& completion) {
    CheckCanModifyGT();
    auto& frame = GetFrameUpdates(completion.FlightIndex);
    if (frame.Phase != SceneFlightPhase::Consumed || frame.FrameSerial != completion.FrameSerial) RADRAY_ABORT("Stale or unconsumed scene completion");
    for (size_t i = 0; i < frame.Count; ++i) {
        auto& entry = frame.Scenes[i];
        auto record = _scenesGT.TryGet({entry.Id.Index, entry.Id.Generation});
        if (record) record->Writer->_assets.ReleaseFlight(completion.FlightIndex);
        if (entry.Destroy && record) {
            if (entry.Id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Scene generation exhausted");
            _scenesGT.Destroy({entry.Id.Index, entry.Id.Generation});
        }
        entry.Updates.Clear();
    }
    frame.Count = 0;
    frame.Phase = SceneFlightPhase::Writable;
}

void RenderSystem::AbandonUnpublishedFrameGT(uint32_t flightIndex) {
    CheckCanModifyGT();
    auto& frame = GetFrameUpdates(flightIndex);
    if (!_stopping || frame.Phase == SceneFlightPhase::Published || frame.Phase == SceneFlightPhase::Consumed) RADRAY_ABORT("Terminal abandon requires stopping and completed published frames");
    for (size_t i = 0; i < frame.Count; ++i) frame.Scenes[i].Updates.Clear();
    for (auto& record : _scenesGT.Values()) record.Writer->_assets.ReleaseFlight(flightIndex);
    frame.Count = 0;
    frame.Phase = SceneFlightPhase::Writable;
}

void RenderSystem::AbandonUnpublishedFramesGT() {
    for (uint32_t i = 0; i < _frameUpdates.size(); ++i) AbandonUnpublishedFrameGT(i);
}

Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(const ShaderProgramRequest& request) {
    return _shaderCache ? _shaderCache->GetOrCreateShaderProgram(request) : nullptr;
}
Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(std::span<const byte> bytes, const shader::GpuArtifactHash& identity,
                                                                const render::ShaderProgramLayoutRecipe& recipe) {
    return _shaderCache ? _shaderCache->GetOrCreateShaderProgram(bytes, identity, recipe) : nullptr;
}
size_t RenderSystem::GetShaderProgramCacheSize() const noexcept { return _shaderCache ? _shaderCache->GetProgramCount() : 0; }
size_t RenderSystem::GetShaderArtifactCacheSize() const noexcept { return _shaderCache ? _shaderCache->GetArtifactCount() : 0; }
bool RenderSystem::InvalidateShaderSource(std::string_view sourceName) { return _shaderCache && _shaderCache->InvalidateSource(sourceName); }

}  // namespace radray
