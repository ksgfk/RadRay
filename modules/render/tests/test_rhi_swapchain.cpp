#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>
#include <fmt/ranges.h>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/window/native_window.h>

using namespace radray;
using namespace radray::render;

namespace {

constexpr uint32_t kInitialWidth = 640;
constexpr uint32_t kInitialHeight = 360;
constexpr uint32_t kResizedWidth = 800;
constexpr uint32_t kResizedHeight = 450;
constexpr uint32_t kBackBufferCount = 3;
constexpr uint32_t kInFlightFrameCount = 2;
constexpr uint32_t kSmokeFrameCount = 64;
constexpr uint32_t kPreResizeFrameCount = 16;
constexpr uint32_t kPostResizeFrameCount = 32;
constexpr uint32_t kAcquireRetryMax = 120;

class LogCollector {
public:
    static void Callback(LogLevel level, std::string_view message, void* userData) {
        auto* self = static_cast<LogCollector*>(userData);
        if (self == nullptr) {
            return;
        }
        if (level == LogLevel::Err || level == LogLevel::Critical) {
            std::lock_guard<std::mutex> lock(self->_mutex);
            self->_errors.emplace_back(message);
        }
    }

    std::vector<std::string> GetErrors() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _errors;
    }

private:
    mutable std::mutex _mutex;
    std::vector<std::string> _errors;
};

class ScopedVulkanInstance {
public:
    ~ScopedVulkanInstance() noexcept {
        this->Reset();
    }

    unique_ptr<InstanceVulkan>& Ref() noexcept {
        return _instance;
    }

    void Reset() noexcept {
        if (_instance != nullptr) {
            DestroyVulkanInstance(std::move(_instance));
        }
    }

private:
    unique_ptr<InstanceVulkan> _instance{};
};

struct QueueWaitGuard {
    CommandQueue* Queue{nullptr};
    ~QueueWaitGuard() noexcept {
        if (Queue != nullptr) {
            Queue->Wait();
        }
    }
};

struct SwapChainRuntime {
    unique_ptr<SwapChain> Swapchain{};
    TextureFormat Format{TextureFormat::UNKNOWN};
    std::vector<TextureState> BackBufferStates{};
    std::vector<unique_ptr<TextureView>> Rtvs{};
    std::vector<Texture*> RtvTargets{};
};

std::string JoinErrors(const std::vector<std::string>& errors, size_t maxCount = 8) {
    const size_t count = std::min(maxCount, errors.size());
    std::string result = fmt::format("{}", fmt::join(errors.begin(), errors.begin() + count, "\n"));
    if (errors.size() > count) {
        result += fmt::format("\n...({} more)", errors.size() - count);
    }
    return result;
}

Nullable<unique_ptr<NativeWindow>> CreateTestWindow(uint32_t width, uint32_t height) noexcept {
#if defined(_WIN32)
    Win32WindowCreateDescriptor desc{};
    desc.Title = "RHISwapchainSmoke";
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
    desc.Title = "RHISwapchainSmoke";
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

bool CreateDeviceForBackend(
    RenderBackend backend,
    LogCollector* logs,
    ScopedVulkanInstance& vkInstance,
    shared_ptr<Device>& device,
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
            auto devOpt = CreateDevice(desc);
            if (!devOpt.HasValue()) {
                reason = "CreateDevice(D3D12) failed";
                return false;
            }
            device = devOpt.Release();
            return true;
#else
            RADRAY_UNUSED(logs);
            reason = "D3D12 backend is not enabled for this build";
            return false;
#endif
        }
        case RenderBackend::Vulkan: {
#if defined(RADRAY_ENABLE_VULKAN)
            VulkanInstanceDescriptor insDesc{};
            insDesc.AppName = "RHISwapchainSmoke";
            insDesc.AppVersion = 1;
            insDesc.EngineName = "RadRay";
            insDesc.EngineVersion = 1;
            insDesc.IsEnableDebugLayer = true;
            insDesc.IsEnableGpuBasedValid = false;
            insDesc.LogCallback = &LogCollector::Callback;
            insDesc.LogUserData = logs;
            auto insOpt = CreateVulkanInstance(insDesc);
            if (!insOpt.HasValue()) {
                reason = "CreateVulkanInstance failed";
                return false;
            }
            vkInstance.Ref() = insOpt.Release();

            VulkanCommandQueueDescriptor queueDesc{};
            queueDesc.Type = QueueType::Direct;
            queueDesc.Count = 1;
            VulkanDeviceDescriptor devDesc{};
            devDesc.PhysicalDeviceIndex = std::nullopt;
            devDesc.Queues = std::span{&queueDesc, 1};
            auto devOpt = CreateDevice(devDesc);
            if (!devOpt.HasValue()) {
                reason = "CreateDevice(Vulkan) failed";
                vkInstance.Reset();
                return false;
            }
            device = devOpt.Release();
            return true;
#else
            RADRAY_UNUSED(logs);
            reason = "Vulkan backend is not enabled for this build";
            return false;
#endif
        }
        default: {
            reason = "Backend is not supported by this test";
            return false;
        }
    }
}

bool CreateSwapChainRuntime(
    Device* device,
    CommandQueue* queue,
    const WindowNativeHandler& nativeHandler,
    uint32_t width,
    uint32_t height,
    SwapChainRuntime& runtime,
    std::string& reason) {
    if (nativeHandler.Handle == nullptr) {
        reason = "window native handle is null";
        return false;
    }

    constexpr std::array<TextureFormat, 2> formats = {
        TextureFormat::BGRA8_UNORM,
        TextureFormat::RGBA8_UNORM};
    for (TextureFormat format : formats) {
        SwapChainDescriptor scDesc{};
        scDesc.PresentQueue = queue;
        scDesc.NativeHandler = nativeHandler.Handle;
        scDesc.Width = width;
        scDesc.Height = height;
        scDesc.BackBufferCount = kBackBufferCount;
        scDesc.FlightFrameCount = kInFlightFrameCount;
        scDesc.Format = format;
        scDesc.PresentMode = PresentMode::FIFO;
        auto swapchainOpt = device->CreateSwapChain(scDesc);
        if (!swapchainOpt.HasValue()) {
            continue;
        }
        runtime.Swapchain = swapchainOpt.Release();
        runtime.Format = format;
        const uint32_t count = runtime.Swapchain->GetBackBufferCount();
        if (count == 0) {
            reason = "swapchain back buffer count is zero";
            runtime.Swapchain.reset();
            return false;
        }
        runtime.BackBufferStates.assign(count, TextureState::Undefined);
        runtime.Rtvs.resize(count);
        runtime.RtvTargets.assign(count, nullptr);
        return true;
    }

    reason = "CreateSwapChain failed for BGRA8_UNORM and RGBA8_UNORM";
    return false;
}

bool EnsureBackBufferRtv(
    Device* device,
    SwapChainRuntime& runtime,
    uint32_t backBufferIndex,
    Texture* backBuffer,
    std::string& reason) {
    if (backBufferIndex >= runtime.Rtvs.size()) {
        reason = "back buffer index out of range";
        return false;
    }
    if (runtime.Rtvs[backBufferIndex] != nullptr &&
        runtime.RtvTargets[backBufferIndex] == backBuffer) {
        return true;
    }

    TextureViewDescriptor rtvDesc{};
    rtvDesc.Target = backBuffer;
    rtvDesc.Dim = TextureDimension::Dim2D;
    rtvDesc.Format = backBuffer->GetDesc().Format;
    rtvDesc.Range = SubresourceRange{0, 1, 0, 1};
    rtvDesc.Usage = TextureViewUsage::RenderTarget;
    auto rtvOpt = device->CreateTextureView(rtvDesc);
    if (!rtvOpt.HasValue()) {
        reason = "CreateTextureView for swapchain back buffer failed";
        return false;
    }
    runtime.Rtvs[backBufferIndex] = rtvOpt.Release();
    runtime.RtvTargets[backBufferIndex] = backBuffer;
    return true;
}

bool RenderFrames(
    Device* device,
    NativeWindow* window,
    CommandQueue* queue,
    SwapChainRuntime& runtime,
    std::vector<unique_ptr<CommandBuffer>>& cmdBuffers,
    std::vector<unique_ptr<Fence>>& fences,
    std::vector<uint64_t>& expectedCompletedValues,
    uint64_t frameStart,
    uint32_t frameCount,
    std::unordered_set<uint32_t>& seenBackBufferIndices,
    std::string& reason) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        window->DispatchEvents();
        if (window->ShouldClose()) {
            reason = "window closed during test";
            return false;
        }

        const uint64_t frameNumber = frameStart + i;
        const uint32_t slot = static_cast<uint32_t>(frameNumber % kInFlightFrameCount);
        auto* fence = fences[slot].get();
        auto* cmd = cmdBuffers[slot].get();

        fence->Wait();
        const uint64_t completedValue = fence->GetCompletedValue();
        if (completedValue < expectedCompletedValues[slot]) {
            reason = fmt::format(
                "fence completed value is stale (slot={}, completed={}, expected={})",
                slot, completedValue, expectedCompletedValues[slot]);
            return false;
        }

        Texture* backBuffer = nullptr;
        SwapChainSyncObject* waitToDrawSync = nullptr;
        SwapChainSyncObject* readyToPresentSync = nullptr;
        for (uint32_t retry = 0; retry < kAcquireRetryMax; ++retry) {
            auto acquired = runtime.Swapchain->AcquireNext();
            if (acquired.BackBuffer.HasValue()) {
                backBuffer = acquired.BackBuffer.Get();
                waitToDrawSync = acquired.WaitToDraw;
                readyToPresentSync = acquired.ReadyToPresent;
                break;
            }
            window->DispatchEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (backBuffer == nullptr) {
            reason = "AcquireNext failed after retries";
            return false;
        }

        const uint32_t backBufferIndex = runtime.Swapchain->GetCurrentBackBufferIndex();
        if (backBufferIndex >= runtime.BackBufferStates.size()) {
            reason = fmt::format("invalid back buffer index {}", backBufferIndex);
            return false;
        }
        if (!EnsureBackBufferRtv(device, runtime, backBufferIndex, backBuffer, reason)) {
            return false;
        }
        seenBackBufferIndices.emplace(backBufferIndex);

        cmd->Begin();
        {
            ResourceBarrierDescriptor toRenderTarget = BarrierTextureDescriptor{
                backBuffer,
                runtime.BackBufferStates[backBufferIndex],
                TextureState::RenderTarget};
            cmd->ResourceBarrier(std::span{&toRenderTarget, 1});
        }
        {
            const float t = static_cast<float>(frameNumber % 255) / 255.0f;
            ColorClearValue clear{{{t, 1.0f - t, 0.25f, 1.0f}}};
            ColorAttachment colorAttachment{
                runtime.Rtvs[backBufferIndex].get(),
                LoadAction::Clear,
                StoreAction::Store,
                clear};
            RenderPassDescriptor passDesc{};
            passDesc.ColorAttachments = std::span{&colorAttachment, 1};
            auto passOpt = cmd->BeginRenderPass(passDesc);
            if (!passOpt.HasValue()) {
                reason = "BeginRenderPass failed";
                return false;
            }
            cmd->EndRenderPass(passOpt.Release());
        }
        {
            ResourceBarrierDescriptor toPresent = BarrierTextureDescriptor{
                backBuffer,
                TextureState::RenderTarget,
                TextureState::Present};
            cmd->ResourceBarrier(std::span{&toPresent, 1});
        }
        cmd->End();

        CommandBuffer* submitCmds[] = {cmd};
        Fence* signalFences[] = {fence};
        expectedCompletedValues[slot]++;
        uint64_t signalValues[] = {expectedCompletedValues[slot]};
        CommandQueueSubmitDescriptor submitDesc{};
        submitDesc.CmdBuffers = submitCmds;
        submitDesc.SignalFences = signalFences;
        submitDesc.SignalValues = signalValues;
        submitDesc.WaitToExecute = std::span{&waitToDrawSync, 1};
        submitDesc.ReadyToPresent = std::span{&readyToPresentSync, 1};
        queue->Submit(submitDesc);
        runtime.Swapchain->Present(readyToPresentSync);

        runtime.BackBufferStates[backBufferIndex] = TextureState::Present;
    }
    return true;
}

void RunSwapChainScenario(RenderBackend backend, bool withResize) {
    auto windowOpt = CreateTestWindow(kInitialWidth, kInitialHeight);
    if (!windowOpt.HasValue()) {
        GTEST_SKIP() << "Cannot create native window for this platform.";
    }
    auto window = windowOpt.Release();

    LogCollector logs;
    ScopedVulkanInstance vkInstance;
    shared_ptr<Device> device{};
    std::string reason;
    if (!CreateDeviceForBackend(backend, &logs, vkInstance, device, reason)) {
        GTEST_SKIP() << reason;
    }

    auto queueOpt = device->GetCommandQueue(QueueType::Direct, 0);
    if (!queueOpt.HasValue()) {
        GTEST_SKIP() << "Cannot get direct queue from device.";
    }
    auto* queue = queueOpt.Get();
    QueueWaitGuard queueWait{queue};

    std::vector<unique_ptr<CommandBuffer>> cmdBuffers{};
    std::vector<unique_ptr<Fence>> fences{};
    cmdBuffers.reserve(kInFlightFrameCount);
    fences.reserve(kInFlightFrameCount);
    for (uint32_t i = 0; i < kInFlightFrameCount; ++i) {
        auto cmdOpt = device->CreateCommandBuffer(queue);
        if (!cmdOpt.HasValue()) {
            GTEST_SKIP() << "CreateCommandBuffer failed for slot " << i;
        }
        cmdBuffers.emplace_back(cmdOpt.Release());

        auto fenceOpt = device->CreateFence();
        if (!fenceOpt.HasValue()) {
            GTEST_SKIP() << "CreateFence failed for slot " << i;
        }
        fences.emplace_back(fenceOpt.Release());
    }

    auto initialSize = window->GetSize();
    if (initialSize.X <= 0 || initialSize.Y <= 0) {
        GTEST_SKIP() << "Window size is invalid before swapchain creation.";
    }

    SwapChainRuntime runtime{};
    if (!CreateSwapChainRuntime(
            device.get(),
            queue,
            window->GetNativeHandler(),
            static_cast<uint32_t>(initialSize.X),
            static_cast<uint32_t>(initialSize.Y),
            runtime,
            reason)) {
        GTEST_SKIP() << reason;
    }

    std::vector<uint64_t> expectedCompletedValues(kInFlightFrameCount, 0);
    std::unordered_set<uint32_t> seenBackBufferIndices{};

    if (!RenderFrames(
            device.get(),
            window.get(),
            queue,
            runtime,
            cmdBuffers,
            fences,
            expectedCompletedValues,
            0,
            withResize ? kPreResizeFrameCount : kSmokeFrameCount,
            seenBackBufferIndices,
            reason)) {
        FAIL() << reason;
    }

    if (withResize) {
        window->SetSize(static_cast<int>(kResizedWidth), static_cast<int>(kResizedHeight));
        for (uint32_t i = 0; i < 32; ++i) {
            window->DispatchEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        queue->Wait();

        runtime.Swapchain.reset();
        runtime.BackBufferStates.clear();
        runtime.Rtvs.clear();
        runtime.RtvTargets.clear();
        seenBackBufferIndices.clear();

        auto resizedSize = window->GetSize();
        if (resizedSize.X <= 0 || resizedSize.Y <= 0) {
            resizedSize = WindowVec2i{
                static_cast<int32_t>(kResizedWidth),
                static_cast<int32_t>(kResizedHeight)};
        }
        if (!CreateSwapChainRuntime(
                device.get(),
                queue,
                window->GetNativeHandler(),
                static_cast<uint32_t>(resizedSize.X),
                static_cast<uint32_t>(resizedSize.Y),
                runtime,
                reason)) {
            FAIL() << reason;
        }

        if (!RenderFrames(
                device.get(),
                window.get(),
                queue,
                runtime,
                cmdBuffers,
                fences,
                expectedCompletedValues,
                kPreResizeFrameCount,
                kPostResizeFrameCount,
                seenBackBufferIndices,
                reason)) {
            FAIL() << reason;
        }
    }

    queue->Wait();
    for (uint32_t i = 0; i < kInFlightFrameCount; ++i) {
        auto* fence = fences[i].get();
        fence->Wait();
        ASSERT_GE(fence->GetCompletedValue(), expectedCompletedValues[i])
            << "Fence completed value mismatch at slot " << i;
    }

    ASSERT_GE(seenBackBufferIndices.size(), 2u)
        << "Swapchain did not rotate back buffers as expected.";

    const auto errors = logs.GetErrors();
    ASSERT_TRUE(errors.empty()) << "Captured render errors:\n"
                                << JoinErrors(errors);

    runtime.Swapchain.reset();
    window->Destroy();
}

}  // namespace

TEST(RHISwapchain, Smoke_D3D12) {
    RunSwapChainScenario(RenderBackend::D3D12, false);
}

TEST(RHISwapchain, Smoke_Vulkan) {
    RunSwapChainScenario(RenderBackend::Vulkan, false);
}

TEST(RHISwapchain, RecreateAfterResize_D3D12) {
    RunSwapChainScenario(RenderBackend::D3D12, true);
}

TEST(RHISwapchain, RecreateAfterResize_Vulkan) {
    RunSwapChainScenario(RenderBackend::Vulkan, true);
}
