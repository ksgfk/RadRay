#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

#include <radray/nullable.h>

#include <radray/runtime/frame_snapshot_builder.h>
#include <radray/runtime/frame_snapshot_queue.h>
#include <radray/runtime/renderer_runtime.h>

namespace radray::runtime {

enum class RuntimeDriverMode : uint32_t {
    SingleThread = 0,
    DualThread,
};

class IRenderFrameSink {
public:
    virtual ~IRenderFrameSink() noexcept = default;

    virtual bool RenderFrame(const FrameSnapshot& snapshot, Nullable<string*> reason = nullptr) noexcept = 0;

    virtual void WaitIdle() noexcept = 0;
};

struct RuntimeDriverCreateDesc {
    RendererRuntimeCreateDesc Renderer{};
    RuntimeDriverMode Mode{RuntimeDriverMode::DualThread};
    unique_ptr<IRenderFrameSink> FrameSink{};
};

class RuntimeDriver {
public:
    static Nullable<unique_ptr<RuntimeDriver>> Create(
        RuntimeDriverCreateDesc desc,
        Nullable<string*> reason = nullptr) noexcept;

    ~RuntimeDriver() noexcept;

    void Destroy() noexcept;

    RendererRuntime& Renderer() noexcept;

    const RendererRuntime& Renderer() const noexcept;

    FrameSnapshotQueue& SnapshotQueue() noexcept;

    FrameSnapshotSlot* BeginSnapshotBuild() noexcept;

    FrameSnapshotBuilder CreateSnapshotBuilder(FrameSnapshotSlot& slot) noexcept;

    bool PublishSnapshot(FrameSnapshotSlot& slot, FrameSnapshot&& snapshot) noexcept;

    bool TickSingleThread(Nullable<string*> reason = nullptr) noexcept;

    void RequestStop() noexcept;

private:
    RuntimeDriver() noexcept = default;
    bool InitializeImpl(RuntimeDriverCreateDesc desc, Nullable<string*> reason = nullptr) noexcept;
    bool ConsumeLatestFrame(Nullable<string*> reason = nullptr) noexcept;
    void RenderThreadMain() noexcept;

    unique_ptr<RendererRuntime> _renderer{};
    unique_ptr<IRenderFrameSink> _frameSink{};
    FrameSnapshotQueue _queue{};
    RuntimeDriverMode _mode{RuntimeDriverMode::DualThread};
    std::mutex _mutex{};
    std::condition_variable _cv{};
    std::thread _renderThread{};
    bool _stopRequested{false};
    uint64_t _publishSequence{0};
};

}  // namespace radray::runtime
