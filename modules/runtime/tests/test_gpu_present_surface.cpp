#include <atomic>
#include <cstdint>
#include <optional>
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

void ValidationLogCallback(LogLevel level, std::string_view message, void* /*userData*/) noexcept {
    Log({}, level, message);
}

// ---------------------------------------------------------------------------
// Helper: create a GpuRuntime for a given backend
// ---------------------------------------------------------------------------
Nullable<unique_ptr<GpuRuntime>> CreateRuntime(RenderBackend backend) noexcept {
    GpuRuntimeDescriptor desc{};
    desc.Backend = backend;
    desc.EnableDebugValidation = true;
    desc.LogCallback = ValidationLogCallback;
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

            ctx->Present(surface, result.BackBuffer);
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

        ctx->Present(surface, result.BackBuffer);
        GpuTask task = runtime.Submit(std::move(ctx));
        task.Wait();
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

        auto runtimeOpt = CreateRuntime(backend);
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
    }

    unique_ptr<NativeWindow> window_;
    unique_ptr<GpuRuntime> runtime_;
    unique_ptr<GpuPresentSurface> surface_;
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

}  // namespace
