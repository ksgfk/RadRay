#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/application.h>

using namespace radray;
using namespace radray::render;

namespace {

using namespace std::chrono_literals;

constexpr uint32_t kWindowWidth = 480;
constexpr uint32_t kWindowHeight = 270;
constexpr uint32_t kBackBufferCount = 3;
constexpr uint32_t kFlightFrameCount = 2;
constexpr uint32_t kMailboxCount = 3;
constexpr uint32_t kSingleOnlyFrames = 120;
constexpr uint32_t kMultiOnlyFrames = 120;
constexpr uint32_t kSwitchPreFrames = 60;
constexpr uint32_t kSwitchPostFrames = 60;
constexpr std::chrono::seconds kScenarioTimeout{60};

constexpr std::array<TextureFormat, 2> kFallbackFormats = {
    TextureFormat::BGRA8_UNORM,
    TextureFormat::RGBA8_UNORM};
constexpr std::array<VulkanCommandQueueDescriptor, 1> kVulkanQueueDescs = {
    VulkanCommandQueueDescriptor{QueueType::Direct, 1}};

enum class FrameLoopScenario {
    SingleThreadOnly,
    MultiThreadOnly,
    SingleThreadToMultiThread,
    MultiThreadToSingleThread
};

enum class ExpectedThreadMode {
    SingleThread,
    MultiThread
};

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

        std::lock_guard<std::mutex> lock{self->_mutex};
        self->_errors.emplace_back(message);
    }

    vector<string> GetErrors() const {
        std::lock_guard<std::mutex> lock{_mutex};
        return _errors;
    }

    size_t GetErrorCount() const noexcept {
        std::lock_guard<std::mutex> lock{_mutex};
        return _errors.size();
    }

    void Truncate(size_t count) noexcept {
        std::lock_guard<std::mutex> lock{_mutex};
        if (_errors.size() > count) {
            _errors.resize(count);
        }
    }

private:
    mutable std::mutex _mutex;
    vector<string> _errors;
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

string JoinErrors(const vector<string>& errors, size_t maxCount = 8) {
    if (errors.empty()) {
        return {};
    }

    const size_t count = std::min(maxCount, errors.size());
    string result = fmt::format("{}", fmt::join(errors.begin(), errors.begin() + count, "\n"));
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

std::string_view ScenarioTestName(FrameLoopScenario scenario) noexcept {
    switch (scenario) {
        case FrameLoopScenario::SingleThreadOnly: return "single-thread only";
        case FrameLoopScenario::MultiThreadOnly: return "multi-thread only";
        case FrameLoopScenario::SingleThreadToMultiThread: return "single-thread to multi-thread";
        case FrameLoopScenario::MultiThreadToSingleThread: return "multi-thread to single-thread";
        default: return "unknown";
    }
}

vector<RenderBackend> GetEnabledRuntimeBackends() noexcept {
    vector<RenderBackend> backends{};
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
    backends.push_back(RenderBackend::D3D12);
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    backends.push_back(RenderBackend::Vulkan);
#endif
    return backends;
}

NativeWindowCreateDescriptor MakeWindowDesc(RenderBackend backend) {
#if defined(_WIN32)
    Win32WindowCreateDescriptor desc{};
    desc.Title = backend == RenderBackend::D3D12
                     ? "ApplicationFrameLoop-D3D12"
                     : "ApplicationFrameLoop-Vulkan";
    desc.Width = static_cast<int32_t>(kWindowWidth);
    desc.Height = static_cast<int32_t>(kWindowHeight);
    desc.X = 160;
    desc.Y = 160;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return desc;
#elif defined(__APPLE__)
    CocoaWindowCreateDescriptor desc{};
    desc.Title = backend == RenderBackend::D3D12
                     ? "ApplicationFrameLoop-D3D12"
                     : "ApplicationFrameLoop-Vulkan";
    desc.Width = static_cast<int32_t>(kWindowWidth);
    desc.Height = static_cast<int32_t>(kWindowHeight);
    desc.X = 160;
    desc.Y = 160;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return desc;
#else
    RADRAY_UNUSED(backend);
    return Win32WindowCreateDescriptor{};
#endif
}

DeviceDescriptor MakeDeviceDesc(RenderBackend backend, LogCollector* logs) {
    switch (backend) {
        case RenderBackend::D3D12: {
            D3D12DeviceDescriptor desc{};
            desc.AdapterIndex = std::nullopt;
            desc.IsEnableDebugLayer = true;
            desc.IsEnableGpuBasedValid = true;
            desc.LogCallback = &LogCollector::Callback;
            desc.LogUserData = logs;
            return desc;
        }
        case RenderBackend::Vulkan: {
            VulkanDeviceDescriptor desc{};
            desc.PhysicalDeviceIndex = std::nullopt;
            desc.Queues = kVulkanQueueDescs;
            return desc;
        }
        default:
            RADRAY_ABORT("unsupported application frame-loop backend");
            return MetalDeviceDescriptor{};
    }
}

std::optional<VulkanInstanceDescriptor> MakeVulkanInstanceDesc(RenderBackend backend, LogCollector* logs) {
    if (backend != RenderBackend::Vulkan) {
        return std::nullopt;
    }

    VulkanInstanceDescriptor desc{};
    desc.AppName = "ApplicationFrameLoop";
    desc.AppVersion = 1;
    desc.EngineName = "RadRay";
    desc.EngineVersion = 1;
    desc.IsEnableDebugLayer = true;
    desc.IsEnableGpuBasedValid = false;
    desc.LogCallback = &LogCollector::Callback;
    desc.LogUserData = logs;
    return desc;
}

ColorClearValue MakeGradientClearValue(uint64_t frameIndex) noexcept {
    const float t = static_cast<float>(frameIndex % 120) / 119.0f;
    return ColorClearValue{{{t, 1.0f - t, 0.20f + 0.50f * t, 1.0f}}};
}

struct MailboxRenderData {
    uint64_t FrameIndex{0};
    ColorClearValue ClearValue{};
    ExpectedThreadMode ExpectedMode{ExpectedThreadMode::SingleThread};
};

struct BackBufferViewCache {
    vector<unique_ptr<TextureView>> RenderTargetViews{};
    vector<Texture*> Targets{};
    vector<TextureState> States{};
    unordered_set<uint32_t> SeenBackBufferIndices{};
};

class ApplicationFrameLoopApp final : public Application {
public:
    ApplicationFrameLoopApp(
        RenderBackend backend,
        FrameLoopScenario scenario,
        LogCollector* logs) noexcept
        : _backend(backend),
          _scenario(scenario),
          _logs(logs) {}

    void OnInitialize() override {
        _mainThreadId = std::this_thread::get_id();
        _startTime = std::chrono::steady_clock::now();
        _prepareMode = StartsMultiThreaded() ? ExpectedThreadMode::MultiThread : ExpectedThreadMode::SingleThread;

        try {
            this->CreateGpuRuntime(MakeDeviceDesc(_backend, _logs), MakeVulkanInstanceDesc(_backend, _logs));
        } catch (const std::exception& ex) {
            SetSkipReason(fmt::format("GpuRuntime create failed for {}: {}", BackendTestName(_backend), ex.what()));
            _exitRequested = true;
            return;
        }

        _mailboxData.resize(kMailboxCount);
        string lastFailure = "CreateWindow failed";
        for (size_t i = 0; i < kFallbackFormats.size(); ++i) {
            const size_t errorBaseline = _logs != nullptr ? _logs->GetErrorCount() : 0;
            try {
                GpuSurfaceDescriptor surfaceDesc{};
                surfaceDesc.Width = kWindowWidth;
                surfaceDesc.Height = kWindowHeight;
                surfaceDesc.BackBufferCount = kBackBufferCount;
                surfaceDesc.FlightFrameCount = kFlightFrameCount;
                surfaceDesc.Format = kFallbackFormats[i];
                surfaceDesc.PresentMode = PresentMode::FIFO;
                surfaceDesc.QueueSlot = 0;

                _window = this->CreateWindow(MakeWindowDesc(_backend), surfaceDesc, true, kMailboxCount);
                _surfaceFormat = kFallbackFormats[i];
                break;
            } catch (const std::exception& ex) {
                lastFailure = fmt::format("CreateWindow failed for {}: {}", kFallbackFormats[i], ex.what());
            }

            if (_logs != nullptr && i + 1 < kFallbackFormats.size()) {
                _logs->Truncate(errorBaseline);
            }
        }

        if (!_window.IsValid()) {
            SetSkipReason(lastFailure);
            _exitRequested = true;
            return;
        }

        this->RequestMultiThreaded(StartsMultiThreaded());
    }

    void OnShutdown() override {
        {
            std::lock_guard<std::mutex> lock{_renderDataMutex};
            _seenBackBufferCount.store(_backBuffers.SeenBackBufferIndices.size());
            _backBuffers = {};
            _mailboxData.clear();
        }
        Application::OnShutdown();
    }

    void OnUpdate() override {
        if (HasFailure() || HasTimedOut()) {
            _exitRequested = true;
            return;
        }

        switch (_scenario) {
            case FrameLoopScenario::SingleThreadOnly:
                _prepareMode = ExpectedThreadMode::SingleThread;
                if (_singleThreadRenderCount.load() >= kSingleOnlyFrames) {
                    _exitRequested = true;
                }
                break;
            case FrameLoopScenario::MultiThreadOnly:
                _prepareMode = ExpectedThreadMode::MultiThread;
                if (_multiThreadRenderCount.load() >= kMultiOnlyFrames) {
                    _exitRequested = true;
                }
                break;
            case FrameLoopScenario::SingleThreadToMultiThread:
                UpdateSwitchScenario(
                    ExpectedThreadMode::SingleThread,
                    ExpectedThreadMode::MultiThread,
                    _singleThreadRenderCount,
                    _multiThreadRenderCount);
                break;
            case FrameLoopScenario::MultiThreadToSingleThread:
                UpdateSwitchScenario(
                    ExpectedThreadMode::MultiThread,
                    ExpectedThreadMode::SingleThread,
                    _multiThreadRenderCount,
                    _singleThreadRenderCount);
                break;
            default:
                SetFailure("Unknown frame-loop scenario.");
                _exitRequested = true;
                break;
        }

        if (_prepareMode == ExpectedThreadMode::MultiThread && !_exitRequested.load()) {
            std::this_thread::sleep_for(1ms);
        }
    }

    void OnPrepareRender(AppWindowHandle window, uint32_t mailboxSlot) override {
        if (!ValidateWindow(window)) {
            return;
        }
        if (mailboxSlot >= _mailboxData.size()) {
            SetFailure(fmt::format("OnPrepareRender received invalid mailbox slot {}.", mailboxSlot));
            return;
        }

        const uint64_t frameIndex = _prepareCount.fetch_add(1);
        std::lock_guard<std::mutex> lock{_renderDataMutex};
        _mailboxData[mailboxSlot] = MailboxRenderData{
            frameIndex,
            MakeGradientClearValue(frameIndex),
            _prepareMode};
    }

    void OnRender(AppWindowHandle window, GpuFrameContext* context, uint32_t mailboxSlot) override {
        if (!ValidateWindow(window)) {
            return;
        }
        if (context == nullptr) {
            SetFailure("OnRender received null GpuFrameContext.");
            return;
        }

        MailboxRenderData renderData{};
        {
            std::lock_guard<std::mutex> lock{_renderDataMutex};
            if (mailboxSlot >= _mailboxData.size()) {
                SetFailure(fmt::format("OnRender received invalid mailbox slot {}.", mailboxSlot));
                return;
            }
            renderData = _mailboxData[mailboxSlot];
        }

        const bool onMainThread = std::this_thread::get_id() == _mainThreadId;
        if (renderData.ExpectedMode == ExpectedThreadMode::SingleThread && !onMainThread) {
            SetFailure("A single-thread frame was rendered outside the main thread.");
            return;
        }
        if (renderData.ExpectedMode == ExpectedThreadMode::MultiThread && onMainThread) {
            SetFailure("A multi-thread frame was rendered on the main thread.");
            return;
        }

        if (!RecordClearBackBuffer(*context, renderData.ClearValue)) {
            return;
        }

        _renderCount.fetch_add(1);
        if (renderData.ExpectedMode == ExpectedThreadMode::SingleThread) {
            _singleThreadRenderCount.fetch_add(1);
        } else {
            _multiThreadRenderCount.fetch_add(1);
        }
    }

    string GetFailure() const {
        std::lock_guard<std::mutex> lock{_messageMutex};
        return _failure;
    }

    string GetSkipReason() const {
        std::lock_guard<std::mutex> lock{_messageMutex};
        return _skipReason;
    }

    uint32_t SingleThreadRenderCount() const noexcept {
        return _singleThreadRenderCount.load();
    }

    uint32_t MultiThreadRenderCount() const noexcept {
        return _multiThreadRenderCount.load();
    }

    uint32_t RenderCount() const noexcept {
        return _renderCount.load();
    }

    size_t SeenBackBufferCount() const {
        return _seenBackBufferCount.load();
    }

    TextureFormat SurfaceFormat() const noexcept {
        return _surfaceFormat;
    }

private:
    bool StartsMultiThreaded() const noexcept {
        return _scenario == FrameLoopScenario::MultiThreadOnly ||
               _scenario == FrameLoopScenario::MultiThreadToSingleThread;
    }

    void UpdateSwitchScenario(
        ExpectedThreadMode fromMode,
        ExpectedThreadMode toMode,
        const std::atomic<uint32_t>& fromCount,
        const std::atomic<uint32_t>& toCount) {
        if (!_switched) {
            _prepareMode = fromMode;
            if (fromCount.load() >= kSwitchPreFrames) {
                this->RequestMultiThreaded(toMode == ExpectedThreadMode::MultiThread);
                _prepareMode = toMode;
                _switched = true;
            }
            return;
        }

        _prepareMode = toMode;
        if (toCount.load() >= kSwitchPostFrames) {
            _exitRequested = true;
        }
    }

    bool HasTimedOut() {
        if (std::chrono::steady_clock::now() - _startTime <= kScenarioTimeout) {
            return false;
        }

        SetFailure(fmt::format(
            "{} timed out after {} rendered frames (single={}, multi={}).",
            ScenarioTestName(_scenario),
            _renderCount.load(),
            _singleThreadRenderCount.load(),
            _multiThreadRenderCount.load()));
        return true;
    }

    bool ValidateWindow(AppWindowHandle window) {
        if (window.Id == _window.Id) {
            return true;
        }

        SetFailure(fmt::format("Unexpected window handle {}.", window.Id));
        return false;
    }

    bool RecordClearBackBuffer(GpuFrameContext& context, const ColorClearValue& clearValue) {
        if (_gpu == nullptr || _gpu->GetDevice() == nullptr) {
            SetFailure("GpuRuntime or device is null during OnRender.");
            return false;
        }

        auto* backBuffer = context.GetBackBuffer();
        if (backBuffer == nullptr) {
            SetFailure("Frame context returned null back buffer.");
            return false;
        }

        const uint32_t backBufferIndex = context.GetBackBufferIndex();
        std::lock_guard<std::mutex> lock{_renderDataMutex};
        EnsureBackBufferStorage(backBufferIndex);

        if (_backBuffers.RenderTargetViews[backBufferIndex] == nullptr ||
            _backBuffers.Targets[backBufferIndex] != backBuffer) {
            TextureViewDescriptor rtvDesc{};
            rtvDesc.Target = backBuffer;
            rtvDesc.Dim = TextureDimension::Dim2D;
            rtvDesc.Format = backBuffer->GetDesc().Format;
            rtvDesc.Range = SubresourceRange{0, 1, 0, 1};
            rtvDesc.Usage = TextureViewUsage::RenderTarget;

            auto rtvOpt = _gpu->GetDevice()->CreateTextureView(rtvDesc);
            if (!rtvOpt.HasValue()) {
                SetFailure("CreateTextureView for application backbuffer failed.");
                return false;
            }
            _backBuffers.RenderTargetViews[backBufferIndex] = rtvOpt.Release();
            _backBuffers.Targets[backBufferIndex] = backBuffer;
        }

        auto* cmd = context.CreateCommandBuffer();
        if (cmd == nullptr) {
            SetFailure("CreateCommandBuffer returned null during application frame-loop test.");
            return false;
        }

        cmd->Begin();
        ResourceBarrierDescriptor toRenderTarget = BarrierTextureDescriptor{
            .Target = backBuffer,
            .Before = _backBuffers.States[backBufferIndex],
            .After = TextureState::RenderTarget,
        };
        cmd->ResourceBarrier(std::span{&toRenderTarget, 1});

        ColorAttachment colorAttachment{};
        colorAttachment.Target = _backBuffers.RenderTargetViews[backBufferIndex].get();
        colorAttachment.Load = LoadAction::Clear;
        colorAttachment.Store = StoreAction::Store;
        colorAttachment.ClearValue = clearValue;

        RenderPassDescriptor passDesc{};
        passDesc.ColorAttachments = std::span{&colorAttachment, 1};
        passDesc.Name = "application-frame-loop-clear";

        auto passOpt = cmd->BeginRenderPass(passDesc);
        if (!passOpt.HasValue()) {
            ResourceBarrierDescriptor toPresent = BarrierTextureDescriptor{
                .Target = backBuffer,
                .Before = TextureState::RenderTarget,
                .After = TextureState::Present,
            };
            cmd->ResourceBarrier(std::span{&toPresent, 1});
            cmd->End();
            SetFailure("BeginRenderPass failed during application frame-loop test.");
            return false;
        }
        cmd->EndRenderPass(passOpt.Release());

        ResourceBarrierDescriptor toPresent = BarrierTextureDescriptor{
            .Target = backBuffer,
            .Before = TextureState::RenderTarget,
            .After = TextureState::Present,
        };
        cmd->ResourceBarrier(std::span{&toPresent, 1});
        cmd->End();

        _backBuffers.States[backBufferIndex] = TextureState::Present;
        _backBuffers.SeenBackBufferIndices.emplace(backBufferIndex);
        _seenBackBufferCount.store(_backBuffers.SeenBackBufferIndices.size());
        return true;
    }

    void EnsureBackBufferStorage(uint32_t backBufferIndex) {
        const size_t requiredSize = static_cast<size_t>(backBufferIndex) + 1;
        if (_backBuffers.RenderTargetViews.size() < requiredSize) {
            _backBuffers.RenderTargetViews.resize(requiredSize);
            _backBuffers.Targets.resize(requiredSize, nullptr);
            _backBuffers.States.resize(requiredSize, TextureState::Undefined);
        }
    }

    bool HasFailure() const {
        std::lock_guard<std::mutex> lock{_messageMutex};
        return !_failure.empty();
    }

    void SetFailure(string message) {
        std::lock_guard<std::mutex> lock{_messageMutex};
        if (_failure.empty()) {
            _failure = std::move(message);
        }
    }

    void SetSkipReason(string message) {
        std::lock_guard<std::mutex> lock{_messageMutex};
        if (_skipReason.empty()) {
            _skipReason = std::move(message);
        }
    }

    RenderBackend _backend{RenderBackend::D3D12};
    FrameLoopScenario _scenario{FrameLoopScenario::SingleThreadOnly};
    LogCollector* _logs{nullptr};
    AppWindowHandle _window{};
    TextureFormat _surfaceFormat{TextureFormat::UNKNOWN};
    std::thread::id _mainThreadId{};
    std::chrono::steady_clock::time_point _startTime{};
    ExpectedThreadMode _prepareMode{ExpectedThreadMode::SingleThread};
    bool _switched{false};

    mutable std::mutex _messageMutex;
    string _failure{};
    string _skipReason{};

    mutable std::mutex _renderDataMutex;
    vector<MailboxRenderData> _mailboxData{};
    BackBufferViewCache _backBuffers{};

    std::atomic<uint32_t> _prepareCount{0};
    std::atomic<uint32_t> _renderCount{0};
    std::atomic<uint32_t> _singleThreadRenderCount{0};
    std::atomic<uint32_t> _multiThreadRenderCount{0};
    std::atomic<size_t> _seenBackBufferCount{0};
};

class ApplicationFrameLoopSmokeTest : public ::testing::TestWithParam<RenderBackend> {
protected:
    void SetUp() override {
        _logScope = make_unique<ScopedGlobalLogCallback>(&_logs);
    }

    void TearDown() override {
        _logScope.reset();
    }

    void RunScenario(FrameLoopScenario scenario) {
        ApplicationFrameLoopApp app{GetParam(), scenario, &_logs};
        const int32_t exitCode = app.Run(0, nullptr);
        const string skipReason = app.GetSkipReason();
        if (!skipReason.empty()) {
            GTEST_SKIP() << skipReason;
        }

        ASSERT_EQ(exitCode, 0)
            << app.GetFailure();
        EXPECT_TRUE(app.GetFailure().empty())
            << app.GetFailure();

        switch (scenario) {
            case FrameLoopScenario::SingleThreadOnly:
                EXPECT_GE(app.SingleThreadRenderCount(), kSingleOnlyFrames);
                EXPECT_EQ(app.MultiThreadRenderCount(), 0u);
                break;
            case FrameLoopScenario::MultiThreadOnly:
                EXPECT_EQ(app.SingleThreadRenderCount(), 0u);
                EXPECT_GE(app.MultiThreadRenderCount(), kMultiOnlyFrames);
                break;
            case FrameLoopScenario::SingleThreadToMultiThread:
            case FrameLoopScenario::MultiThreadToSingleThread:
                EXPECT_GE(app.SingleThreadRenderCount(), kSwitchPreFrames);
                EXPECT_GE(app.MultiThreadRenderCount(), kSwitchPostFrames);
                break;
            default:
                FAIL() << "Unknown scenario.";
                break;
        }

        EXPECT_EQ(app.RenderCount(), app.SingleThreadRenderCount() + app.MultiThreadRenderCount());
        EXPECT_GE(app.SeenBackBufferCount(), 2u)
            << "Application frame loop did not rotate swapchain backbuffers for "
            << BackendTestName(GetParam()) << " using " << fmt::format("{}", app.SurfaceFormat());

        const auto errors = _logs.GetErrors();
        EXPECT_TRUE(errors.empty())
            << "Captured render errors:\n" << JoinErrors(errors);
    }

    LogCollector _logs;
    unique_ptr<ScopedGlobalLogCallback> _logScope;
};

TEST_P(ApplicationFrameLoopSmokeTest, RunsSingleThreadOnlyFrameLoop) {
    RunScenario(FrameLoopScenario::SingleThreadOnly);
}

TEST_P(ApplicationFrameLoopSmokeTest, RunsMultiThreadOnlyFrameLoop) {
    RunScenario(FrameLoopScenario::MultiThreadOnly);
}

TEST_P(ApplicationFrameLoopSmokeTest, SwitchesFromSingleThreadToMultiThread) {
    RunScenario(FrameLoopScenario::SingleThreadToMultiThread);
}

TEST_P(ApplicationFrameLoopSmokeTest, SwitchesFromMultiThreadToSingleThread) {
    RunScenario(FrameLoopScenario::MultiThreadToSingleThread);
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    ApplicationFrameLoopSmokeTest,
    ::testing::ValuesIn(GetEnabledRuntimeBackends()),
    [](const ::testing::TestParamInfo<RenderBackend>& info) {
        return string{BackendTestName(info.param)};
    });

}  // namespace
