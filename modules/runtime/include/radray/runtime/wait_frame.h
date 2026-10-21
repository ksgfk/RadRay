#pragma once

// 帧边界等待与延迟销毁: docs/architecture/frame-and-gpu.md

#include <radray/coroutine.h>
#include <radray/runtime_type.h>
#include <radray/types.h>

namespace radray {

/// GT completion notification for GPU work covered by the implementation's current flight.
/// Cancellation stops the waiting task; it is not evidence that the GPU has finished.
/// Keep submitted resource owners in GpuSystem::RetainForFrameGT, independently of this task.
/// The caller's TaskScope must end before the processor is destroyed.
class IWaitFrameProcessor {
public:
    virtual ~IWaitFrameProcessor() noexcept = default;

    /// 挂起至"调用时刻已录制的 GPU work 全部完成", 之后在主线程恢复。
    /// 【口径是保守的】实现可以多等, 但不得少等 —— 少等一次就是 use-after-free。
    virtual task<void> Wait() = 0;
};

template <>
struct RuntimeTypeTrait<IWaitFrameProcessor> {
    static constexpr RuntimeTypeId value{0x1a7c4d92, 0x63be, 0x4f05, 0x9c, 0x2e, 0x80, 0x47, 0xb6, 0x15, 0xda, 0x38};
};

}  // namespace radray
