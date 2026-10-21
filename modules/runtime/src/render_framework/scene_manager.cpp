#include <radray/runtime/render_framework/scene_manager.h>

#include <limits>
#include <algorithm>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/frame_timeline.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/renderer.h>

namespace radray {
SceneManager::SceneManager(uint32_t flightCount) : _frameUpdates(flightCount) {
    if (flightCount == 0) RADRAY_ABORT("Scene delivery requires at least one flight");
}
SceneManager::~SceneManager() noexcept { OnShutdown(); }

void SceneManager::CheckCanModifyGT() const {
    if (std::this_thread::get_id() != _ownerThread || _collecting) RADRAY_ABORT("Scene mutation requires GT outside collection");
}
void SceneManager::SetCollecting(bool collecting) {
    if (collecting) CheckCanModifyGT();
    _collecting = collecting;
}

void SceneManager::OnShutdown() noexcept {
    for (const auto& record : _scenesGT.Values()) {
        if (record.Writer->_claimed) RADRAY_ABORT("Disconnect all Worlds before shutting down SceneManager");
    }
    BeginStoppingGT();
    AbandonUnpublishedFramesGT();
    PumpAssetExitsGT();
    _renderer = nullptr;
    _scenesRT.clear();
    _scenesGT.Clear();
    _frameUpdates.clear();
}

SceneId SceneManager::CreateSceneGT() {
    CheckCanModifyGT();
    if (_frameUpdates.empty() || _stopping) RADRAY_ABORT("SceneManager is stopping or shut down");
    const auto handle = _scenesGT.Emplace();
    const SceneId id{handle.Index, handle.Generation};
    _scenesGT.Get(handle).Writer = make_unique<SceneWriter>(id, static_cast<uint32_t>(_frameUpdates.size()));
    return id;
}

Nullable<SceneWriter*> SceneManager::GetSceneWriterGT(SceneId id) noexcept {
    auto record = _scenesGT.TryGet({id.Index, id.Generation});
    if (!record || record->Writer->_closing || record->Writer->_claimed) return nullptr;
    return record->Writer.get();
}

SceneWriter& SceneManager::ClaimSceneWriterGT(SceneId id) {
    auto writer = GetSceneWriterGT(id);
    if (!writer) RADRAY_ABORT("Scene already claimed or unavailable");
    writer->_claimed = true;
    return *writer.Get();
}

void SceneManager::ReleaseSceneWriterGT(SceneId id) {
    auto record = _scenesGT.TryGet({id.Index, id.Generation});
    if (!record || !record->Writer->_claimed) RADRAY_ABORT("Scene is not claimed");
    record->Writer->_claimed = false;
}

void SceneManager::DestroySceneGT(SceneId id) {
    CheckCanModifyGT();
    auto record = _scenesGT.TryGet({id.Index, id.Generation});
    if (!record || record->Writer->_closing || record->Writer->_claimed) RADRAY_ABORT("Invalid scene destruction");
    record->Writer->_closing = true;
}

SceneManager::FrameUpdates& SceneManager::GetFrameUpdates(uint32_t flightIndex) {
    if (flightIndex >= _frameUpdates.size()) RADRAY_ABORT("Invalid scene flight index");
    return _frameUpdates[flightIndex];
}

std::span<const SceneFrameUpdate> SceneManager::GetFrameUpdatesRT(uint32_t flightIndex) const {
    if (flightIndex >= _frameUpdates.size()) RADRAY_ABORT("Invalid scene flight index");
    const auto& frame = _frameUpdates[flightIndex];
    return {frame.Scenes.data(), frame.Count};
}

std::span<const SceneViewRequest> SceneManager::GetFrameViewsRT(uint32_t flightIndex) const {
    if (flightIndex >= _frameUpdates.size()) RADRAY_ABORT("Invalid view flight index");
    return _frameUpdates[flightIndex].Views;
}

void SceneManager::SealFrameGT(uint32_t flightIndex) {
    RADRAY_PROFILE_SCOPE_N("SceneManager::SealFrameGT");
    CheckCanModifyGT();
    PumpAssetExitsGT();
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
        record.Writer->Flush(entry.Updates, flightIndex, bool(_renderer));
        record.CreatePending = false;
        if (entry.Destroy) record.DestroySealed = true;
    }
    frame.UpdateSequence = _nextUpdateSequence++;
    frame.FrameSerial = 0;
    frame.Phase = SceneFlightPhase::Sealed;
}

void SceneManager::PublishFrameGT(uint32_t flightIndex) {
    CheckCanModifyGT();
    auto& frame = GetFrameUpdates(flightIndex);
    if (_stopping || frame.Phase != SceneFlightPhase::Sealed || frame.UpdateSequence != _lastPublishedSequence + 1) RADRAY_ABORT("Invalid scene publication order");
    _lastPublishedSequence = frame.UpdateSequence;
    frame.Phase = SceneFlightPhase::Published;
}

uint64_t SceneManager::GetUpdateSequence(uint32_t flightIndex) const {
    if (flightIndex >= _frameUpdates.size()) RADRAY_ABORT("Invalid scene flight index");
    return _frameUpdates[flightIndex].UpdateSequence;
}
uint64_t SceneManager::GetFrameSerial(uint32_t flightIndex) const {
    if (flightIndex >= _frameUpdates.size()) RADRAY_ABORT("Invalid scene flight index");
    return _frameUpdates[flightIndex].FrameSerial;
}
void SceneManager::BeginStoppingGT() noexcept {
    CheckCanModifyGT();
    _stopping = true;
}

void SceneManager::ConsumeRenderUpdates(uint32_t flightIndex, uint64_t frameSerial) {
    RADRAY_PROFILE_SCOPE_N("SceneManager::ConsumeRenderUpdates");
    if (_renderer) _renderer->ProcessCompletions();
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
        slot.Scene->Apply(entry.Updates, _renderer ? &_applyChanges : Nullable<SceneApplyChanges*>{nullptr});
        if (_renderer) _renderer->GetScenes().Apply(GetRendererSceneKey(id), _applyChanges, entry.Updates.AssetExits, entry.Destroy);
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

Nullable<const RenderScene*> SceneManager::GetSceneRT(SceneId id) const noexcept {
    if (!id.IsValid() || id.Index >= _scenesRT.size()) return nullptr;
    const auto& slot = _scenesRT[id.Index];
    return slot.Generation == id.Generation ? slot.Scene.get() : nullptr;
}

void SceneManager::AttachRenderer(Renderer& renderer) {
    CheckCanModifyGT();
    if (_renderer || _lastPublishedSequence || _nextUpdateSequence != 1) RADRAY_ABORT("Attach Renderer before scene delivery starts");
    _renderer = &renderer;
}
RendererSceneKey SceneManager::GetRendererSceneKey(SceneId id) const noexcept { return {_delivery, id}; }
void SceneManager::PumpAssetExitsGT() {
    CheckCanModifyGT();
    if (_renderer) {
        for (const auto& exit : _renderer->GetScenes().TakeConfirmations(_delivery)) {
            auto record = _scenesGT.TryGet({exit.Scene.Index, exit.Scene.Generation});
            if (record) record->Writer->_assets.ConfirmExit(exit.Token);
        }
    }
    vector<SceneId> complete;
    for (const auto& record : _scenesGT.Values())
        if (record.DestroyCompleted && !record.Writer->_assets.HasPendingExits()) complete.push_back(record.Writer->GetSceneId());
    for (const auto id : complete) _scenesGT.Destroy({id.Index, id.Generation});
}

void SceneManager::OnFlightCompletedGT(const FlightCompletion& completion) {
    CheckCanModifyGT();
    auto& frame = GetFrameUpdates(completion.FlightIndex);
    if (frame.Phase != SceneFlightPhase::Consumed || frame.FrameSerial != completion.FrameSerial) RADRAY_ABORT("Stale or unconsumed scene completion");
    frame.Views.clear();
    for (size_t i = 0; i < frame.Count; ++i) {
        auto& entry = frame.Scenes[i];
        auto record = _scenesGT.TryGet({entry.Id.Index, entry.Id.Generation});
        if (record) record->Writer->_assets.ReleaseFlight(completion.FlightIndex);
        if (entry.Destroy && record) {
            if (entry.Id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Scene generation exhausted");
            record->DestroyCompleted = true;
        }
        entry.Updates.Clear();
    }
    frame.Count = 0;
    frame.Phase = SceneFlightPhase::Writable;
    PumpAssetExitsGT();
}

void SceneManager::AbandonUnpublishedFrameGT(uint32_t flightIndex) {
    CheckCanModifyGT();
    auto& frame = GetFrameUpdates(flightIndex);
    if (!_stopping || frame.Phase == SceneFlightPhase::Published || frame.Phase == SceneFlightPhase::Consumed) RADRAY_ABORT("Terminal abandon requires stopping and completed published frames");
    for (size_t i = 0; i < frame.Count; ++i) frame.Scenes[i].Updates.Clear();
    for (auto& record : _scenesGT.Values()) record.Writer->_assets.ReleaseFlight(flightIndex);
    frame.Count = 0;
    frame.Views.clear();
    frame.Phase = SceneFlightPhase::Writable;
}

void SceneManager::AbandonUnpublishedFramesGT() {
    for (uint32_t i = 0; i < _frameUpdates.size(); ++i) AbandonUnpublishedFrameGT(i);
}

}  // namespace radray
