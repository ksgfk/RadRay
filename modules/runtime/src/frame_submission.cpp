#include <radray/runtime/frame_submission.h>

namespace radray {
FrameSubmission::~FrameSubmission() noexcept { Cancel(); }
bool FrameSubmission::Record() noexcept {
    auto expected = FrameOperationStatus::Declared;
    return _status.compare_exchange_strong(expected, FrameOperationStatus::Recorded, std::memory_order_release);
}
bool FrameSubmission::Submit(uint64_t serial) {
    if (serial != _serial || Status() != FrameOperationStatus::Recorded) return false;
    if (OnSubmitted) OnSubmitted();
    _status.store(FrameOperationStatus::Submitted, std::memory_order_release);
    OnSubmitted = {};
    return true;
}
bool FrameSubmission::Complete(uint64_t serial, bool success) {
    if (serial != _serial || Status() != FrameOperationStatus::Submitted) return false;
    if (OnCompleted) OnCompleted(success);
    _status.store(success ? FrameOperationStatus::GpuCompleted : FrameOperationStatus::Cancelled, std::memory_order_release);
    OnSubmitted = {};
    OnCompleted = {};
    return true;
}
void FrameSubmission::Cancel() {
    const auto state = Status();
    if (state == FrameOperationStatus::GpuCompleted || state == FrameOperationStatus::Cancelled || state == FrameOperationStatus::Submitted) return;
    if (OnCompleted) OnCompleted(false);
    _status.store(FrameOperationStatus::Cancelled, std::memory_order_release);
    OnSubmitted = {};
    OnCompleted = {};
}
}  // namespace radray
