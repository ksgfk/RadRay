#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <radray/logger.h>
#include <radray/render/common.h>
#define private public
#include <radray/runtime/gpu_system.h>
#undef private
#include <radray/window/native_window.h>

using namespace radray;
using namespace radray::render;

namespace {

using namespace std::chrono_literals;

constexpr uint32_t kInitialWidth = 640;
constexpr uint32_t kInitialHeight = 360;
constexpr uint32_t kResizedWidth = 800;
constexpr uint32_t kResizedHeight = 450;
constexpr uint32_t kBackBufferCount = 3;
constexpr uint32_t kFlightFrameCount = 2;
constexpr uint32_t kSingleThreadFrameCount = 24;
constexpr uint32_t kRenderThreadFrameCount = 24;
constexpr uint32_t kWarmupFrameCount = 12;
constexpr uint32_t kPostSwitchFrameCount = 16;
constexpr uint32_t kMultiSwapchainFrameCount = 12;
constexpr uint32_t kPreResizeFrameCount = 10;
constexpr uint32_t kPostResizeFrameCount = 12;
constexpr uint32_t kAcquireRetryMax = 240;
constexpr uint32_t kResizePumpCount = 32;

constexpr std::array<TextureFormat, 2> kFallbackFormats = {
    TextureFormat::BGRA8_UNORM,
    TextureFormat::RGBA8_UNORM};

class LogCollector {
public:
    static void Callback(LogLevel level, std::string_view message, void* userData) {
        auto* self = static_cast<LogCollector*>(userData);
        if (self == nullptr) {
            return;
        }
        if (level != LogLevel::Err && level != LogLevel::Critical) {
            return;
        }
        std::lock_guard<std::mutex> lock(self->_mutex);
        self->_errors.emplace_back(message);
    }

    std::vector<std::string> GetErrors() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _errors;
    }

    size_t GetErrorCount() const noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        return _errors.size();
    }

    void Truncate(size_t count) noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_errors.size() > count) {
            _errors.resize(count);
        }
    }

private:
    mutable std::mutex _mutex;
    std::vector<std::string> _errors;
};

class ScopedGlobalLogCallback {
public:
    explicit ScopedGlobalLogCallback(LogCollector* logs) noexcept {
        SetLogCallback(&LogCollector::Callback, logs);
    }

    ~ScopedGlobalLogCallback() noexcept {
        ClearLogCallback();
    }

    ScopedGlobalLogCallback(const ScopedGlobalLogCallback&) = delete;
    ScopedGlobalLogCallback& operator=(const ScopedGlobalLogCallback&) = delete;
};

struct SurfaceOptions {
    uint32_t WidthHint{0};
    uint32_t HeightHint{0};
    uint32_t BackBufferCount{kBackBufferCount};
    uint32_t FlightFrameCount{kFlightFrameCount};
    PresentMode PresentModeValue{PresentMode::FIFO};
    uint32_t QueueSlot{0};
};

struct SurfaceState {
    unique_ptr<NativeWindow> Window{};
    unique_ptr<GpuSurface> Surface{};
    TextureFormat Format{TextureFormat::UNKNOWN};
    std::vector<TextureState> BackBufferStates{};
    std::unordered_set<uint32_t> SeenBackBufferIndices{};
};

std::string JoinErrors(const std::vector<std::string>& errors, size_t maxCount = 8) {
    if (errors.empty()) {
        return {};
    }
    const size_t count = std::min(maxCount, errors.size());
    std::string result = fmt::format("{}", fmt::join(errors.begin(), errors.begin() + count, "\n"));
    if (errors.size() > count) {
        result += fmt::format("\n...({} more)", errors.size() - count);
    }
    return result;
}

std::string_view BackendTestName(RenderBackend backend) noexcept {
    switch (backend) {
        case RenderBackend::D3D12: return "D3D12";
        case RenderBackend::Vulkan: return "Vulkan";
        default: return "Unknown";
    }
}

std::vector<RenderBackend> GetEnabledRuntimeBackends() noexcept {
    std::vector<RenderBackend> backends{};
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
    backends.push_back(RenderBackend::D3D12);
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    backends.push_back(RenderBackend::Vulkan);
#endif
    return backends;
}

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

bool CreateRuntimeForBackend(
    RenderBackend backend,
    LogCollector* logs,
    unique_ptr<GpuRuntime>& runtime,
    std::string& reason) {
    switch (backend) {
        case RenderBackend::D3D12: {
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
            D3D12DeviceDescriptor desc{};
            desc.AdapterIndex = std::nullopt;
            desc.IsEnableDebugLayer = true;
            desc.IsEnableGpuBasedValid = true;
            desc.LogCallback = &LogCollector::Callback;
            desc.LogUserData = logs;
            auto runtimeOpt = GpuRuntime::Create(desc);
            if (!runtimeOpt.HasValue()) {
                reason = "GpuRuntime::Create(D3D12) failed";
                return false;
            }
            runtime = runtimeOpt.Release();
            return true;
#else
            RADRAY_UNUSED(logs);
            reason = "D3D12 backend is not enabled for this build";
            return false;
#endif
        }
        case RenderBackend::Vulkan: {
#if defined(RADRAY_ENABLE_VULKAN)
            VulkanInstanceDescriptor instanceDesc{};
            instanceDesc.AppName = "GpuRuntimeSmoke";
            instanceDesc.AppVersion = 1;
            instanceDesc.EngineName = "RadRay";
            instanceDesc.EngineVersion = 1;
            instanceDesc.IsEnableDebugLayer = true;
            instanceDesc.IsEnableGpuBasedValid = false;
            instanceDesc.LogCallback = &LogCollector::Callback;
            instanceDesc.LogUserData = logs;

            VulkanCommandQueueDescriptor queueDesc{};
            queueDesc.Type = QueueType::Direct;
            queueDesc.Count = 1;

            VulkanDeviceDescriptor deviceDesc{};
            deviceDesc.PhysicalDeviceIndex = std::nullopt;
            deviceDesc.Queues = std::span{&queueDesc, 1};

            auto runtimeOpt = GpuRuntime::Create(deviceDesc, instanceDesc);
            if (!runtimeOpt.HasValue()) {
                reason = "GpuRuntime::Create(Vulkan) failed";
                return false;
            }
            runtime = runtimeOpt.Release();
            return true;
#else
            RADRAY_UNUSED(logs);
            reason = "Vulkan backend is not enabled for this build";
            return false;
#endif
        }
        default: {
            reason = "Unsupported backend";
            return false;
        }
    }
}

bool CreateWindowForSurface(
    uint32_t width,
    uint32_t height,
    SurfaceState& state,
    std::string& reason) {
    auto windowOpt = CreateTestWindow(width, height);
    if (!windowOpt.HasValue()) {
        reason = "Cannot create native window for this platform.";
        return false;
    }
    state.Window = windowOpt.Release();
    auto size = state.Window->GetSize();
    if (size.X <= 0 || size.Y <= 0) {
        reason = "Window size is invalid before surface creation.";
        return false;
    }
    return true;
}

bool CreateSurfaceForWindow(
    GpuRuntime& runtime,
    SurfaceState& state,
    LogCollector* logs,
    std::span<const TextureFormat> formats,
    const SurfaceOptions& options,
    std::string& reason) {
    if (state.Window == nullptr) {
        reason = "Window is null.";
        return false;
    }

    const auto nativeHandler = state.Window->GetNativeHandler();
    if (nativeHandler.Handle == nullptr) {
        reason = "Window native handle is null.";
        return false;
    }

    auto size = state.Window->GetSize();
    uint32_t width = size.X > 0 ? static_cast<uint32_t>(size.X) : options.WidthHint;
    uint32_t height = size.Y > 0 ? static_cast<uint32_t>(size.Y) : options.HeightHint;
    if (width == 0 || height == 0) {
        reason = "Window size is invalid before surface creation.";
        return false;
    }

    std::string lastFailure = "GpuRuntime::CreateSurface failed";
    for (size_t i = 0; i < formats.size(); ++i) {
        const TextureFormat format = formats[i];
        const size_t errorBaseline = logs != nullptr ? logs->GetErrorCount() : 0;
        try {
            auto surface = runtime.CreateSurface(
                nativeHandler.Handle,
                width,
                height,
                options.BackBufferCount,
                options.FlightFrameCount,
                format,
                options.PresentModeValue,
                options.QueueSlot);
            if (surface == nullptr || !surface->IsValid()) {
                lastFailure = fmt::format("Created invalid surface for format {}", format);
            } else {
                const uint32_t actualBackBufferCount = surface->_swapchain->GetBackBufferCount();
                if (actualBackBufferCount == 0) {
                    lastFailure = "Surface back buffer count is zero.";
                } else {
                    state.Surface = std::move(surface);
                    state.Format = format;
                    state.BackBufferStates.assign(actualBackBufferCount, TextureState::Undefined);
                    state.SeenBackBufferIndices.clear();
                    return true;
                }
            }
        } catch (const std::exception& ex) {
            lastFailure = fmt::format("CreateSurface failed for format {}: {}", format, ex.what());
        } catch (...) {
            lastFailure = fmt::format("CreateSurface failed for format {} with unknown exception", format);
        }

        if (logs != nullptr && i + 1 < formats.size()) {
            logs->Truncate(errorBaseline);
        }
    }

    reason = lastFailure;
    return false;
}

void DestroySurfaceState(SurfaceState& state) noexcept {
    state.Surface.reset();
    if (state.Window != nullptr) {
        state.Window->Destroy();
    }
    state.Window.reset();
    state.BackBufferStates.clear();
    state.SeenBackBufferIndices.clear();
    state.Format = TextureFormat::UNKNOWN;
}

bool PumpWindow(SurfaceState& state, std::string& reason) {
    if (state.Window == nullptr) {
        reason = "Window is null.";
        return false;
    }
    state.Window->DispatchEvents();
    if (state.Window->ShouldClose()) {
        reason = "Window closed during test.";
        return false;
    }
    return true;
}

bool PumpWindows(SurfaceState& lhs, SurfaceState& rhs, std::string& reason) {
    return PumpWindow(lhs, reason) && PumpWindow(rhs, reason);
}

bool SubmitMinimalFrame(
    GpuRuntime& runtime,
    SurfaceState& state,
    unique_ptr<GpuFrameContext> frameContext,
    std::string& reason) {
    if (frameContext == nullptr) {
        reason = "Frame context is null.";
        return false;
    }

    const uint32_t backBufferIndex = frameContext->GetBackBufferIndex();
    if (backBufferIndex >= state.BackBufferStates.size()) {
        reason = fmt::format("Invalid back buffer index {}", backBufferIndex);
        return false;
    }

    auto* backBuffer = frameContext->GetBackBuffer();
    if (backBuffer == nullptr) {
        reason = "Frame context returned null back buffer.";
        return false;
    }

    try {
        auto* cmd = frameContext->CreateCommandBuffer();
        if (cmd == nullptr) {
            reason = "CreateCommandBuffer returned null.";
            return false;
        }

        cmd->Begin();
        if (state.BackBufferStates[backBufferIndex] != TextureState::Present) {
            ResourceBarrierDescriptor toPresent = BarrierTextureDescriptor{
                .Target = backBuffer,
                .Before = state.BackBufferStates[backBufferIndex],
                .After = TextureState::Present,
            };
            cmd->ResourceBarrier(std::span{&toPresent, 1});
        }
        cmd->End();

        state.SeenBackBufferIndices.emplace(backBufferIndex);
        auto task = runtime.Submit(std::move(frameContext));
        if (!task.IsValid()) {
            reason = "GpuRuntime::Submit returned an invalid task.";
            return false;
        }

        state.BackBufferStates[backBufferIndex] = TextureState::Present;
        runtime.ProcessTasks();
        return true;
    } catch (const std::exception& ex) {
        reason = fmt::format("SubmitMinimalFrame failed: {}", ex.what());
        return false;
    } catch (...) {
        reason = "SubmitMinimalFrame failed with unknown exception.";
        return false;
    }
}

template <typename RetryCallback>
bool TryRenderOneFrame(
    GpuRuntime& runtime,
    SurfaceState& state,
    RetryCallback&& onRetry,
    std::string& reason) {
    if (state.Surface == nullptr) {
        reason = "Surface is null.";
        return false;
    }

    for (uint32_t retry = 0; retry < kAcquireRetryMax; ++retry) {
        GpuRuntime::BeginFrameResult result{};
        try {
            result = runtime.TryBeginFrame(state.Surface.get());
        } catch (const std::exception& ex) {
            reason = fmt::format("TryBeginFrame threw: {}", ex.what());
            return false;
        } catch (...) {
            reason = "TryBeginFrame threw an unknown exception.";
            return false;
        }

        switch (result.Status) {
            case SwapChainAcquireStatus::Success: {
                if (!result.Context.HasValue()) {
                    reason = "TryBeginFrame returned Success without a frame context.";
                    return false;
                }
                return SubmitMinimalFrame(runtime, state, result.Context.Release(), reason);
            }
            case SwapChainAcquireStatus::RetryLater: {
                onRetry();
                runtime.ProcessTasks();
                std::this_thread::sleep_for(1ms);
                break;
            }
            case SwapChainAcquireStatus::RequireRecreate: {
                reason = "TryBeginFrame requested swapchain recreation unexpectedly.";
                return false;
            }
            case SwapChainAcquireStatus::Error:
            default: {
                reason = fmt::format("TryBeginFrame returned unexpected status {}", static_cast<int32_t>(result.Status));
                return false;
            }
        }
    }

    reason = "TryBeginFrame failed after retries.";
    return false;
}

bool RunFramesSingleThread(
    GpuRuntime& runtime,
    SurfaceState& state,
    uint32_t frameCount,
    std::string& reason) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        if (!PumpWindow(state, reason)) {
            return false;
        }

        bool windowClosed = false;
        if (!TryRenderOneFrame(
                runtime,
                state,
                [&]() {
                    if (state.Window != nullptr) {
                        state.Window->DispatchEvents();
                        windowClosed = state.Window->ShouldClose();
                    }
                },
                reason)) {
            if (windowClosed && reason.empty()) {
                reason = "Window closed during test.";
            }
            return false;
        }
    }
    return true;
}

bool RunFramesInterleavedSingleThread(
    GpuRuntime& runtime,
    SurfaceState& lhs,
    SurfaceState& rhs,
    uint32_t frameCount,
    std::string& reason) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        if (!PumpWindows(lhs, rhs, reason)) {
            return false;
        }

        bool windowClosed = false;
        auto pumpBoth = [&]() {
            if (lhs.Window != nullptr) {
                lhs.Window->DispatchEvents();
                windowClosed = windowClosed || lhs.Window->ShouldClose();
            }
            if (rhs.Window != nullptr) {
                rhs.Window->DispatchEvents();
                windowClosed = windowClosed || rhs.Window->ShouldClose();
            }
        };

        if (!TryRenderOneFrame(runtime, lhs, pumpBoth, reason)) {
            if (windowClosed && reason.empty()) {
                reason = "Window closed during multi-swapchain test.";
            }
            return false;
        }
        if (!TryRenderOneFrame(runtime, rhs, pumpBoth, reason)) {
            if (windowClosed && reason.empty()) {
                reason = "Window closed during multi-swapchain test.";
            }
            return false;
        }
    }
    return true;
}

bool RunFramesOnDedicatedRenderThread(
    GpuRuntime& runtime,
    SurfaceState& state,
    uint32_t frameCount,
    std::string& reason) {
    struct SharedState {
        std::mutex Mutex{};
        std::condition_variable Cv{};
        uint32_t Requested{0};
        uint32_t Completed{0};
        bool Stop{false};
        std::string Error{};
    };

    SharedState shared{};
    std::thread renderThread([&]() {
        while (true) {
            uint32_t targetFrame = 0;
            {
                std::unique_lock<std::mutex> lock(shared.Mutex);
                shared.Cv.wait(lock, [&]() {
                    return shared.Stop || shared.Requested > shared.Completed;
                });
                if (shared.Stop && shared.Requested <= shared.Completed) {
                    break;
                }
                targetFrame = shared.Completed + 1;
            }

            std::string localReason{};
            if (!TryRenderOneFrame(runtime, state, []() {}, localReason)) {
                std::lock_guard<std::mutex> lock(shared.Mutex);
                if (shared.Error.empty()) {
                    shared.Error = localReason;
                }
                shared.Stop = true;
                shared.Cv.notify_all();
                break;
            }

            {
                std::lock_guard<std::mutex> lock(shared.Mutex);
                shared.Completed = targetFrame;
            }
            shared.Cv.notify_all();
        }
    });

    for (uint32_t i = 0; i < frameCount; ++i) {
        {
            std::lock_guard<std::mutex> lock(shared.Mutex);
            shared.Requested = i + 1;
        }
        shared.Cv.notify_all();

        while (true) {
            {
                std::lock_guard<std::mutex> lock(shared.Mutex);
                if (!shared.Error.empty()) {
                    reason = shared.Error;
                    break;
                }
                if (shared.Completed >= i + 1) {
                    break;
                }
            }

            if (!PumpWindow(state, reason)) {
                std::lock_guard<std::mutex> lock(shared.Mutex);
                if (shared.Error.empty()) {
                    shared.Error = reason;
                }
                shared.Stop = true;
                shared.Cv.notify_all();
                break;
            }

            std::this_thread::sleep_for(1ms);
        }

        if (!reason.empty()) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(shared.Mutex);
        shared.Stop = true;
        if (reason.empty() && !shared.Error.empty()) {
            reason = shared.Error;
        }
    }
    shared.Cv.notify_all();
    renderThread.join();

    if (reason.empty()) {
        std::lock_guard<std::mutex> lock(shared.Mutex);
        if (!shared.Error.empty()) {
            reason = shared.Error;
        }
    }
    return reason.empty();
}

bool ForceSyncAndDrain(GpuRuntime& runtime, GpuSurface* surface, std::string& reason) {
    if (surface == nullptr || surface->_swapchain == nullptr) {
        reason = "Surface or swapchain is null during force sync.";
        return false;
    }

    auto* queue = surface->_swapchain->GetDesc().PresentQueue;
    if (queue == nullptr) {
        reason = "Present queue is null during force sync.";
        return false;
    }

    queue->Wait();
    runtime.ProcessTasks();
    if (!runtime._pendings.empty()) {
        runtime.ProcessTasks();
    }
    if (!runtime._pendings.empty()) {
        reason = fmt::format(
            "GpuRuntime still has {} pending submissions after queue wait.",
            runtime._pendings.size());
        return false;
    }
    return true;
}

bool ExpectNoCapturedErrors(const LogCollector& logs, std::string& reason) {
    const auto errors = logs.GetErrors();
    if (!errors.empty()) {
        reason = fmt::format("Captured render errors:\n{}", JoinErrors(errors));
        return false;
    }
    return true;
}

void PumpForResize(SurfaceState& state) {
    if (state.Window == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < kResizePumpCount; ++i) {
        state.Window->DispatchEvents();
        std::this_thread::sleep_for(2ms);
    }
}

class GpuRuntimeSmokeTest : public ::testing::TestWithParam<RenderBackend> {};

TEST_P(GpuRuntimeSmokeTest, SingleThreadAcquirePresentWorks) {
    LogCollector logs{};
    ScopedGlobalLogCallback logScope{&logs};
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), &logs, runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    SurfaceState state{};
    if (!CreateWindowForSurface(kInitialWidth, kInitialHeight, state, reason)) {
        GTEST_SKIP() << reason;
    }
    SurfaceOptions options{};
    options.WidthHint = kInitialWidth;
    options.HeightHint = kInitialHeight;
    ASSERT_TRUE(CreateSurfaceForWindow(*runtime, state, &logs, std::span<const TextureFormat>{kFallbackFormats}, options, reason))
        << reason;

    ASSERT_TRUE(RunFramesSingleThread(*runtime, state, kSingleThreadFrameCount, reason))
        << reason;
    ASSERT_TRUE(ForceSyncAndDrain(*runtime, state.Surface.get(), reason))
        << reason;
    ASSERT_GE(state.SeenBackBufferIndices.size(), 2u)
        << "Swapchain did not rotate back buffers as expected.";

    DestroySurfaceState(state);
    runtime.reset();

    ASSERT_TRUE(ExpectNoCapturedErrors(logs, reason))
        << reason;
}

TEST_P(GpuRuntimeSmokeTest, DedicatedRenderThreadAcquirePresentWorks) {
    LogCollector logs{};
    ScopedGlobalLogCallback logScope{&logs};
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), &logs, runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    SurfaceState state{};
    if (!CreateWindowForSurface(kInitialWidth, kInitialHeight, state, reason)) {
        GTEST_SKIP() << reason;
    }
    SurfaceOptions options{};
    options.WidthHint = kInitialWidth;
    options.HeightHint = kInitialHeight;
    ASSERT_TRUE(CreateSurfaceForWindow(*runtime, state, &logs, std::span<const TextureFormat>{kFallbackFormats}, options, reason))
        << reason;

    ASSERT_TRUE(RunFramesOnDedicatedRenderThread(*runtime, state, kRenderThreadFrameCount, reason))
        << reason;
    ASSERT_TRUE(ForceSyncAndDrain(*runtime, state.Surface.get(), reason))
        << reason;
    ASSERT_GE(state.SeenBackBufferIndices.size(), 2u)
        << "Swapchain did not rotate back buffers as expected.";

    DestroySurfaceState(state);
    runtime.reset();

    ASSERT_TRUE(ExpectNoCapturedErrors(logs, reason))
        << reason;
}

TEST_P(GpuRuntimeSmokeTest, SwitchesFromSingleThreadToDedicatedRenderThread) {
    LogCollector logs{};
    ScopedGlobalLogCallback logScope{&logs};
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), &logs, runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    SurfaceState state{};
    if (!CreateWindowForSurface(kInitialWidth, kInitialHeight, state, reason)) {
        GTEST_SKIP() << reason;
    }
    SurfaceOptions options{};
    options.WidthHint = kInitialWidth;
    options.HeightHint = kInitialHeight;
    ASSERT_TRUE(CreateSurfaceForWindow(*runtime, state, &logs, std::span<const TextureFormat>{kFallbackFormats}, options, reason))
        << reason;

    ASSERT_TRUE(RunFramesSingleThread(*runtime, state, kWarmupFrameCount, reason))
        << reason;
    ASSERT_TRUE(RunFramesOnDedicatedRenderThread(*runtime, state, kPostSwitchFrameCount, reason))
        << reason;
    ASSERT_TRUE(ForceSyncAndDrain(*runtime, state.Surface.get(), reason))
        << reason;
    ASSERT_GE(state.SeenBackBufferIndices.size(), 2u)
        << "Swapchain did not rotate back buffers as expected.";

    DestroySurfaceState(state);
    runtime.reset();

    ASSERT_TRUE(ExpectNoCapturedErrors(logs, reason))
        << reason;
}

TEST_P(GpuRuntimeSmokeTest, MultipleSwapchainsSubmitOnSameRuntime) {
    LogCollector logs{};
    ScopedGlobalLogCallback logScope{&logs};
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), &logs, runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    SurfaceState lhs{};
    SurfaceState rhs{};
    if (!CreateWindowForSurface(kInitialWidth, kInitialHeight, lhs, reason)) {
        GTEST_SKIP() << reason;
    }
    if (!CreateWindowForSurface(kInitialWidth + 80, kInitialHeight + 40, rhs, reason)) {
        GTEST_SKIP() << reason;
    }

    SurfaceOptions lhsOptions{};
    lhsOptions.WidthHint = kInitialWidth;
    lhsOptions.HeightHint = kInitialHeight;
    lhsOptions.QueueSlot = 0;
    SurfaceOptions rhsOptions{};
    rhsOptions.WidthHint = kInitialWidth + 80;
    rhsOptions.HeightHint = kInitialHeight + 40;
    rhsOptions.QueueSlot = 0;

    ASSERT_TRUE(CreateSurfaceForWindow(*runtime, lhs, &logs, std::span<const TextureFormat>{kFallbackFormats}, lhsOptions, reason))
        << reason;
    ASSERT_TRUE(CreateSurfaceForWindow(*runtime, rhs, &logs, std::span<const TextureFormat>{kFallbackFormats}, rhsOptions, reason))
        << reason;

    ASSERT_TRUE(RunFramesInterleavedSingleThread(*runtime, lhs, rhs, kMultiSwapchainFrameCount, reason))
        << reason;
    ASSERT_TRUE(ForceSyncAndDrain(*runtime, lhs.Surface.get(), reason))
        << reason;
    ASSERT_GE(lhs.SeenBackBufferIndices.size(), 2u)
        << "Primary swapchain did not rotate back buffers as expected.";
    ASSERT_GE(rhs.SeenBackBufferIndices.size(), 2u)
        << "Secondary swapchain did not rotate back buffers as expected.";

    DestroySurfaceState(lhs);
    DestroySurfaceState(rhs);
    runtime.reset();

    ASSERT_TRUE(ExpectNoCapturedErrors(logs, reason))
        << reason;
}

TEST_P(GpuRuntimeSmokeTest, RecreateSwapchainAfterResizeWithQueueWait) {
    LogCollector logs{};
    ScopedGlobalLogCallback logScope{&logs};
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), &logs, runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    SurfaceState state{};
    if (!CreateWindowForSurface(kInitialWidth, kInitialHeight, state, reason)) {
        GTEST_SKIP() << reason;
    }
    SurfaceOptions options{};
    options.WidthHint = kInitialWidth;
    options.HeightHint = kInitialHeight;
    ASSERT_TRUE(CreateSurfaceForWindow(*runtime, state, &logs, std::span<const TextureFormat>{kFallbackFormats}, options, reason))
        << reason;

    ASSERT_TRUE(RunFramesSingleThread(*runtime, state, kPreResizeFrameCount, reason))
        << reason;

    const auto originalFormat = state.Format;
    const auto originalQueueSlot = state.Surface->_queueSlot;
    const auto originalPresentMode = state.Surface->_swapchain->GetDesc().PresentMode;

    state.Window->SetSize(static_cast<int>(kResizedWidth), static_cast<int>(kResizedHeight));
    PumpForResize(state);

    ASSERT_TRUE(ForceSyncAndDrain(*runtime, state.Surface.get(), reason))
        << reason;

    state.Surface.reset();
    state.BackBufferStates.clear();
    state.SeenBackBufferIndices.clear();

    std::array<TextureFormat, 1> recreateFormats = {originalFormat};
    SurfaceOptions recreateOptions{};
    recreateOptions.WidthHint = kResizedWidth;
    recreateOptions.HeightHint = kResizedHeight;
    recreateOptions.BackBufferCount = kBackBufferCount;
    recreateOptions.FlightFrameCount = kFlightFrameCount;
    recreateOptions.PresentModeValue = originalPresentMode;
    recreateOptions.QueueSlot = originalQueueSlot;
    ASSERT_TRUE(CreateSurfaceForWindow(*runtime, state, &logs, std::span<const TextureFormat>{recreateFormats}, recreateOptions, reason))
        << reason;

    ASSERT_TRUE(RunFramesSingleThread(*runtime, state, kPostResizeFrameCount, reason))
        << reason;
    ASSERT_TRUE(ForceSyncAndDrain(*runtime, state.Surface.get(), reason))
        << reason;
    ASSERT_GE(state.SeenBackBufferIndices.size(), 2u)
        << "Swapchain did not rotate back buffers as expected after recreation.";

    DestroySurfaceState(state);
    runtime.reset();

    ASSERT_TRUE(ExpectNoCapturedErrors(logs, reason))
        << reason;
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    GpuRuntimeSmokeTest,
    ::testing::ValuesIn(GetEnabledRuntimeBackends()),
    [](const ::testing::TestParamInfo<RenderBackend>& info) {
        return std::string{BackendTestName(info.param)};
    });

}  // namespace
