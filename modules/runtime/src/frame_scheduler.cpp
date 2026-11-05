#include <radray/runtime/frame_scheduler.h>

#include <limits>

#include <radray/logger.h>

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

void FrameScheduler::RunFrame(Scene& source) noexcept { RunFrameImpl(source, &source); }

void FrameScheduler::RunFrame(const Scene& source) noexcept { RunFrameImpl(source, nullptr); }

void FrameScheduler::RunFrameImpl(const Scene& source, Nullable<Scene*> mutableSource) noexcept {
    if (_nextFrameId == std::numeric_limits<uint64_t>::max()) {
        RADRAY_ABORT("FrameScheduler: frame IDs exhausted");
    }
    auto& slot = *_slots[_currentSlotIndex];
    WaitSlot(slot);
    slot.ResetCommandStorage();
    slot.ResetRetiredResources();
    slot.ResetFrameData();
    slot._frameId = _nextFrameId;
    if (mutableSource) mutableSource->SyncCachedMeshDrawCommands(_pipeline->GetMeshPassRegistrations());
#if RADRAY_IS_DEBUG
    slot._inFrameCallback = true;
#endif
    _pipeline->Extract(source, slot);
    _pipeline->Record(slot);
#if RADRAY_IS_DEBUG
    slot._inFrameCallback = false;
#endif
    slot._inFlight = true;
    ++_nextFrameId;
    _currentSlotIndex = (_currentSlotIndex + 1) % GetSlotCount();
}

void FrameScheduler::WaitIdle() noexcept {
    for (const auto& slot : _slots) WaitSlot(*slot);
    for (const auto& slot : _slots) slot->ResetCommandStorage();
    for (const auto& slot : _slots) slot->ResetRetiredResources();
    for (const auto& slot : _slots) slot->ResetFrameData();
}

void FrameScheduler::WaitSlot(FrameSlot& slot) noexcept {
    if (!slot._inFlight) return;
    _pipeline->Wait(slot);
    slot._inFlight = false;
}

}  // namespace radray
