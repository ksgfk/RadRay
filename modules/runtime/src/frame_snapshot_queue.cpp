#include <radray/runtime/frame_snapshot_queue.h>

namespace radray::runtime {

FrameSnapshotSlot* FrameSnapshotQueue::BeginBuild() noexcept {
    std::scoped_lock lock{_mutex};
    if (_buildingSlot != nullptr) {
        return nullptr;
    }

    for (auto& slot : _slots) {
        if (slot.SlotState.load(std::memory_order_acquire) == FrameSnapshotSlot::State::Free) {
            slot.SlotState.store(FrameSnapshotSlot::State::Building, std::memory_order_release);
            _buildingSlot = &slot;
            return &slot;
        }
    }

    for (auto& slot : _slots) {
        if (slot.SlotState.load(std::memory_order_acquire) == FrameSnapshotSlot::State::Published) {
            this->DropPublishedUnlocked(slot);
            slot.SlotState.store(FrameSnapshotSlot::State::Building, std::memory_order_release);
            _buildingSlot = &slot;
            return &slot;
        }
    }

    return nullptr;
}

FrameSnapshotBuilder FrameSnapshotQueue::CreateBuilder(FrameSnapshotSlot& slot) noexcept {
    FrameSnapshotBuilder builder{};
    builder.ResetFromSlot(slot, 0, 0, 0.0);
    return builder;
}

bool FrameSnapshotQueue::Publish(FrameSnapshotSlot& slot, FrameSnapshot&& snapshot) noexcept {
    std::scoped_lock lock{_mutex};
    if (_buildingSlot != &slot || slot.SlotState.load(std::memory_order_acquire) != FrameSnapshotSlot::State::Building) {
        return false;
    }

    slot.Snapshot = std::move(snapshot);
    slot.PublishedFrameId = slot.Snapshot.Header.FrameId;
    slot.SlotState.store(FrameSnapshotSlot::State::Published, std::memory_order_release);
    _buildingSlot = nullptr;

    for (auto& other : _slots) {
        if (&other == &slot) {
            continue;
        }
        if (other.SlotState.load(std::memory_order_acquire) == FrameSnapshotSlot::State::Published &&
            other.PublishedFrameId < slot.PublishedFrameId) {
            this->DropPublishedUnlocked(other);
        }
    }
    return true;
}

const FrameSnapshot* FrameSnapshotQueue::AcquireLatestForRender(uint64_t* publishedFrameId) noexcept {
    std::scoped_lock lock{_mutex};
    if (_readingSlot != nullptr &&
        _readingSlot->SlotState.load(std::memory_order_acquire) == FrameSnapshotSlot::State::Reading) {
        if (publishedFrameId != nullptr) {
            *publishedFrameId = _readingSlot->PublishedFrameId;
        }
        return &_readingSlot->Snapshot;
    }

    FrameSnapshotSlot* latest = nullptr;
    for (auto& slot : _slots) {
        if (slot.SlotState.load(std::memory_order_acquire) != FrameSnapshotSlot::State::Published) {
            continue;
        }
        if (latest == nullptr || slot.PublishedFrameId > latest->PublishedFrameId) {
            latest = &slot;
        }
    }

    if (latest == nullptr) {
        return nullptr;
    }

    for (auto& slot : _slots) {
        if (&slot == latest) {
            continue;
        }
        if (slot.SlotState.load(std::memory_order_acquire) == FrameSnapshotSlot::State::Published) {
            this->DropPublishedUnlocked(slot);
        }
    }

    latest->SlotState.store(FrameSnapshotSlot::State::Reading, std::memory_order_release);
    _readingSlot = latest;
    if (publishedFrameId != nullptr) {
        *publishedFrameId = latest->PublishedFrameId;
    }
    return &latest->Snapshot;
}

void FrameSnapshotQueue::ReleaseRendered(uint64_t frameId) noexcept {
    std::scoped_lock lock{_mutex};
    if (_readingSlot == nullptr) {
        return;
    }
    if (_readingSlot->PublishedFrameId != frameId) {
        return;
    }
    this->DropPublishedUnlocked(*_readingSlot);
    _readingSlot = nullptr;
}

void FrameSnapshotQueue::DropPublishedUnlocked(FrameSnapshotSlot& slot) noexcept {
    slot.Snapshot = FrameSnapshot{};
    slot.PublishedFrameId = 0;
    slot.SlotState.store(FrameSnapshotSlot::State::Free, std::memory_order_release);
}

}  // namespace radray::runtime
