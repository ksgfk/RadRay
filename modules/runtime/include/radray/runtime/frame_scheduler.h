#pragma once

#include <radray/runtime/frame_pipeline.h>

namespace radray {

// See docs/architecture/runtime.md for the frame loop and ownership contract.
class FrameScheduler final {
public:
    ~FrameScheduler() noexcept;

    FrameScheduler(const FrameScheduler&) = delete;
    FrameScheduler& operator=(const FrameScheduler&) = delete;
    FrameScheduler(FrameScheduler&&) = delete;
    FrameScheduler& operator=(FrameScheduler&&) = delete;

    // Consumes pipeline on entry. Slot count must be positive and remains fixed.
    // DeviceContext must outlive this scheduler.
    static Nullable<unique_ptr<FrameScheduler>> Create(DeviceContext& device, unique_ptr<FramePipeline> pipeline, uint32_t slotCount = 2);

    uint32_t GetSlotCount() const noexcept;

    // Serial calls only. Return means Record completed, not GPU-complete. Frame errors are fatal.
    // Source is borrowed through Extract/Record; referenced resources survive until allocator reset.
    // The mutable overload synchronizes dirty mesh commands before Extract.
    void RunFrame(Scene& source) noexcept;
    void RunFrame(const Scene& source) noexcept;

    // Waits for all accepted frames, then clears recordings, CPU data and arena objects.
    // Does not change the next slot index; repeated calls do not repeat completed waits.
    void WaitIdle() noexcept;

private:
    FrameScheduler(unique_ptr<FramePipeline> pipeline, vector<unique_ptr<FrameSlot>> slots) noexcept;

    void WaitSlot(FrameSlot& slot) noexcept;
    void RunFrameImpl(const Scene& source, Nullable<Scene*> mutableSource) noexcept;

    // The destructor drains all slots; release their recordings before pipeline resources.
    unique_ptr<FramePipeline> _pipeline;
    vector<unique_ptr<FrameSlot>> _slots;
    uint64_t _nextFrameId{0};
    uint32_t _currentSlotIndex{0};
};

}  // namespace radray
