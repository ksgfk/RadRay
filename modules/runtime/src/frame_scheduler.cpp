#include <radray/runtime/frame_scheduler.h>

#include <limits>

#include <radray/logger.h>
#include <radray/profiler.h>

namespace radray {

Nullable<unique_ptr<FrameScheduler>> FrameScheduler::Create(DeviceContext& device, unique_ptr<FramePipeline> pipeline, uint32_t slotCount) {
    if (!pipeline) {
        RADRAY_ERR_LOG("FrameScheduler: pipeline is null");
        return nullptr;
    }
    if (slotCount == 0) {
        RADRAY_ERR_LOG("FrameScheduler: slot count must be positive");
        return nullptr;
    }
    vector<unique_ptr<FrameSlot>> slots;
    slots.reserve(slotCount);
    for (uint32_t index = 0; index < slotCount; ++index) {
        auto slot = FrameSlot::Create(device, index);
        if (!slot) return nullptr;
        slots.push_back(slot.Release());
    }
    return unique_ptr<FrameScheduler>{new FrameScheduler{std::move(pipeline), std::move(slots)}};
}

FrameScheduler::FrameScheduler(unique_ptr<FramePipeline> pipeline, vector<unique_ptr<FrameSlot>> slots) noexcept
    : _pipeline(std::move(pipeline)), _slots(std::move(slots)) {}

FrameScheduler::~FrameScheduler() noexcept { WaitIdle(); }

uint32_t FrameScheduler::GetSlotCount() const noexcept { return static_cast<uint32_t>(_slots.size()); }

FrameSlot& FrameScheduler::BeginFrame() noexcept {
    auto& slot = *_slots[_currentSlotIndex];
    if (_slotBegun) return slot;
    RADRAY_PROFILE_SCOPE_N("RadRay.FrameScheduler.BeginFrame");
    if (_nextFrameId == std::numeric_limits<uint64_t>::max()) {
        RADRAY_ABORT("FrameScheduler: frame IDs exhausted");
    }
    WaitSlot(slot);
    slot.ResetCommandStorage();
    slot.ResetRetiredResources();
    slot.ResetFrameData();
    slot._frameId = _nextFrameId;
    _slotBegun = true;
#if RADRAY_IS_DEBUG
    slot._inFrameCallback = true;
#endif
    return slot;
}

Nullable<FrameSlot*> FrameScheduler::GetCurrentSlot() noexcept {
    return _slotBegun ? Nullable<FrameSlot*>{_slots[_currentSlotIndex].get()} : nullptr;
}

void FrameScheduler::RunFrame(Scene& source) noexcept { RunFrameImpl(source, &source); }

void FrameScheduler::RunFrame(const Scene& source) noexcept { RunFrameImpl(source, nullptr); }

void FrameScheduler::RunFrameImpl(const Scene& source, Nullable<Scene*> mutableSource) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FrameScheduler.RunFrame");
    RADRAY_PROFILE_PLOT("RadRay.FrameScheduler.Slot", static_cast<int64_t>(_currentSlotIndex));
    auto& slot = BeginFrame();
    const auto passes = _pipeline->GetMeshPassRegistrations();
    if (mutableSource) mutableSource->SyncCachedMeshDrawCommands(passes);
    for (const auto& pass : passes) slot.GetPassDrawList(pass.Pass).SetSortMode(pass.SortMode);
    _pipeline->Extract(source, slot);
    _pipeline->Record(source, slot);
#if RADRAY_IS_DEBUG
    slot._inFrameCallback = false;
#endif
    slot._inFlight = true;
    _slotBegun = false;
    ++_nextFrameId;
    _currentSlotIndex = (_currentSlotIndex + 1) % GetSlotCount();
}

void FrameScheduler::WaitIdle() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FrameScheduler.WaitIdle");
    for (const auto& slot : _slots) WaitSlot(*slot);
    for (const auto& slot : _slots) slot->ResetCommandStorage();
    for (const auto& slot : _slots) slot->ResetRetiredResources();
    for (const auto& slot : _slots) slot->ResetFrameData();
    _slotBegun = false;
}

void FrameScheduler::WaitSlot(FrameSlot& slot) noexcept {
    if (!slot._inFlight) return;
    RADRAY_PROFILE_SCOPE_N("RadRay.FrameScheduler.WaitSlot");
    _pipeline->Wait(slot);
    slot._inFlight = false;
}

}  // namespace radray
