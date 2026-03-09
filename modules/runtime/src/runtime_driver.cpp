#include <radray/logger.h>
#include <radray/runtime/runtime_driver.h>

namespace radray::runtime {
namespace {

bool StoreReason(Nullable<string*> reason, std::string_view message) noexcept {
    if (reason.HasValue()) {
        *reason.Get() = string{message};
    }
    return false;
}

class RendererFrameSink final : public IRenderFrameSink {
public:
    explicit RendererFrameSink(RendererRuntime* renderer) noexcept
        : _renderer(renderer) {}

    bool RenderFrame(const FrameSnapshot& snapshot, Nullable<string*> reason = nullptr) noexcept override {
        if (_renderer == nullptr) {
            return StoreReason(reason, "renderer frame sink is not initialized");
        }
        return _renderer->RenderFrame(snapshot, reason);
    }

    void WaitIdle() noexcept override {
        if (_renderer != nullptr) {
            _renderer->WaitIdle();
        }
    }

private:
    RendererRuntime* _renderer{nullptr};
};

}  // namespace

Nullable<unique_ptr<RuntimeDriver>> RuntimeDriver::Create(
    RuntimeDriverCreateDesc desc,
    Nullable<string*> reason) noexcept {
    auto driver = unique_ptr<RuntimeDriver>(new RuntimeDriver{});
    if (!driver->InitializeImpl(std::move(desc), reason)) {
        return nullptr;
    }
    return driver;
}

RuntimeDriver::~RuntimeDriver() noexcept {
    this->Destroy();
}

bool RuntimeDriver::InitializeImpl(RuntimeDriverCreateDesc desc, Nullable<string*> reason) noexcept {
    this->Destroy();

    _mode = desc.Mode;
    _stopRequested = false;
    _publishSequence = 0;

    if (desc.FrameSink != nullptr) {
        _frameSink = std::move(desc.FrameSink);
    } else {
        auto rendererOpt = RendererRuntime::Create(desc.Renderer, reason);
        if (!rendererOpt.HasValue()) {
            return false;
        }
        _renderer = rendererOpt.Release();
        _frameSink = make_unique<RendererFrameSink>(_renderer.get());
    }

    if (_frameSink == nullptr) {
        return StoreReason(reason, "runtime driver requires a frame sink");
    }

    if (_mode == RuntimeDriverMode::DualThread) {
        try {
            _renderThread = std::thread([this] { this->RenderThreadMain(); });
        } catch (...) {
            _frameSink.reset();
            _renderer.reset();
            return StoreReason(reason, "failed to start runtime render worker thread");
        }
    }
    return true;
}

void RuntimeDriver::Destroy() noexcept {
    this->RequestStop();
    if (_renderThread.joinable()) {
        _renderThread.join();
    }
    if (_frameSink != nullptr) {
        _frameSink->WaitIdle();
    }
    if (_renderer != nullptr) {
        _renderer->Destroy();
    }
    _frameSink.reset();
    _renderer.reset();
    _publishSequence = 0;
    _stopRequested = false;
}

RendererRuntime& RuntimeDriver::Renderer() noexcept {
    RADRAY_ASSERT(_renderer != nullptr);
    return *_renderer;
}

const RendererRuntime& RuntimeDriver::Renderer() const noexcept {
    RADRAY_ASSERT(_renderer != nullptr);
    return *_renderer;
}

FrameSnapshotQueue& RuntimeDriver::SnapshotQueue() noexcept {
    return _queue;
}

FrameSnapshotSlot* RuntimeDriver::BeginSnapshotBuild() noexcept {
    return _queue.BeginBuild();
}

FrameSnapshotBuilder RuntimeDriver::CreateSnapshotBuilder(FrameSnapshotSlot& slot) noexcept {
    return _queue.CreateBuilder(slot);
}

bool RuntimeDriver::PublishSnapshot(FrameSnapshotSlot& slot, FrameSnapshot&& snapshot) noexcept {
    if (!_queue.Publish(slot, std::move(snapshot))) {
        return false;
    }

    {
        std::scoped_lock lock{_mutex};
        ++_publishSequence;
    }
    _cv.notify_one();
    return true;
}

bool RuntimeDriver::TickSingleThread(Nullable<string*> reason) noexcept {
    if (_mode != RuntimeDriverMode::SingleThread) {
        return StoreReason(reason, "TickSingleThread is only valid in single-thread runtime driver mode");
    }
    return this->ConsumeLatestFrame(reason);
}

void RuntimeDriver::RequestStop() noexcept {
    {
        std::scoped_lock lock{_mutex};
        _stopRequested = true;
    }
    _cv.notify_all();
}

bool RuntimeDriver::ConsumeLatestFrame(Nullable<string*> reason) noexcept {
    if (_frameSink == nullptr) {
        return StoreReason(reason, "runtime driver frame sink is not initialized");
    }

    uint64_t frameId = 0;
    const FrameSnapshot* snapshot = _queue.AcquireLatestForRender(&frameId);
    if (snapshot == nullptr) {
        return StoreReason(reason, "no published frame snapshot is available");
    }

    const bool ok = _frameSink->RenderFrame(*snapshot, reason);
    _queue.ReleaseRendered(frameId);
    return ok;
}

void RuntimeDriver::RenderThreadMain() noexcept {
    uint64_t observedSequence = 0;
    while (true) {
        {
            std::unique_lock lock{_mutex};
            _cv.wait(lock, [this, observedSequence] {
                return _stopRequested || _publishSequence != observedSequence;
            });
            if (_stopRequested) {
                break;
            }
            observedSequence = _publishSequence;
        }

        string reason{};
        if (!this->ConsumeLatestFrame(&reason) && !reason.empty() &&
            reason != "no published frame snapshot is available") {
            RADRAY_ERR_LOG("RuntimeDriver render worker failed: {}", reason);
        }
    }
}

}  // namespace radray::runtime
