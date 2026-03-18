#include <chrono>
#include <deque>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>
#include <radray/window/native_window.h>

using namespace radray;

namespace {

constexpr uint32_t kWindowWidth = 640;
constexpr uint32_t kWindowHeight = 360;
constexpr uint32_t kBackBufferCount = 2;
constexpr uint32_t kFrameInFlightCount = 2;
constexpr uint32_t kSmokeFrameCount = 3;
constexpr auto kWorkerPollInterval = std::chrono::milliseconds(1);
constexpr auto kTryBeginFrameMaxBlockTime = std::chrono::milliseconds(50);
constexpr auto kBeginFrameShouldBlockTime = std::chrono::milliseconds(20);

enum class TestBackend {
    D3D12,
    Vulkan,
};

struct ScenarioResult {
    enum class Status {
        Passed,
        Skipped,
        Failed,
    };

    Status TestStatus{Status::Passed};
    std::string Message{};
};

struct SurfaceInputs {
    WindowNativeHandler NativeHandler{};
    WindowVec2i Size{};
};

struct RuntimeSurfaceSetup {
    unique_ptr<GpuRuntime> Runtime{};
    unique_ptr<GpuSurface> Surface{};
    render::SwapChainDescriptor SurfaceDesc{};
};

using SubmittedTasks = std::deque<GpuTask>;

class RhiLogCollector {
public:
    static void Callback(LogLevel level, std::string_view message, void* userData) {
        auto* self = static_cast<RhiLogCollector*>(userData);
        if (self == nullptr) {
            return;
        }
        if (level != LogLevel::Err && level != LogLevel::Critical) {
            return;
        }

        std::lock_guard<std::mutex> lock(self->_mutex);
        self->_messages.emplace_back(message);
    }

    std::vector<std::string> GetMessages() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _messages;
    }

private:
    mutable std::mutex _mutex;
    std::vector<std::string> _messages;
};

ScenarioResult Passed() {
    return {};
}

ScenarioResult Skipped(std::string message) {
    return ScenarioResult{ScenarioResult::Status::Skipped, std::move(message)};
}

ScenarioResult Failed(std::string message) {
    return ScenarioResult{ScenarioResult::Status::Failed, std::move(message)};
}

bool IsPassed(const ScenarioResult& result) noexcept {
    return result.TestStatus == ScenarioResult::Status::Passed;
}

std::string JoinMessages(const std::vector<std::string>& messages, size_t maxCount = 8) {
    std::string result;
    const size_t count = std::min(maxCount, messages.size());
    for (size_t i = 0; i < count; ++i) {
        if (!result.empty()) {
            result += '\n';
        }
        result += messages[i];
    }
    if (messages.size() > count) {
        result += "\n...(" + std::to_string(messages.size() - count) + " more)";
    }
    return result;
}

ScenarioResult CheckNoRhiErrors(const RhiLogCollector& logs) {
    const auto messages = logs.GetMessages();
    if (!messages.empty()) {
        return Failed("Captured RHI error-or-higher messages:\n" + JoinMessages(messages));
    }
    return Passed();
}

std::string_view GetBackendName(TestBackend backend) noexcept {
    switch (backend) {
        case TestBackend::D3D12:
            return "D3D12";
        case TestBackend::Vulkan:
            return "Vulkan";
    }
    return "Unknown";
}

Nullable<unique_ptr<NativeWindow>> CreateTestWindow() noexcept {
#if defined(_WIN32)
    Win32WindowCreateDescriptor desc{};
    desc.Title = "GpuRuntimeSmoke";
    desc.Width = static_cast<int32_t>(kWindowWidth);
    desc.Height = static_cast<int32_t>(kWindowHeight);
    desc.X = 120;
    desc.Y = 120;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return CreateNativeWindow(desc);
#elif defined(__APPLE__)
    CocoaWindowCreateDescriptor desc{};
    desc.Title = "GpuRuntimeSmoke";
    desc.Width = static_cast<int32_t>(kWindowWidth);
    desc.Height = static_cast<int32_t>(kWindowHeight);
    desc.X = 120;
    desc.Y = 120;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return CreateNativeWindow(desc);
#else
    return nullptr;
#endif
}

Nullable<unique_ptr<GpuRuntime>> CreateRuntimeForBackend(TestBackend backend, RhiLogCollector* logs) {
    switch (backend) {
        case TestBackend::D3D12: {
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
            render::D3D12DeviceDescriptor deviceDesc{};
            deviceDesc.AdapterIndex = std::nullopt;
            deviceDesc.IsEnableDebugLayer = true;
            deviceDesc.IsEnableGpuBasedValid = true;
            deviceDesc.LogCallback = &RhiLogCollector::Callback;
            deviceDesc.LogUserData = logs;
            return GpuRuntime::Create(render::DeviceDescriptor{deviceDesc}, std::nullopt);
#else
            static_cast<void>(logs);
            return nullptr;
#endif
        }
        case TestBackend::Vulkan: {
#if defined(RADRAY_ENABLE_VULKAN)
            render::VulkanInstanceDescriptor instanceDesc{};
            instanceDesc.AppName = "GpuRuntimeSmoke";
            instanceDesc.AppVersion = 1;
            instanceDesc.EngineName = "RadRay";
            instanceDesc.EngineVersion = 1;
            instanceDesc.IsEnableDebugLayer = true;
            instanceDesc.IsEnableGpuBasedValid = false;
            instanceDesc.LogCallback = &RhiLogCollector::Callback;
            instanceDesc.LogUserData = logs;

            render::VulkanCommandQueueDescriptor queueDescs[] = {
                {
                    .Type = render::QueueType::Direct,
                    .Count = 1,
                }};
            render::VulkanDeviceDescriptor deviceDesc{};
            deviceDesc.PhysicalDeviceIndex = std::nullopt;
            deviceDesc.Queues = std::span{queueDescs};
            return GpuRuntime::Create(
                render::DeviceDescriptor{deviceDesc},
                std::optional<render::VulkanInstanceDescriptor>{instanceDesc});
#else
            static_cast<void>(logs);
            return nullptr;
#endif
        }
    }
    static_cast<void>(logs);
    return nullptr;
}

std::optional<SurfaceInputs> GetSurfaceInputs(NativeWindow* window, std::string& reason) {
    if (window == nullptr || !window->IsValid()) {
        reason = "Native window is invalid.";
        return std::nullopt;
    }
    const WindowVec2i size = window->GetSize();
    if (size.X <= 0 || size.Y <= 0) {
        reason = "Window size is invalid before surface creation.";
        return std::nullopt;
    }
    const WindowNativeHandler nativeHandler = window->GetNativeHandler();
    if (nativeHandler.Handle == nullptr) {
        reason = "Window native handle is null.";
        return std::nullopt;
    }
    return SurfaceInputs{
        .NativeHandler = nativeHandler,
        .Size = size,
    };
}

render::SwapChainDescriptor BuildSurfaceDescriptor(const SurfaceInputs& inputs) noexcept {
    render::SwapChainDescriptor surfaceDesc{};
    // GpuRuntime does not expose a public present queue yet. Keep the descriptor
    // wired up so the acquire/present flow is compiled, and fill the queue later
    // when gpu_system has a concrete implementation.
    surfaceDesc.PresentQueue = nullptr;
    surfaceDesc.NativeHandler = inputs.NativeHandler.Handle;
    surfaceDesc.Width = static_cast<uint32_t>(inputs.Size.X);
    surfaceDesc.Height = static_cast<uint32_t>(inputs.Size.Y);
    surfaceDesc.BackBufferCount = kBackBufferCount;
    surfaceDesc.FlightFrameCount = kFrameInFlightCount;
    surfaceDesc.Format = render::TextureFormat::BGRA8_UNORM;
    surfaceDesc.PresentMode = render::PresentMode::FIFO;
    return surfaceDesc;
}

ScenarioResult ValidateSurfaceBasics(const GpuSurface& surface, const render::SwapChainDescriptor& surfaceDesc) {
    if (!surface.IsValid()) {
        return Failed("GpuSurface is invalid after CreateSurface.");
    }
    if (surface.GetWidth() != surfaceDesc.Width) {
        return Failed("GpuSurface width does not match the swapchain descriptor.");
    }
    if (surface.GetHeight() != surfaceDesc.Height) {
        return Failed("GpuSurface height does not match the swapchain descriptor.");
    }
    if (surface.GetFormat() != surfaceDesc.Format) {
        return Failed("GpuSurface format does not match the swapchain descriptor.");
    }
    if (surface.GetPresentMode() != surfaceDesc.PresentMode) {
        return Failed("GpuSurface present mode does not match the swapchain descriptor.");
    }
    return Passed();
}

ScenarioResult ValidateFrameContext(const GpuFrameContext& frame) {
    if (!frame.GetBackBuffer().IsValid()) {
        return Failed("GpuFrameContext returned an invalid back-buffer handle.");
    }
    if (frame.GetBackBufferIndex() == std::numeric_limits<uint32_t>::max()) {
        return Failed("GpuFrameContext returned an invalid back-buffer index.");
    }
    return Passed();
}

ScenarioResult InitializeRuntimeSurface(
    TestBackend backend,
    const SurfaceInputs& inputs,
    RhiLogCollector* logs,
    RuntimeSurfaceSetup& setup) {
    auto runtimeOpt = CreateRuntimeForBackend(backend, logs);
    if (!runtimeOpt.HasValue()) {
        return Skipped(std::string(GetBackendName(backend)) + " backend is not available for GpuRuntime::Create.");
    }
    setup.Runtime = runtimeOpt.Release();
    if (setup.Runtime == nullptr || !setup.Runtime->IsValid()) {
        return Skipped("GpuRuntime is not valid yet.");
    }

    setup.SurfaceDesc = BuildSurfaceDescriptor(inputs);
    setup.Surface = setup.Runtime->CreateSurface(setup.SurfaceDesc, 0);
    if (setup.Surface == nullptr) {
        return Skipped("GpuRuntime::CreateSurface is not implemented yet.");
    }

    return ValidateSurfaceBasics(*setup.Surface, setup.SurfaceDesc);
}

ScenarioResult SubmitFrameAndWait(GpuRuntime* runtime, unique_ptr<GpuFrameContext> frame) {
    if (runtime == nullptr) {
        return Failed("GpuRuntime is null during submit.");
    }
    if (frame == nullptr) {
        return Skipped("GpuRuntime::BeginFrame is not implemented yet.");
    }

    const ScenarioResult frameValidation = ValidateFrameContext(*frame);
    if (!IsPassed(frameValidation)) {
        return frameValidation;
    }

    auto task = runtime->Submit(std::move(frame));
    if (!task.IsValid()) {
        return Skipped("GpuRuntime::Submit is not implemented yet.");
    }

    runtime->ProcessTasks();
    task.Wait();
    if (!task.IsCompleted()) {
        return Failed("GpuTask is not completed after Wait().");
    }
    return Passed();
}

ScenarioResult SubmitFrameWithoutWaiting(
    GpuRuntime* runtime,
    unique_ptr<GpuFrameContext> frame,
    SubmittedTasks& submittedTasks) {
    if (runtime == nullptr) {
        return Failed("GpuRuntime is null during submit.");
    }
    if (frame == nullptr) {
        return Skipped("GpuRuntime::BeginFrame is not implemented yet.");
    }

    const ScenarioResult frameValidation = ValidateFrameContext(*frame);
    if (!IsPassed(frameValidation)) {
        return frameValidation;
    }

    auto task = runtime->Submit(std::move(frame));
    if (!task.IsValid()) {
        return Skipped("GpuRuntime::Submit is not implemented yet.");
    }

    runtime->ProcessTasks();
    submittedTasks.emplace_back(std::move(task));
    return Passed();
}

ScenarioResult WaitSubmittedTasks(GpuRuntime* runtime, SubmittedTasks& submittedTasks) {
    if (runtime == nullptr) {
        return Failed("GpuRuntime is null during task drain.");
    }

    runtime->ProcessTasks();
    while (!submittedTasks.empty()) {
        GpuTask task = std::move(submittedTasks.front());
        submittedTasks.pop_front();
        task.Wait();
        if (!task.IsCompleted()) {
            return Failed("GpuTask is not completed after Wait().");
        }
        runtime->ProcessTasks();
    }
    return Passed();
}

ScenarioResult FillFrameQueue(
    NativeWindow* window,
    GpuRuntime* runtime,
    GpuSurface* surface,
    SubmittedTasks& submittedTasks,
    uint32_t frameCount) {
    for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        if (window != nullptr) {
            window->DispatchEvents();
            if (window->ShouldClose()) {
                return Failed("Window closed while filling the in-flight frame queue.");
            }
        }

        const ScenarioResult submitResult = SubmitFrameWithoutWaiting(runtime, runtime->BeginFrame(surface), submittedTasks);
        if (!IsPassed(submitResult)) {
            return submitResult;
        }
    }
    return Passed();
}

ScenarioResult WaitForWorkerScenario(
    NativeWindow* window,
    std::future<ScenarioResult>& future,
    std::thread& worker,
    std::string_view closeMessage) {
    while (future.wait_for(kWorkerPollInterval) != std::future_status::ready) {
        if (window != nullptr) {
            window->DispatchEvents();
            if (window->ShouldClose()) {
                worker.join();
                return Failed(std::string(closeMessage));
            }
        }
        std::this_thread::sleep_for(kWorkerPollInterval);
    }

    worker.join();
    try {
        return future.get();
    } catch (const std::exception& ex) {
        return Failed(std::string("Worker thread threw an exception: ") + ex.what());
    } catch (...) {
        return Failed("Worker thread threw an unknown exception.");
    }
}

ScenarioResult RunGpuRuntimeSwapChainSmoke(TestBackend backend) {
    RhiLogCollector logs{};
    auto windowOpt = CreateTestWindow();
    if (!windowOpt.HasValue()) {
        return Skipped("Cannot create a native window on this platform.");
    }
    auto window = windowOpt.Release();

    std::string reason;
    const auto inputsOpt = GetSurfaceInputs(window.get(), reason);
    if (!inputsOpt.has_value()) {
        return Skipped(std::move(reason));
    }

    RuntimeSurfaceSetup setup{};
    const ScenarioResult initResult = InitializeRuntimeSurface(backend, *inputsOpt, &logs, setup);
    if (!IsPassed(initResult)) {
        return initResult;
    }

    SubmittedTasks submittedTasks{};
    for (uint32_t frameIndex = 0; frameIndex < kSmokeFrameCount; ++frameIndex) {
        window->DispatchEvents();
        if (window->ShouldClose()) {
            return Failed("Window closed during the smoke test.");
        }

        if (submittedTasks.size() >= kFrameInFlightCount) {
            const ScenarioResult drainResult = WaitSubmittedTasks(setup.Runtime.get(), submittedTasks);
            if (!IsPassed(drainResult)) {
                return drainResult;
            }
        }

        const ScenarioResult submitResult = SubmitFrameWithoutWaiting(
            setup.Runtime.get(),
            setup.Runtime->BeginFrame(setup.Surface.get()),
            submittedTasks);
        if (!IsPassed(submitResult)) {
            return submitResult;
        }
    }

    const ScenarioResult finalDrainResult = WaitSubmittedTasks(setup.Runtime.get(), submittedTasks);
    if (!IsPassed(finalDrainResult)) {
        return finalDrainResult;
    }

    setup.Surface->Destroy();
    setup.Runtime->Destroy();
    window->Destroy();
    return CheckNoRhiErrors(logs);
}

ScenarioResult RunTryBeginFrameNonBlockingWhenGpuBusySmoke(TestBackend backend) {
    RhiLogCollector logs{};
    auto windowOpt = CreateTestWindow();
    if (!windowOpt.HasValue()) {
        return Skipped("Cannot create a native window on this platform.");
    }
    auto window = windowOpt.Release();

    std::string reason;
    const auto inputsOpt = GetSurfaceInputs(window.get(), reason);
    if (!inputsOpt.has_value()) {
        return Skipped(std::move(reason));
    }

    RuntimeSurfaceSetup setup{};
    const ScenarioResult initResult = InitializeRuntimeSurface(backend, *inputsOpt, &logs, setup);
    if (!IsPassed(initResult)) {
        return initResult;
    }

    SubmittedTasks submittedTasks{};
    const ScenarioResult fillResult = FillFrameQueue(
        window.get(),
        setup.Runtime.get(),
        setup.Surface.get(),
        submittedTasks,
        kFrameInFlightCount);
    if (!IsPassed(fillResult)) {
        return fillResult;
    }

    window->DispatchEvents();
    const auto begin = std::chrono::steady_clock::now();
    auto frameOpt = setup.Runtime->TryBeginFrame(setup.Surface.get());
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    if (elapsed > kTryBeginFrameMaxBlockTime) {
        return Failed(
            "GpuRuntime::TryBeginFrame blocked for " +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) +
            " ms.");
    }

    if (frameOpt.HasValue()) {
        const ScenarioResult submitResult = SubmitFrameWithoutWaiting(setup.Runtime.get(), frameOpt.Release(), submittedTasks);
        if (!IsPassed(submitResult)) {
            return submitResult;
        }
    }

    const ScenarioResult drainResult = WaitSubmittedTasks(setup.Runtime.get(), submittedTasks);
    if (!IsPassed(drainResult)) {
        return drainResult;
    }

    setup.Surface->Destroy();
    setup.Runtime->Destroy();
    window->Destroy();
    return CheckNoRhiErrors(logs);
}

ScenarioResult RunBeginFrameBlocksWhenGpuBusySmoke(TestBackend backend) {
    RhiLogCollector logs{};
    auto windowOpt = CreateTestWindow();
    if (!windowOpt.HasValue()) {
        return Skipped("Cannot create a native window on this platform.");
    }
    auto window = windowOpt.Release();

    std::string reason;
    const auto inputsOpt = GetSurfaceInputs(window.get(), reason);
    if (!inputsOpt.has_value()) {
        return Skipped(std::move(reason));
    }

    RuntimeSurfaceSetup setup{};
    const ScenarioResult initResult = InitializeRuntimeSurface(backend, *inputsOpt, &logs, setup);
    if (!IsPassed(initResult)) {
        return initResult;
    }

    SubmittedTasks submittedTasks{};
    const ScenarioResult fillResult = FillFrameQueue(
        window.get(),
        setup.Runtime.get(),
        setup.Surface.get(),
        submittedTasks,
        kFrameInFlightCount);
    if (!IsPassed(fillResult)) {
        return fillResult;
    }

    std::packaged_task<ScenarioResult()> workerTask([
                                                      runtime = setup.Runtime.get(),
                                                      surface = setup.Surface.get()]() mutable {
        return SubmitFrameAndWait(runtime, runtime->BeginFrame(surface));
    });
    auto future = workerTask.get_future();
    std::thread worker(std::move(workerTask));

    const auto deadline = std::chrono::steady_clock::now() + kBeginFrameShouldBlockTime;
    while (std::chrono::steady_clock::now() < deadline) {
        window->DispatchEvents();
        if (window->ShouldClose()) {
            worker.join();
            return Failed("Window closed while observing BeginFrame blocking behavior.");
        }
        if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            worker.join();
            const ScenarioResult earlyResult = future.get();
            if (earlyResult.TestStatus == ScenarioResult::Status::Skipped) {
                return earlyResult;
            }
            return Failed("GpuRuntime::BeginFrame did not block when the in-flight frame queue was full.");
        }
        std::this_thread::sleep_for(kWorkerPollInterval);
    }

    const ScenarioResult drainResult = WaitSubmittedTasks(setup.Runtime.get(), submittedTasks);
    if (!IsPassed(drainResult)) {
        worker.join();
        return drainResult;
    }

    const ScenarioResult workerResult = WaitForWorkerScenario(
        window.get(),
        future,
        worker,
        "Window closed while waiting for blocked BeginFrame to resume.");
    if (!IsPassed(workerResult)) {
        return workerResult;
    }

    setup.Surface->Destroy();
    setup.Runtime->Destroy();
    window->Destroy();
    return CheckNoRhiErrors(logs);
}

ScenarioResult RunGpuRuntimeScheduledOnWorkerThreadSmoke(TestBackend backend) {
    RhiLogCollector logs{};
    auto windowOpt = CreateTestWindow();
    if (!windowOpt.HasValue()) {
        return Skipped("Cannot create a native window on this platform.");
    }
    auto window = windowOpt.Release();

    std::string reason;
    const auto inputsOpt = GetSurfaceInputs(window.get(), reason);
    if (!inputsOpt.has_value()) {
        return Skipped(std::move(reason));
    }
    const SurfaceInputs inputs = *inputsOpt;

    std::packaged_task<ScenarioResult()> workerTask([backend, inputs, &logs]() mutable {
        RuntimeSurfaceSetup setup{};
        const ScenarioResult initResult = InitializeRuntimeSurface(backend, inputs, &logs, setup);
        if (!IsPassed(initResult)) {
            return initResult;
        }

        const ScenarioResult submitResult = SubmitFrameAndWait(setup.Runtime.get(), setup.Runtime->BeginFrame(setup.Surface.get()));
        if (!IsPassed(submitResult)) {
            return submitResult;
        }

        setup.Surface->Destroy();
        setup.Runtime->Destroy();
        return Passed();
    });
    auto future = workerTask.get_future();
    std::thread worker(std::move(workerTask));

    const ScenarioResult result = WaitForWorkerScenario(
        window.get(),
        future,
        worker,
        "Window closed while worker thread was scheduling GpuRuntime.");

    window->Destroy();
    if (!IsPassed(result)) {
        return result;
    }
    return CheckNoRhiErrors(logs);
}

ScenarioResult RunMainThreadThenWorkerThreadSchedulingSmoke(TestBackend backend) {
    RhiLogCollector logs{};
    auto windowOpt = CreateTestWindow();
    if (!windowOpt.HasValue()) {
        return Skipped("Cannot create a native window on this platform.");
    }
    auto window = windowOpt.Release();

    std::string reason;
    const auto inputsOpt = GetSurfaceInputs(window.get(), reason);
    if (!inputsOpt.has_value()) {
        return Skipped(std::move(reason));
    }

    RuntimeSurfaceSetup setup{};
    const ScenarioResult initResult = InitializeRuntimeSurface(backend, *inputsOpt, &logs, setup);
    if (!IsPassed(initResult)) {
        return initResult;
    }

    auto mainThreadFrame = setup.Runtime->BeginFrame(setup.Surface.get());
    if (mainThreadFrame == nullptr) {
        return Skipped("GpuRuntime::BeginFrame is not implemented yet.");
    }
    const ScenarioResult frameValidation = ValidateFrameContext(*mainThreadFrame);
    if (!IsPassed(frameValidation)) {
        return frameValidation;
    }

    std::packaged_task<ScenarioResult()> workerTask([
                                                      runtime = std::move(setup.Runtime),
                                                      surface = std::move(setup.Surface),
                                                      mainThreadFrame = std::move(mainThreadFrame)]() mutable {
        const ScenarioResult submitMainFrameResult = SubmitFrameAndWait(runtime.get(), std::move(mainThreadFrame));
        if (!IsPassed(submitMainFrameResult)) {
            return submitMainFrameResult;
        }

        const ScenarioResult submitWorkerFrameResult = SubmitFrameAndWait(runtime.get(), runtime->BeginFrame(surface.get()));
        if (!IsPassed(submitWorkerFrameResult)) {
            return submitWorkerFrameResult;
        }

        surface->Destroy();
        runtime->Destroy();
        return Passed();
    });
    auto future = workerTask.get_future();
    std::thread worker(std::move(workerTask));

    const ScenarioResult result = WaitForWorkerScenario(
        window.get(),
        future,
        worker,
        "Window closed while GpuRuntime was being handed off to a worker thread.");

    window->Destroy();
    if (!IsPassed(result)) {
        return result;
    }
    return CheckNoRhiErrors(logs);
}

static_assert(std::is_convertible_v<std::unique_ptr<GpuFrameContext>, std::unique_ptr<GpuAsyncContext>>);

}  // namespace

TEST(GpuRuntimeSmoke, AcquirePresentSwapChain_D3D12) {
    const ScenarioResult result = RunGpuRuntimeSwapChainSmoke(TestBackend::D3D12);
    if (result.TestStatus == ScenarioResult::Status::Skipped) {
        GTEST_SKIP() << result.Message;
    }
    if (result.TestStatus == ScenarioResult::Status::Failed) {
        FAIL() << result.Message;
    }
}

TEST(GpuRuntimeSmoke, AcquirePresentSwapChain_Vulkan) {
    const ScenarioResult result = RunGpuRuntimeSwapChainSmoke(TestBackend::Vulkan);
    if (result.TestStatus == ScenarioResult::Status::Skipped) {
        GTEST_SKIP() << result.Message;
    }
    if (result.TestStatus == ScenarioResult::Status::Failed) {
        FAIL() << result.Message;
    }
}

TEST(GpuRuntimeSmoke, BeginFrameBlocksWhenGpuBusy_D3D12) {
    const ScenarioResult result = RunBeginFrameBlocksWhenGpuBusySmoke(TestBackend::D3D12);
    if (result.TestStatus == ScenarioResult::Status::Skipped) {
        GTEST_SKIP() << result.Message;
    }
    if (result.TestStatus == ScenarioResult::Status::Failed) {
        FAIL() << result.Message;
    }
}

TEST(GpuRuntimeSmoke, BeginFrameBlocksWhenGpuBusy_Vulkan) {
    const ScenarioResult result = RunBeginFrameBlocksWhenGpuBusySmoke(TestBackend::Vulkan);
    if (result.TestStatus == ScenarioResult::Status::Skipped) {
        GTEST_SKIP() << result.Message;
    }
    if (result.TestStatus == ScenarioResult::Status::Failed) {
        FAIL() << result.Message;
    }
}

TEST(GpuRuntimeSmoke, TryBeginFrameDoesNotBlockWhenGpuBusy_D3D12) {
    const ScenarioResult result = RunTryBeginFrameNonBlockingWhenGpuBusySmoke(TestBackend::D3D12);
    if (result.TestStatus == ScenarioResult::Status::Skipped) {
        GTEST_SKIP() << result.Message;
    }
    if (result.TestStatus == ScenarioResult::Status::Failed) {
        FAIL() << result.Message;
    }
}

TEST(GpuRuntimeSmoke, TryBeginFrameDoesNotBlockWhenGpuBusy_Vulkan) {
    const ScenarioResult result = RunTryBeginFrameNonBlockingWhenGpuBusySmoke(TestBackend::Vulkan);
    if (result.TestStatus == ScenarioResult::Status::Skipped) {
        GTEST_SKIP() << result.Message;
    }
    if (result.TestStatus == ScenarioResult::Status::Failed) {
        FAIL() << result.Message;
    }
}

TEST(GpuRuntimeSmoke, WorkerThreadSchedulesRuntime_D3D12) {
    const ScenarioResult result = RunGpuRuntimeScheduledOnWorkerThreadSmoke(TestBackend::D3D12);
    if (result.TestStatus == ScenarioResult::Status::Skipped) {
        GTEST_SKIP() << result.Message;
    }
    if (result.TestStatus == ScenarioResult::Status::Failed) {
        FAIL() << result.Message;
    }
}

TEST(GpuRuntimeSmoke, WorkerThreadSchedulesRuntime_Vulkan) {
    const ScenarioResult result = RunGpuRuntimeScheduledOnWorkerThreadSmoke(TestBackend::Vulkan);
    if (result.TestStatus == ScenarioResult::Status::Skipped) {
        GTEST_SKIP() << result.Message;
    }
    if (result.TestStatus == ScenarioResult::Status::Failed) {
        FAIL() << result.Message;
    }
}

TEST(GpuRuntimeSmoke, MainThreadThenWorkerThreadSchedulesRuntime_D3D12) {
    const ScenarioResult result = RunMainThreadThenWorkerThreadSchedulingSmoke(TestBackend::D3D12);
    if (result.TestStatus == ScenarioResult::Status::Skipped) {
        GTEST_SKIP() << result.Message;
    }
    if (result.TestStatus == ScenarioResult::Status::Failed) {
        FAIL() << result.Message;
    }
}

TEST(GpuRuntimeSmoke, MainThreadThenWorkerThreadSchedulesRuntime_Vulkan) {
    const ScenarioResult result = RunMainThreadThenWorkerThreadSchedulingSmoke(TestBackend::Vulkan);
    if (result.TestStatus == ScenarioResult::Status::Skipped) {
        GTEST_SKIP() << result.Message;
    }
    if (result.TestStatus == ScenarioResult::Status::Failed) {
        FAIL() << result.Message;
    }
}
