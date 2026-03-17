#include <string>

#include <gtest/gtest.h>

#include <radray/runtime/gpu_system.h>
#include <radray/window/native_window.h>

using namespace radray;

namespace {

Nullable<unique_ptr<NativeWindow>> CreateTestWindow(uint32_t width, uint32_t height) noexcept {
#if defined(_WIN32)
    Win32WindowCreateDescriptor desc{};
    desc.Title = "GpuRuntimeSmoke";
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
    desc.Title = "GpuRuntimeSmoke";
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

Nullable<unique_ptr<GpuRuntime>> TryCreateRuntime(render::RenderBackend backend) noexcept {
    GpuRuntimeDescriptor desc{};
    desc.Backend = backend;
    desc.EnableDebugValidation = true;
    return GpuRuntime::Create(desc);
}

void RunCreateDestroySmoke(render::RenderBackend backend, std::string_view backendName) {
    auto runtimeOpt = TryCreateRuntime(backend);
    if (!runtimeOpt.HasValue()) {
        GTEST_SKIP() << backendName << " runtime unavailable";
    }

    auto runtime = runtimeOpt.Release();
    ASSERT_NE(runtime, nullptr);
    EXPECT_TRUE(runtime->IsValid());
    runtime->Destroy();
}

void RunBeginFrameAndAsyncSmoke(render::RenderBackend backend, std::string_view backendName) {
    auto runtimeOpt = TryCreateRuntime(backend);
    if (!runtimeOpt.HasValue()) {
        GTEST_SKIP() << backendName << " runtime unavailable";
    }

    auto runtime = runtimeOpt.Release();
    ASSERT_NE(runtime, nullptr);

    auto frameOpt = runtime->BeginFrame();
    if (!frameOpt.HasValue()) {
        GTEST_SKIP() << backendName << " BeginFrame unavailable";
    }
    auto frame = frameOpt.Release();
    ASSERT_NE(frame, nullptr);

    auto asyncOpt = runtime->BeginAsync();
    if (!asyncOpt.HasValue()) {
        GTEST_SKIP() << backendName << " BeginAsync unavailable";
    }
    auto asyncContext = asyncOpt.Release();
    ASSERT_NE(asyncContext, nullptr);
}

void RunEmptySubmitSmoke(render::RenderBackend backend, std::string_view backendName) {
    auto runtimeOpt = TryCreateRuntime(backend);
    if (!runtimeOpt.HasValue()) {
        GTEST_SKIP() << backendName << " runtime unavailable";
    }

    auto runtime = runtimeOpt.Release();
    ASSERT_NE(runtime, nullptr);

    auto frameOpt = runtime->BeginFrame();
    if (!frameOpt.HasValue()) {
        GTEST_SKIP() << backendName << " BeginFrame unavailable";
    }
    auto frame = frameOpt.Release();
    ASSERT_NE(frame, nullptr);
    auto frameTask = runtime->Submit(std::move(frame));
    EXPECT_FALSE(frameTask.IsValid());

    auto asyncOpt = runtime->BeginAsync();
    if (!asyncOpt.HasValue()) {
        GTEST_SKIP() << backendName << " BeginAsync unavailable";
    }
    auto asyncContext = asyncOpt.Release();
    ASSERT_NE(asyncContext, nullptr);
    auto asyncTask = runtime->Submit(std::move(asyncContext));
    EXPECT_FALSE(asyncTask.IsValid());

    runtime->ProcessTasks();
}

void RunCreatePresentSurfaceSmoke(render::RenderBackend backend, std::string_view backendName) {
    auto windowOpt = CreateTestWindow(640, 360);
    if (!windowOpt.HasValue()) {
        GTEST_SKIP() << "window unavailable for " << backendName;
    }
    auto window = windowOpt.Release();
    ASSERT_NE(window, nullptr);
    if (!window->IsValid()) {
        GTEST_SKIP() << "window invalid for " << backendName;
    }

    auto runtimeOpt = TryCreateRuntime(backend);
    if (!runtimeOpt.HasValue()) {
        GTEST_SKIP() << backendName << " runtime unavailable";
    }
    auto runtime = runtimeOpt.Release();
    ASSERT_NE(runtime, nullptr);

    WindowNativeHandler nativeHandler = window->GetNativeHandler();
    if (nativeHandler.Handle == nullptr) {
        GTEST_SKIP() << "native handle unavailable for " << backendName;
    }

    GpuPresentSurfaceDescriptor desc{};
    desc.NativeWindowHandle = nativeHandler.Handle;
    desc.Width = 640;
    desc.Height = 360;
    desc.BackBufferCount = 3;
    desc.FlightFrameCount = 2;
    desc.Format = render::TextureFormat::BGRA8_UNORM;
    desc.PresentMode = render::PresentMode::FIFO;

    auto surfaceOpt = runtime->CreatePresentSurface(desc);
    if (!surfaceOpt.HasValue()) {
        GTEST_SKIP() << backendName << " CreatePresentSurface unavailable";
    }
    auto surface = surfaceOpt.Release();
    ASSERT_NE(surface, nullptr);
    EXPECT_TRUE(surface->IsValid());

    auto frameOpt = runtime->BeginFrame();
    if (!frameOpt.HasValue()) {
        GTEST_SKIP() << backendName << " BeginFrame unavailable";
    }
    auto frame = frameOpt.Release();
    ASSERT_NE(frame, nullptr);

    auto acquire = frame->AcquireSurface(*surface);
    if (acquire.Status != GpuSurfaceAcquireStatus::Success || acquire.Lease == nullptr) {
        GTEST_SKIP() << backendName << " surface acquire unavailable";
    }

    EXPECT_TRUE(frame->Present(*acquire.Lease.Get()));
    runtime->Submit(std::move(frame));
    runtime->ProcessTasks();
}

TEST(GpuRuntimeSmoke, CreateDestroy_D3D12) {
#if defined(RADRAY_ENABLE_D3D12)
    RunCreateDestroySmoke(render::RenderBackend::D3D12, "D3D12");
#else
    GTEST_SKIP() << "D3D12 disabled";
#endif
}

TEST(GpuRuntimeSmoke, CreateDestroy_Vulkan) {
#if defined(RADRAY_ENABLE_VULKAN)
    RunCreateDestroySmoke(render::RenderBackend::Vulkan, "Vulkan");
#else
    GTEST_SKIP() << "Vulkan disabled";
#endif
}

TEST(GpuRuntimeSmoke, BeginFrameAndBeginAsync_D3D12) {
#if defined(RADRAY_ENABLE_D3D12)
    RunBeginFrameAndAsyncSmoke(render::RenderBackend::D3D12, "D3D12");
#else
    GTEST_SKIP() << "D3D12 disabled";
#endif
}

TEST(GpuRuntimeSmoke, BeginFrameAndBeginAsync_Vulkan) {
#if defined(RADRAY_ENABLE_VULKAN)
    RunBeginFrameAndAsyncSmoke(render::RenderBackend::Vulkan, "Vulkan");
#else
    GTEST_SKIP() << "Vulkan disabled";
#endif
}

TEST(GpuRuntimeSmoke, EmptySubmitAndProcessTasks_D3D12) {
#if defined(RADRAY_ENABLE_D3D12)
    RunEmptySubmitSmoke(render::RenderBackend::D3D12, "D3D12");
#else
    GTEST_SKIP() << "D3D12 disabled";
#endif
}

TEST(GpuRuntimeSmoke, EmptySubmitAndProcessTasks_Vulkan) {
#if defined(RADRAY_ENABLE_VULKAN)
    RunEmptySubmitSmoke(render::RenderBackend::Vulkan, "Vulkan");
#else
    GTEST_SKIP() << "Vulkan disabled";
#endif
}

TEST(GpuRuntimeSmoke, CreatePresentSurface_D3D12) {
#if defined(RADRAY_ENABLE_D3D12)
    RunCreatePresentSurfaceSmoke(render::RenderBackend::D3D12, "D3D12");
#else
    GTEST_SKIP() << "D3D12 disabled";
#endif
}

TEST(GpuRuntimeSmoke, CreatePresentSurface_Vulkan) {
#if defined(RADRAY_ENABLE_VULKAN)
    RunCreatePresentSurfaceSmoke(render::RenderBackend::Vulkan, "Vulkan");
#else
    GTEST_SKIP() << "Vulkan disabled";
#endif
}

}  // namespace
