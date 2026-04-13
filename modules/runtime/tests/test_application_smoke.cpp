#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/application.h>
#include <radray/window/native_window.h>

using namespace radray;
using namespace radray::render;

namespace {

constexpr uint32_t kInitialWidth = 640;
constexpr uint32_t kInitialHeight = 360;
constexpr uint32_t kBackBufferCount = 3;
constexpr uint32_t kFlightFrameCount = 2;
constexpr uint32_t kTargetRenderFrameCount = 6;
constexpr uint32_t kTargetRenderFrameCountAllowDrop = 2;
constexpr uint32_t kMaxUpdateCount = 1200;

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
    mutable std::mutex _mutex{};
    std::vector<std::string> _errors{};
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

bool ExpectNoCapturedErrors(const LogCollector& logs, std::string& reason) {
    const auto errors = logs.GetErrors();
    if (!errors.empty()) {
        reason = fmt::format("Captured render errors:\n{}", JoinErrors(errors));
        return false;
    }
    return true;
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
    desc.Title = "ApplicationSmokeProbe";
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
    desc.Title = "ApplicationSmokeProbe";
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
            instanceDesc.AppName = "ApplicationSmoke";
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
        default:
            reason = "Unsupported backend";
            return false;
    }
}

bool ProbeSurfaceFormatForBackend(RenderBackend backend, TextureFormat& format, std::string& reason) {
    LogCollector logs{};
    unique_ptr<GpuRuntime> runtime{};
    if (!CreateRuntimeForBackend(backend, &logs, runtime, reason)) {
        return false;
    }

    auto windowOpt = CreateTestWindow(kInitialWidth, kInitialHeight);
    if (!windowOpt.HasValue()) {
        reason = "Cannot create native window for this platform.";
        return false;
    }
    auto window = windowOpt.Release();
    const auto nativeHandler = window->GetNativeHandler();
    if (nativeHandler.Handle == nullptr) {
        reason = "Window native handle is null.";
        window->Destroy();
        return false;
    }

    const auto size = window->GetSize();
    if (size.X <= 0 || size.Y <= 0) {
        reason = "Window size is invalid before surface creation.";
        window->Destroy();
        return false;
    }

    for (size_t i = 0; i < kFallbackFormats.size(); ++i) {
        const auto errorBaseline = logs.GetErrorCount();
        try {
            GpuSurfaceDescriptor surfaceDesc{};
            surfaceDesc.NativeHandler = nativeHandler.Handle;
            surfaceDesc.Width = static_cast<uint32_t>(size.X);
            surfaceDesc.Height = static_cast<uint32_t>(size.Y);
            surfaceDesc.BackBufferCount = kBackBufferCount;
            surfaceDesc.FlightFrameCount = kFlightFrameCount;
            surfaceDesc.Format = kFallbackFormats[i];
            surfaceDesc.PresentMode = PresentMode::FIFO;
            surfaceDesc.QueueSlot = 0;

            auto surface = runtime->CreateSurface(surfaceDesc);
            if (surface != nullptr && surface->IsValid()) {
                format = kFallbackFormats[i];
                surface.reset();
                window->Destroy();
                return true;
            }
        } catch (...) {
        }

        if (i + 1 < kFallbackFormats.size()) {
            logs.Truncate(errorBaseline);
        }
    }

    window->Destroy();
    reason = "CreateSurface failed for all fallback formats.";
    return false;
}

NativeWindowCreateDescriptor MakeTestWindowDesc(std::string_view title, uint32_t width, uint32_t height) {
#if defined(_WIN32)
    Win32WindowCreateDescriptor desc{};
    desc.Title = title;
    desc.Width = static_cast<int32_t>(width);
    desc.Height = static_cast<int32_t>(height);
    desc.X = 120;
    desc.Y = 120;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return desc;
#elif defined(__APPLE__)
    CocoaWindowCreateDescriptor desc{};
    desc.Title = title;
    desc.Width = static_cast<int32_t>(width);
    desc.Height = static_cast<int32_t>(height);
    desc.X = 120;
    desc.Y = 120;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return desc;
#else
    RADRAY_UNUSED(title);
    RADRAY_UNUSED(width);
    RADRAY_UNUSED(height);
    return Win32WindowCreateDescriptor{};
#endif
}

class ApplicationSmokeApp final : public Application {
public:
    ApplicationSmokeApp(
        RenderBackend backend,
        TextureFormat surfaceFormat,
        bool allowFrameDrop,
        LogCollector* logs) noexcept
        : _backend(backend),
          _surfaceFormat(surfaceFormat),
          _allowFrameDropConfig(allowFrameDrop),
          _logs(logs),
          _targetRenderFrameCount(allowFrameDrop ? kTargetRenderFrameCountAllowDrop : kTargetRenderFrameCount) {}

    const std::string& GetFailureReason() const noexcept { return _failureReason; }
    uint32_t GetRenderedFrameCount() const noexcept { return _renderedFrameCount; }
    uint32_t GetTargetRenderFrameCount() const noexcept { return _targetRenderFrameCount; }
    size_t GetSeenBackBufferCount() const noexcept { return _seenBackBufferIndices.size(); }

protected:
    void OnInitialize() override {
        _allowFrameDrop = _allowFrameDropConfig;

        switch (_backend) {
            case RenderBackend::D3D12: {
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
                D3D12DeviceDescriptor desc{};
                desc.AdapterIndex = std::nullopt;
                desc.IsEnableDebugLayer = true;
                desc.IsEnableGpuBasedValid = true;
                desc.LogCallback = &LogCollector::Callback;
                desc.LogUserData = _logs;
                this->CreateGpuRuntime(DeviceDescriptor{desc}, std::nullopt);
                break;
#else
                throw AppException("D3D12 backend is not enabled for this build");
#endif
            }
            case RenderBackend::Vulkan: {
#if defined(RADRAY_ENABLE_VULKAN)
                VulkanInstanceDescriptor instanceDesc{};
                instanceDesc.AppName = "ApplicationSmoke";
                instanceDesc.AppVersion = 1;
                instanceDesc.EngineName = "RadRay";
                instanceDesc.EngineVersion = 1;
                instanceDesc.IsEnableDebugLayer = true;
                instanceDesc.IsEnableGpuBasedValid = false;
                instanceDesc.LogCallback = &LogCollector::Callback;
                instanceDesc.LogUserData = _logs;

                VulkanCommandQueueDescriptor queueDesc{};
                queueDesc.Type = QueueType::Direct;
                queueDesc.Count = 1;

                VulkanDeviceDescriptor deviceDesc{};
                deviceDesc.PhysicalDeviceIndex = std::nullopt;
                deviceDesc.Queues = std::span{&queueDesc, 1};
                this->CreateGpuRuntime(DeviceDescriptor{deviceDesc}, instanceDesc);
                break;
#else
                throw AppException("Vulkan backend is not enabled for this build");
#endif
            }
            default:
                throw AppException("Unsupported backend");
        }

        GpuSurfaceDescriptor surfaceDesc{};
        surfaceDesc.Width = kInitialWidth;
        surfaceDesc.Height = kInitialHeight;
        surfaceDesc.BackBufferCount = kBackBufferCount;
        surfaceDesc.FlightFrameCount = kFlightFrameCount;
        surfaceDesc.Format = _surfaceFormat;
        surfaceDesc.PresentMode = PresentMode::FIFO;
        surfaceDesc.QueueSlot = 0;

        const auto title = _allowFrameDropConfig ? "ApplicationSmokeDrop" : "ApplicationSmoke";
        _windowHandle = this->CreateWindow(MakeTestWindowDesc(title, kInitialWidth, kInitialHeight), surfaceDesc, true);
        auto& window = _windows.Get(_windowHandle);
        if (window._surface == nullptr || !window._surface->IsValid()) {
            throw AppException("CreateWindow returned an invalid surface");
        }

        const uint32_t backBufferCount = window._surface->_swapchain->GetBackBufferCount();
        if (backBufferCount == 0) {
            throw AppException("Swapchain back buffer count is zero");
        }

        _backBufferStates.assign(backBufferCount, TextureState::Undefined);
        _rtvs.resize(backBufferCount);
        _rtvTargets.assign(backBufferCount, nullptr);
    }

    void OnShutdown() override {
        _rtvTargets.clear();
        _rtvs.clear();
        Application::OnShutdown();
    }

    void OnUpdate() override {
        ++_updateCount;
        if (_updateCount > kMaxUpdateCount) {
            _failureReason = fmt::format(
                "Timed out waiting for rendered frames. rendered={} target={} allowFrameDrop={}",
                _renderedFrameCount,
                _targetRenderFrameCount,
                _allowFrameDropConfig);
            _exitRequested = true;
        }
    }

    void OnPrepareRender(AppWindowHandle window, uint32_t flightIndex) override {
        RADRAY_UNUSED(window);
        RADRAY_UNUSED(flightIndex);
    }

    void OnRender(AppWindowHandle window, GpuFrameContext* context, uint32_t flightIndex) override {
        RADRAY_UNUSED(window);
        RADRAY_UNUSED(flightIndex);
        if (context == nullptr) {
            throw AppException("OnRender received null frame context");
        }

        const uint32_t backBufferIndex = context->GetBackBufferIndex();
        if (backBufferIndex >= _backBufferStates.size()) {
            throw AppException(fmt::format("Invalid back buffer index {}", backBufferIndex));
        }

        auto* backBuffer = context->GetBackBuffer();
        if (backBuffer == nullptr) {
            throw AppException("Frame context returned null back buffer");
        }

        auto* rtv = this->EnsureBackBufferRtv(backBufferIndex, backBuffer);
        auto* cmd = context->CreateCommandBuffer();
        if (cmd == nullptr) {
            throw AppException("CreateCommandBuffer returned null");
        }

        cmd->Begin();
        {
            ResourceBarrierDescriptor toRenderTarget = BarrierTextureDescriptor{
                .Target = backBuffer,
                .Before = _backBufferStates[backBufferIndex],
                .After = TextureState::RenderTarget,
            };
            cmd->ResourceBarrier(std::span{&toRenderTarget, 1});
        }
        {
            ColorAttachment colorAttachment{
                .Target = rtv,
                .Load = LoadAction::Clear,
                .Store = StoreAction::Store,
                .ClearValue = _clearColor,
            };
            RenderPassDescriptor passDesc{};
            passDesc.ColorAttachments = std::span{&colorAttachment, 1};
            auto passOpt = cmd->BeginRenderPass(passDesc);
            if (!passOpt.HasValue()) {
                throw AppException("BeginRenderPass failed");
            }
            cmd->EndRenderPass(passOpt.Release());
        }
        {
            ResourceBarrierDescriptor toPresent = BarrierTextureDescriptor{
                .Target = backBuffer,
                .Before = TextureState::RenderTarget,
                .After = TextureState::Present,
            };
            cmd->ResourceBarrier(std::span{&toPresent, 1});
        }
        cmd->End();

        _backBufferStates[backBufferIndex] = TextureState::Present;
        _seenBackBufferIndices.emplace(backBufferIndex);
        ++_renderedFrameCount;
        if (_renderedFrameCount >= _targetRenderFrameCount) {
            _exitRequested = true;
        }
    }

private:
    TextureView* EnsureBackBufferRtv(uint32_t backBufferIndex, Texture* backBuffer) {
        if (backBufferIndex >= _rtvs.size()) {
            throw AppException("Back buffer index out of RTV cache range");
        }
        if (_rtvs[backBufferIndex] != nullptr && _rtvTargets[backBufferIndex] == backBuffer) {
            return _rtvs[backBufferIndex].get();
        }

        TextureViewDescriptor rtvDesc{};
        rtvDesc.Target = backBuffer;
        rtvDesc.Dim = TextureDimension::Dim2D;
        rtvDesc.Format = backBuffer->GetDesc().Format;
        rtvDesc.Range = SubresourceRange{0, 1, 0, 1};
        rtvDesc.Usage = TextureViewUsage::RenderTarget;
        auto rtvOpt = _gpu->GetDevice()->CreateTextureView(rtvDesc);
        if (!rtvOpt.HasValue()) {
            throw AppException("CreateTextureView for swapchain back buffer failed");
        }

        _rtvs[backBufferIndex] = rtvOpt.Release();
        _rtvTargets[backBufferIndex] = backBuffer;
        return _rtvs[backBufferIndex].get();
    }

private:
    RenderBackend _backend;
    TextureFormat _surfaceFormat{TextureFormat::UNKNOWN};
    bool _allowFrameDropConfig{false};
    LogCollector* _logs{nullptr};
    AppWindowHandle _windowHandle{};
    uint32_t _targetRenderFrameCount{0};
    uint32_t _updateCount{0};
    uint32_t _renderedFrameCount{0};
    vector<TextureState> _backBufferStates{};
    vector<unique_ptr<TextureView>> _rtvs{};
    vector<Texture*> _rtvTargets{};
    unordered_set<uint32_t> _seenBackBufferIndices{};
    string _failureReason{};
    ColorClearValue _clearColor{{0.2f, 0.4f, 0.8f, 1.0f}};
};

class ApplicationSmokeTest : public ::testing::TestWithParam<RenderBackend> {};

TEST_P(ApplicationSmokeTest, SingleThreadClearAndFrameDropToggleWorks) {
    TextureFormat surfaceFormat = TextureFormat::UNKNOWN;
    std::string reason{};
    if (!ProbeSurfaceFormatForBackend(GetParam(), surfaceFormat, reason)) {
        GTEST_SKIP() << reason;
    }

    for (const bool allowFrameDrop : {false, true}) {
        SCOPED_TRACE(fmt::format("AllowFrameDrop={}", allowFrameDrop));

        LogCollector logs{};
        ScopedGlobalLogCallback logScope{&logs};
        ApplicationSmokeApp app{GetParam(), surfaceFormat, allowFrameDrop, &logs};

        ASSERT_NO_THROW(app.Run(0, nullptr));
        ASSERT_TRUE(app.GetFailureReason().empty())
            << app.GetFailureReason();
        ASSERT_GE(app.GetRenderedFrameCount(), app.GetTargetRenderFrameCount())
            << "Application did not render enough frames.";
        if (allowFrameDrop) {
            ASSERT_GE(app.GetSeenBackBufferCount(), 1u)
                << "Frame-drop mode did not clear any back buffer.";
        } else {
            ASSERT_GE(app.GetSeenBackBufferCount(), 2u)
                << "Swapchain did not rotate back buffers as expected.";
        }
        ASSERT_TRUE(ExpectNoCapturedErrors(logs, reason))
            << reason;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Backends,
    ApplicationSmokeTest,
    ::testing::ValuesIn(GetEnabledRuntimeBackends()),
    [](const ::testing::TestParamInfo<RenderBackend>& info) {
        return std::string(BackendTestName(info.param));
    });

}  // namespace
