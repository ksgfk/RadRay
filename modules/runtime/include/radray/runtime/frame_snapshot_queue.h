#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include <radray/runtime/frame_snapshot.h>

namespace radray::runtime {

struct FrameSnapshotSlot {
    enum class State : uint32_t {
        Free = 0,
        Building,
        Published,
        Reading,
    };

    FrameSnapshot Snapshot{};
    uint64_t PublishedFrameId{0};
    std::atomic<State> SlotState{State::Free};
};

class FrameSnapshotQueue {
public:
    static constexpr size_t kSlotCount = 3;

    FrameSnapshotSlot* BeginBuild() noexcept;

    FrameSnapshotBuilder CreateBuilder(FrameSnapshotSlot& slot) noexcept;

    bool Publish(FrameSnapshotSlot& slot, FrameSnapshot&& snapshot) noexcept;

    const FrameSnapshot* AcquireLatestForRender(uint64_t* publishedFrameId = nullptr) noexcept;

    void ReleaseRendered(uint64_t frameId) noexcept;

private:
    void DropPublishedUnlocked(FrameSnapshotSlot& slot) noexcept;

    std::array<FrameSnapshotSlot, kSlotCount> _slots{};
    std::mutex _mutex{};
    FrameSnapshotSlot* _buildingSlot{nullptr};
    FrameSnapshotSlot* _readingSlot{nullptr};
};

}  // namespace radray::runtime
