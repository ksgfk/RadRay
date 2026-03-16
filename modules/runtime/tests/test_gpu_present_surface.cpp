#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>
#include <radray/window/native_window.h>

using namespace radray;
using namespace radray::render;

namespace {

constexpr uint32_t kDefaultWidth = 640;
constexpr uint32_t kDefaultHeight = 360;
constexpr uint32_t kResizedWidth = 800;
constexpr uint32_t kResizedHeight = 450;
constexpr uint32_t kShortFrameCount = 16;
constexpr uint32_t kLongFrameCount = 32;

struct ValidationLogMonitor {
    void Record(LogLevel level, std::string_view message) {
        if (level < LogLevel::Err) {
            return;
        }

        std::lock_guard lock(Mutex);
        ErrorMessages.emplace_back(message);
    }

    void ExpectNoErrors() {
        std::lock_guard lock(Mutex);
        if (ErrorMessages.empty()) {
            return;
        }

        std::ostringstream oss;
        for (const auto& message : ErrorMessages) {
            oss << message << '\n';
        }
        ADD_FAILURE() << "Backend validation logged Err/Critical messages:\n" << oss.str();
        ErrorMessages.clear();
    }

    std::mutex Mutex;
    std::vector<std::string> ErrorMessages;
};

// ---------------------------------------------------------------------------
// Helper: create a platform native window
// ---------------------------------------------------------------------------
Nullable<unique_ptr<NativeWindow>> CreateTestWindow(uint32_t width, uint32_t height) noexcept {
#if defined(_WIN32)
    Win32WindowCreateDescriptor desc{};
    desc.Title = "GpuPresentSurfaceTest";
    desc.Width = static_cast<int32_t>(width);
    desc.Height = static_cast<int32_t>(height);
    desc.X = 120;
    desc.Y = 120;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return CreateNativeWindow(desc);
#elif defined(__APPLE__)
    CocoaWindowCreateDescriptor desc{};
    desc.Title = "GpuPresentSurfaceTest";
    desc.Width = static_cast<int32_t>(width);
    desc.Height = static_cast<int32_t>(height);
    desc.X = 120;
    desc.Y = 120;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return CreateNativeWindow(desc);
#else
    RADRAY_UNUSED(width);
    RADRAY_UNUSED(height);
    return nullptr;
#endif
}

void ValidationLogCallback(LogLevel level, std::string_view message, void* userData) noexcept {
    Log({}, level, message);
    if (auto* monitor = static_cast<ValidationLogMonitor*>(userData)) {
        monitor->Record(level, message);
    }
}

// ---------------------------------------------------------------------------
// Helper: create a GpuRuntime for a given backend
// ---------------------------------------------------------------------------
Nullable<unique_ptr<GpuRuntime>> CreateRuntime(RenderBackend backend, ValidationLogMonitor& validationMonitor) noexcept {
    GpuRuntimeDescriptor desc{};
    desc.Backend = backend;
    desc.EnableDebugValidation = true;
    desc.LogCallback = ValidationLogCallback;
    desc.LogUserData = &validationMonitor;
    return GpuRuntime::Create(desc);
}

// ---------------------------------------------------------------------------
// Helper: create a GpuPresentSurface from runtime + window
// ---------------------------------------------------------------------------
Nullable<unique_ptr<GpuPresentSurface>> CreateSurface(
    GpuRuntime& runtime,
    NativeWindow& window,
    PresentMode presentMode) noexcept {
    auto nativeHandler = window.GetNativeHandler();
    auto size = window.GetSize();
    GpuPresentSurfaceDescriptor desc{};
    desc.NativeWindowHandle = nativeHandler.Handle;
    desc.Width = static_cast<uint32_t>(size.X);
    desc.Height = static_cast<uint32_t>(size.Y);
    desc.PresentMode = presentMode;
    return runtime.CreatePresentSurface(desc);
}

// ---------------------------------------------------------------------------
// Core present loop
//   blocking = true  → Acquire  (waits for available frame)
//   blocking = false → TryAcquire (skips when no frame available)
// ---------------------------------------------------------------------------
void RunPresentLoop(
    GpuRuntime& runtime,
    GpuPresentSurface& surface,
    NativeWindow& window,
    uint32_t frameCount,
    bool blocking) {
    std::vector<GpuTask> pendingTasks;

    for (uint32_t i = 0; i < frameCount; ++i) {
        window.DispatchEvents();
        if (window.ShouldClose()) {
            break;
        }

        auto ctxOpt = runtime.BeginSubmit();
        if (!ctxOpt.HasValue()) {
            FAIL() << "BeginSubmit returned null at frame " << i;
            return;
        }
        auto ctx = ctxOpt.Release();

        std::optional<GpuSurfaceAcquireResult> acquired;
        if (blocking) {
            auto result = ctx->Acquire(surface);
            if (result.Status == GpuSurfaceStatus::Lost) {
                FAIL() << "Blocking Acquire returned Lost at frame " << i;
                return;
            }
            acquired = result;
        } else {
            // TODO: TryAcquire not yet implemented
            // acquired = ctx->TryAcquire(surface);
        }

        if (acquired.has_value()) {
            auto& result = acquired.value();
            EXPECT_TRUE(result.Command.IsValid());
            EXPECT_NE(result.Command.NativeHandle, nullptr);

            // RHI Acquire always returns Success; detect size mismatch ourselves
            auto winSize = window.GetSize();
            uint32_t ww = static_cast<uint32_t>(winSize.X);
            uint32_t wh = static_cast<uint32_t>(winSize.Y);
            if (surface.GetWidth() != ww || surface.GetHeight() != wh) {
                surface.Reconfigure(ww, wh);
                continue;  // drop the stale ctx, re-acquire next iteration
            }

            switch (result.Status) {
                case GpuSurfaceStatus::Success:
                    break;
                case GpuSurfaceStatus::Suboptimal:
                case GpuSurfaceStatus::OutOfDate: {
                    auto size = window.GetSize();
                    surface.Reconfigure(
                        static_cast<uint32_t>(size.X),
                        static_cast<uint32_t>(size.Y));
                    continue;
                }
                case GpuSurfaceStatus::Lost:
                    FAIL() << "Surface lost at frame " << i;
                    return;
            }

            ctx->EndCommand(result.Command);
            if (!ctx->Present(surface, result.BackBuffer)) {
                FAIL() << "Present rejected at frame " << i;
                return;
            }
        }
        // Non-blocking: nullopt simply means no frame available, skip present

        GpuTask task = runtime.Submit(std::move(ctx));
        if (blocking) {
            task.Wait();
        } else {
            pendingTasks.push_back(std::move(task));
        }
    }

    // Wait for all pending non-blocking tasks
    for (auto& task : pendingTasks) {
        if (task.IsValid() && !task.IsCompleted()) {
            task.Wait();
        }
    }
}

// ---------------------------------------------------------------------------
// RunRenderLoop — GPU-only rendering, no event dispatch.
// Safe to call from a worker thread (Cocoa requires DispatchEvents on main).
// ---------------------------------------------------------------------------
void RunRenderLoop(
    GpuRuntime& runtime,
    GpuPresentSurface& surface,
    uint32_t frameCount) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        auto ctxOpt = runtime.BeginSubmit();
        if (!ctxOpt.HasValue()) {
            FAIL() << "BeginSubmit returned null at frame " << i;
            return;
        }
        auto ctx = ctxOpt.Release();

        auto result = ctx->Acquire(surface);
        if (result.Status == GpuSurfaceStatus::Lost) {
            FAIL() << "Acquire returned Lost at frame " << i;
            return;
        }

        EXPECT_TRUE(result.Command.IsValid());
        EXPECT_NE(result.Command.NativeHandle, nullptr);
        ctx->EndCommand(result.Command);
        ASSERT_TRUE(ctx->Present(surface, result.BackBuffer)) << "Present rejected at frame " << i;
        GpuTask task = runtime.Submit(std::move(ctx));
        task.Wait();
    }
}

// ---------------------------------------------------------------------------
// RunPresentLoopDroppingTasks
//   提交后立即丢弃 GpuTask，验证提交资源由 runtime 内部保活。
// ---------------------------------------------------------------------------
void RunPresentLoopDroppingTasks(
    GpuRuntime& runtime,
    GpuPresentSurface& surface,
    NativeWindow& window,
    uint32_t frameCount,
    bool processRuntimeEachFrame = false) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        window.DispatchEvents();
        if (window.ShouldClose()) {
            break;
        }

        auto ctxOpt = runtime.BeginSubmit();
        if (!ctxOpt.HasValue()) {
            FAIL() << "BeginSubmit returned null at frame " << i;
            return;
        }
        auto ctx = ctxOpt.Release();

        auto result = ctx->Acquire(surface);
        if (result.Status == GpuSurfaceStatus::Lost) {
            FAIL() << "Acquire returned Lost at frame " << i;
            return;
        }

        EXPECT_TRUE(result.Command.IsValid());
        EXPECT_NE(result.Command.NativeHandle, nullptr);
        ctx->EndCommand(result.Command);
        ASSERT_TRUE(ctx->Present(surface, result.BackBuffer)) << "Present rejected at frame " << i;
        runtime.Submit(std::move(ctx));
        if (processRuntimeEachFrame) {
            runtime.ProcessSubmits();
        }
    }
}

// ---------------------------------------------------------------------------
// Parameterized test fixture
// ---------------------------------------------------------------------------
enum class TestBackend { D3D12, Vulkan };

class GpuPresentSurfaceTest : public ::testing::TestWithParam<TestBackend> {
protected:
    void SetUp() override {
        auto windowOpt = CreateTestWindow(kDefaultWidth, kDefaultHeight);
        if (!windowOpt.HasValue()) {
            GTEST_SKIP() << "Cannot create native window for this platform.";
        }
        window_ = windowOpt.Release();

        RenderBackend backend = (GetParam() == TestBackend::D3D12)
                                    ? RenderBackend::D3D12
                                    : RenderBackend::Vulkan;

        auto runtimeOpt = CreateRuntime(backend, validationMonitor_);
        if (!runtimeOpt.HasValue()) {
            GTEST_SKIP() << "Cannot create GpuRuntime for backend.";
        }
        runtime_ = runtimeOpt.Release();

        auto surfaceOpt = CreateSurface(*runtime_, *window_, PresentMode::FIFO);
        if (!surfaceOpt.HasValue()) {
            GTEST_SKIP() << "Cannot create GpuPresentSurface.";
        }
        surface_ = surfaceOpt.Release();
    }

    void TearDown() override {
        if (surface_) {
            surface_->Destroy();
            surface_.reset();
        }
        if (runtime_) {
            runtime_->Destroy();
            runtime_.reset();
        }
        if (window_) {
            window_->Destroy();
            window_.reset();
        }
        validationMonitor_.ExpectNoErrors();
    }

    unique_ptr<NativeWindow> window_;
    unique_ptr<GpuRuntime> runtime_;
    unique_ptr<GpuPresentSurface> surface_;
    ValidationLogMonitor validationMonitor_;
};

INSTANTIATE_TEST_SUITE_P(
    Backend,
    GpuPresentSurfaceTest,
    ::testing::Values(TestBackend::D3D12, TestBackend::Vulkan));

// ---------------------------------------------------------------------------
// BasicPresentLoop: blocking Acquire, run N frames
// ---------------------------------------------------------------------------
TEST_P(GpuPresentSurfaceTest, BasicPresentLoop) {
    RunPresentLoop(*runtime_, *surface_, *window_, kLongFrameCount, true);
}

// ---------------------------------------------------------------------------
// NonBlockingPresentLoop: TryAcquire, run N frames
// ---------------------------------------------------------------------------
// TODO: 以后实现，需要修改 render 层接口
// TEST_P(GpuPresentSurfaceTest, NonBlockingPresentLoop) {
//     RunPresentLoop(*runtime_, *surface_, *window_, kLongFrameCount, false);
// }

// ---------------------------------------------------------------------------
// SwitchPresentMode: FIFO → Mailbox mid-flight
// ---------------------------------------------------------------------------
TEST_P(GpuPresentSurfaceTest, SwitchPresentMode) {
    // Phase 1: render with FIFO (set up in SetUp)
    RunPresentLoop(*runtime_, *surface_, *window_, kShortFrameCount, true);

    // Phase 2: reconfigure to Mailbox
    auto size = window_->GetSize();
    bool ok = surface_->Reconfigure(
        static_cast<uint32_t>(size.X),
        static_cast<uint32_t>(size.Y),
        std::nullopt,
        PresentMode::Mailbox);
    ASSERT_TRUE(ok) << "Reconfigure to Mailbox failed";

    EXPECT_EQ(surface_->GetPresentMode(), PresentMode::Mailbox);

    // Phase 3: continue rendering with Mailbox
    RunPresentLoop(*runtime_, *surface_, *window_, kShortFrameCount, true);
}

// ---------------------------------------------------------------------------
// SwitchRenderThread: main thread → worker thread → main thread
// ---------------------------------------------------------------------------
TEST_P(GpuPresentSurfaceTest, SwitchRenderThread) {
    // Phase 1: render on main thread
    RunPresentLoop(*runtime_, *surface_, *window_, kShortFrameCount, true);

    // Phase 2: worker thread renders, main thread dispatches events
    std::atomic<bool> workerDone{false};
    std::thread renderThread([&] {
        RunRenderLoop(*runtime_, *surface_, kLongFrameCount);
        workerDone.store(true, std::memory_order_release);
    });
    while (!workerDone.load(std::memory_order_acquire)) {
        window_->DispatchEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    renderThread.join();

    // Phase 3: back to main thread
    RunPresentLoop(*runtime_, *surface_, *window_, kShortFrameCount, true);
}

// ---------------------------------------------------------------------------
// ReconfigureOnSuboptimal: simulate window resize, handle Suboptimal/OutOfDate
// ---------------------------------------------------------------------------
TEST_P(GpuPresentSurfaceTest, ReconfigureOnSuboptimal) {
    // Phase 1: render at initial size
    RunPresentLoop(*runtime_, *surface_, *window_, kShortFrameCount, true);

    // Phase 2: resize window
    window_->SetSize(
        static_cast<int>(kResizedWidth),
        static_cast<int>(kResizedHeight));
    window_->DispatchEvents();

    // Phase 3: continue rendering — RunPresentLoop handles Suboptimal/OutOfDate internally
    RunPresentLoop(*runtime_, *surface_, *window_, kLongFrameCount, true);

    // Verify surface dimensions match resized window
    auto size = window_->GetSize();
    EXPECT_EQ(surface_->GetWidth(), static_cast<uint32_t>(size.X));
    EXPECT_EQ(surface_->GetHeight(), static_cast<uint32_t>(size.Y));
}

// ---------------------------------------------------------------------------
// RuntimeOwnsSubmittedContexts: drop GpuTask immediately and keep rendering.
// ---------------------------------------------------------------------------
TEST_P(GpuPresentSurfaceTest, RuntimeOwnsSubmittedContexts) {
    RunPresentLoopDroppingTasks(*runtime_, *surface_, *window_, kShortFrameCount);

    auto size = window_->GetSize();
    bool ok = surface_->Reconfigure(
        static_cast<uint32_t>(size.X),
        static_cast<uint32_t>(size.Y));
    ASSERT_TRUE(ok) << "Reconfigure after dropping GpuTask failed";

    RunPresentLoop(*runtime_, *surface_, *window_, kShortFrameCount, true);
}

// ---------------------------------------------------------------------------
// ProcessSubmits: exposed non-blocking maintenance hook for submitted ctx.
// ---------------------------------------------------------------------------
TEST_P(GpuPresentSurfaceTest, ProcessSubmits) {
    RunPresentLoopDroppingTasks(*runtime_, *surface_, *window_, kShortFrameCount, true);

    runtime_->ProcessSubmits();

    auto size = window_->GetSize();
    bool ok = surface_->Reconfigure(
        static_cast<uint32_t>(size.X),
        static_cast<uint32_t>(size.Y));
    ASSERT_TRUE(ok) << "Reconfigure after ProcessSubmits failed";

    RunPresentLoop(*runtime_, *surface_, *window_, kShortFrameCount, true);
}

TEST_P(GpuPresentSurfaceTest, PureCommandSubmit) {
    auto ctxOpt = runtime_->BeginSubmit();
    ASSERT_TRUE(ctxOpt.HasValue()) << "BeginSubmit returned null";
    auto ctx = ctxOpt.Release();

    auto cmd = ctx->BeginCommand(QueueType::Direct);
    ASSERT_TRUE(cmd.IsValid()) << "BeginCommand returned invalid handle";
    ASSERT_NE(cmd.NativeHandle, nullptr);

    ctx->EndCommand(cmd);

    GpuTask task = runtime_->Submit(std::move(ctx));
    ASSERT_TRUE(task.IsValid()) << "Pure command Submit returned invalid task";
    task.Wait();
}

TEST_P(GpuPresentSurfaceTest, SubmitRejectsUnclosedCommand) {
    auto ctxOpt = runtime_->BeginSubmit();
    ASSERT_TRUE(ctxOpt.HasValue()) << "BeginSubmit returned null";
    auto ctx = ctxOpt.Release();

    auto cmd = ctx->BeginCommand(QueueType::Direct);
    ASSERT_TRUE(cmd.IsValid()) << "BeginCommand returned invalid handle";

    GpuTask task = runtime_->Submit(std::move(ctx));
    EXPECT_FALSE(task.IsValid());
}

TEST_P(GpuPresentSurfaceTest, BeginCommandRejectsQueueMismatchAfterAcquire) {
    auto ctxOpt = runtime_->BeginSubmit();
    ASSERT_TRUE(ctxOpt.HasValue()) << "BeginSubmit returned null";
    auto ctx = ctxOpt.Release();

    auto acquired = ctx->Acquire(*surface_);
    ASSERT_NE(acquired.Status, GpuSurfaceStatus::Lost) << "Acquire returned Lost";
    ASSERT_TRUE(acquired.Command.IsValid()) << "Acquire command handle is invalid";
    ctx->EndCommand(acquired.Command);

    auto badCmd = ctx->BeginCommand(QueueType::Compute);
    EXPECT_FALSE(badCmd.IsValid());

    ASSERT_TRUE(ctx->Present(*surface_, acquired.BackBuffer));
    GpuTask task = runtime_->Submit(std::move(ctx));
    ASSERT_TRUE(task.IsValid()) << "Submit after queue mismatch validation failed";
    task.Wait();
}

TEST_P(GpuPresentSurfaceTest, BeginCommandRejectsWhileAnotherCommandIsOpen) {
    auto ctxOpt = runtime_->BeginSubmit();
    ASSERT_TRUE(ctxOpt.HasValue()) << "BeginSubmit returned null";
    auto ctx = ctxOpt.Release();

    auto first = ctx->BeginCommand(QueueType::Direct);
    ASSERT_TRUE(first.IsValid()) << "BeginCommand returned invalid handle";

    auto second = ctx->BeginCommand(QueueType::Direct);
    EXPECT_FALSE(second.IsValid());

    ctx->EndCommand(first);
    GpuTask task = runtime_->Submit(std::move(ctx));
    ASSERT_TRUE(task.IsValid()) << "Submit after open-command validation failed";
    task.Wait();
}

TEST_P(GpuPresentSurfaceTest, SecondAcquireRejected) {
    auto ctxOpt = runtime_->BeginSubmit();
    ASSERT_TRUE(ctxOpt.HasValue()) << "BeginSubmit returned null";
    auto ctx = ctxOpt.Release();

    auto first = ctx->Acquire(*surface_);
    ASSERT_NE(first.Status, GpuSurfaceStatus::Lost) << "First Acquire returned Lost";
    ASSERT_TRUE(first.Command.IsValid()) << "Acquire command handle is invalid";

    auto second = ctx->Acquire(*surface_);
    EXPECT_EQ(second.Status, GpuSurfaceStatus::Lost);
    EXPECT_FALSE(second.BackBuffer.IsValid());
    EXPECT_FALSE(second.Command.IsValid());

    ctx->EndCommand(first.Command);
    ASSERT_TRUE(ctx->Present(*surface_, first.BackBuffer));
    GpuTask task = runtime_->Submit(std::move(ctx));
    ASSERT_TRUE(task.IsValid()) << "Submit after second Acquire validation failed";
    task.Wait();
}

TEST_P(GpuPresentSurfaceTest, SecondPresentRejected) {
    auto ctxOpt = runtime_->BeginSubmit();
    ASSERT_TRUE(ctxOpt.HasValue()) << "BeginSubmit returned null";
    auto ctx = ctxOpt.Release();

    auto acquired = ctx->Acquire(*surface_);
    ASSERT_NE(acquired.Status, GpuSurfaceStatus::Lost) << "Acquire returned Lost";
    ASSERT_TRUE(acquired.Command.IsValid()) << "Acquire command handle is invalid";
    ctx->EndCommand(acquired.Command);

    EXPECT_TRUE(ctx->Present(*surface_, acquired.BackBuffer));
    EXPECT_FALSE(ctx->Present(*surface_, acquired.BackBuffer));

    GpuTask task = runtime_->Submit(std::move(ctx));
    ASSERT_TRUE(task.IsValid()) << "Submit after second Present validation failed";
    task.Wait();
}

TEST_P(GpuPresentSurfaceTest, PresentRejectsMismatchedBackBuffer) {
    auto ctxOpt = runtime_->BeginSubmit();
    ASSERT_TRUE(ctxOpt.HasValue()) << "BeginSubmit returned null";
    auto ctx = ctxOpt.Release();

    auto acquired = ctx->Acquire(*surface_);
    ASSERT_NE(acquired.Status, GpuSurfaceStatus::Lost) << "Acquire returned Lost";
    ASSERT_TRUE(acquired.Command.IsValid()) << "Acquire command handle is invalid";
    ctx->EndCommand(acquired.Command);

    GpuResourceHandle wrongBackBuffer{acquired.BackBuffer.Handle + 1, acquired.BackBuffer.NativeHandle};
    EXPECT_FALSE(ctx->Present(*surface_, wrongBackBuffer));

    ASSERT_TRUE(ctx->Present(*surface_, acquired.BackBuffer));
    GpuTask task = runtime_->Submit(std::move(ctx));
    ASSERT_TRUE(task.IsValid()) << "Submit after mismatched Present validation failed";
    task.Wait();
}

TEST_P(GpuPresentSurfaceTest, SingleSurfaceMultiCommandSubmit) {
    auto ctxOpt = runtime_->BeginSubmit();
    ASSERT_TRUE(ctxOpt.HasValue()) << "BeginSubmit returned null";
    auto ctx = ctxOpt.Release();

    auto acquired = ctx->Acquire(*surface_);
    ASSERT_NE(acquired.Status, GpuSurfaceStatus::Lost) << "Acquire returned Lost";
    ASSERT_TRUE(acquired.Command.IsValid()) << "Acquire command handle is invalid";
    ctx->EndCommand(acquired.Command);

    auto extra = ctx->BeginCommand(QueueType::Direct);
    ASSERT_TRUE(extra.IsValid()) << "Second command handle is invalid";
    ASSERT_NE(extra.NativeHandle, nullptr);

    ctx->EndCommand(extra);
    ASSERT_TRUE(ctx->Present(*surface_, acquired.BackBuffer));

    GpuTask task = runtime_->Submit(std::move(ctx));
    ASSERT_TRUE(task.IsValid()) << "Submit with multiple commands returned invalid task";
    task.Wait();
}

}  // namespace
