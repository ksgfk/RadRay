#pragma once

#include <atomic>
#include <functional>
#include <radray/types.h>

namespace radray {

enum class FrameOperationStatus : uint8_t { Declared,
                                            Recorded,
                                            Submitted,
                                            GpuCompleted,
                                            Cancelled };

/// A receipt belongs to one frame serial, independently of reusable flight indices.
/// Stage transitions are serialized by the frame host; Status may be observed on another thread.
/// Submit means the void RHI Submit call returned; only the fence proves GPU completion.
class FrameSubmission {
public:
    explicit FrameSubmission(uint64_t serial) : _serial(serial) {}
    ~FrameSubmission() noexcept;
    FrameOperationStatus Status() const noexcept { return _status.load(std::memory_order_acquire); }
    uint64_t Serial() const noexcept { return _serial; }
    bool Record() noexcept;
    bool Submit(uint64_t serial);
    bool Complete(uint64_t serial, bool success);
    void Cancel();
    std::function<void()> OnSubmitted;
    std::function<void(bool)> OnCompleted;

private:
    uint64_t _serial;
    std::atomic<FrameOperationStatus> _status{FrameOperationStatus::Declared};
};

}  // namespace radray
