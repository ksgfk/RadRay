#include <array>
#include <mutex>

#include <gtest/gtest.h>
#include <fmt/format.h>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/application.h>
#include <radray/window/native_window.h>

using namespace radray;
using namespace radray::render;

namespace {

constexpr uint32_t kFrameCount = 18;
constexpr uint32_t kInitialWidth = 640;
constexpr uint32_t kInitialHeight = 360;

constexpr std::array<ColorClearValue, 3> kClearColors = {{
    {{{1.0f, 0.0f, 0.0f, 1.0f}}},  // Red
    {{{0.0f, 1.0f, 0.0f, 1.0f}}},  // Green
    {{{0.0f, 0.0f, 1.0f, 1.0f}}},  // Blue
}};

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

struct RGBPacket : public FramePacket {
    uint32_t ColorIndex{0};
};

class RGBCallbacks : public IAppCallbacks {
public:
    uint32_t FramesRendered{0};

    void OnUpdate(Application* app, float dt) override {
        if (app->GetFrameInfo().LogicFrameIndex >= kFrameCount) {
            app->RequestExit();
        }
    }

    unique_ptr<FramePacket> OnExtractRenderData(Application* app, const AppFrameInfo& info) override {
        auto packet = make_unique<RGBPacket>();
        packet->ColorIndex = static_cast<uint32_t>(info.LogicFrameIndex % 3);
        return packet;
    }

    void OnRender(Application* app, GpuFrameContext* frameCtx, const AppFrameInfo& info, FramePacket* packet) override {
        auto* rgbPacket = static_cast<RGBPacket*>(packet);
        uint32_t colorIndex = rgbPacket != nullptr ? rgbPacket->ColorIndex : 0;

        auto* backBuffer = frameCtx->GetBackBuffer();
        uint32_t backBufferIndex = frameCtx->GetBackBufferIndex();

        auto* device = app->GetGpuRuntime()->GetDevice();

        // 按 back buffer 索引缓存 RTV, 保证 VkImageView 在 SubmitFrame 前不被析构
        if (backBufferIndex >= _cachedViews.size()) {
            _cachedViews.resize(backBufferIndex + 1);
        }

        if (!_cachedViews[backBufferIndex]) {
            TextureViewDescriptor viewDesc{};
            viewDesc.Target = backBuffer;
            viewDesc.Dim = TextureDimension::Dim2D;
            viewDesc.Format = app->GetSurface()->GetFormat();
            viewDesc.Range = SubresourceRange{0, 1, 0, 1};
            viewDesc.Usage = TextureViewUsage::RenderTarget;
            auto viewOpt = device->CreateTextureView(viewDesc);
            ASSERT_TRUE(viewOpt.HasValue()) << "Failed to create back buffer RTV";
            _cachedViews[backBufferIndex] = viewOpt.Release();
        }

        auto* view = _cachedViews[backBufferIndex].get();

        auto* cmd = frameCtx->CreateCommandBuffer();
        cmd->Begin();

        // barrier: 当前状态 → RenderTarget
        {
            TextureStates beforeState = TextureState::Undefined;
            if (backBufferIndex < _backBufferStates.size() && _backBufferStates[backBufferIndex] != TextureStates{TextureState::UNKNOWN}) {
                beforeState = _backBufferStates[backBufferIndex];
            }
            if (beforeState != TextureState::RenderTarget) {
                ResourceBarrierDescriptor barrier = BarrierTextureDescriptor{
                    backBuffer,
                    beforeState,
                    TextureState::RenderTarget};
                cmd->ResourceBarrier(std::span{&barrier, 1});
            }
        }

        // clear color render pass
        {
            ColorAttachment colorAttachment{
                view,
                LoadAction::Clear,
                StoreAction::Store,
                kClearColors[colorIndex]};
            RenderPassDescriptor passDesc{};
            passDesc.ColorAttachments = std::span{&colorAttachment, 1};
            auto passOpt = cmd->BeginRenderPass(passDesc);
            ASSERT_TRUE(passOpt.HasValue()) << "BeginRenderPass failed";
            cmd->EndRenderPass(passOpt.Release());
        }

        // barrier: RenderTarget → Present
        {
            ResourceBarrierDescriptor barrier = BarrierTextureDescriptor{
                backBuffer,
                TextureState::RenderTarget,
                TextureState::Present};
            cmd->ResourceBarrier(std::span{&barrier, 1});
        }

        cmd->End();

        // 记录 back buffer 状态
        if (backBufferIndex >= _backBufferStates.size()) {
            _backBufferStates.resize(backBufferIndex + 1, TextureState::UNKNOWN);
        }
        _backBufferStates[backBufferIndex] = TextureState::Present;

        FramesRendered++;
    }

    void OnShutdown(Application* app) override {
        _cachedViews.clear();
    }

    void OnSurfaceRecreated(Application* app, GpuSurface* surface) override {
        _cachedViews.clear();
    }

private:
    std::vector<TextureStates> _backBufferStates;
    std::vector<unique_ptr<TextureView>> _cachedViews;
};

std::vector<RenderBackend> GetEnabledBackends() noexcept {
    std::vector<RenderBackend> backends{};
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
    backends.push_back(RenderBackend::D3D12);
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    backends.push_back(RenderBackend::Vulkan);
#endif
    return backends;
}

std::string_view BackendName(RenderBackend backend) noexcept {
    switch (backend) {
        case RenderBackend::D3D12: return "D3D12";
        case RenderBackend::Vulkan: return "Vulkan";
        default: return "Unknown";
    }
}

class ApplicationSmokeTest : public ::testing::TestWithParam<RenderBackend> {};

TEST_P(ApplicationSmokeTest, RGBClearColor) {
    const auto backend = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    RGBCallbacks callbacks;

#if defined(_WIN32)
    Win32WindowCreateDescriptor windowDesc{};
    windowDesc.Title = "ApplicationSmoke";
    windowDesc.Width = kInitialWidth;
    windowDesc.Height = kInitialHeight;
    windowDesc.X = 120;
    windowDesc.Y = 120;
    windowDesc.Resizable = false;
    NativeWindowCreateDescriptor nativeDesc = windowDesc;
#elif defined(__APPLE__)
    CocoaWindowCreateDescriptor windowDesc{};
    windowDesc.Title = "ApplicationSmoke";
    windowDesc.Width = kInitialWidth;
    windowDesc.Height = kInitialHeight;
    windowDesc.X = 120;
    windowDesc.Y = 120;
    windowDesc.Resizable = false;
    NativeWindowCreateDescriptor nativeDesc = windowDesc;
#else
    GTEST_SKIP() << "No window support on this platform";
    return;
#endif

    auto windowOpt = CreateNativeWindow(nativeDesc);
    ASSERT_TRUE(windowOpt.HasValue()) << "Failed to create native window";
    auto window = windowOpt.Release();

    AppConfig config{};
    config.Window = window.get();
    config.Backend = backend;
    config.SurfaceFormat = TextureFormat::BGRA8_UNORM;
    config.PresentMode = PresentMode::FIFO;
    config.BackBufferCount = 3;
    config.FlightFrameCount = 2;
    config.MultiThreadedRender = false;
    config.AllowFrameDrop = false;

    Application app(config, &callbacks);
    int exitCode = app.Run();

    // 清理窗口
    window->Destroy();
    window.reset();

    EXPECT_EQ(exitCode, 0) << "Application::Run() returned non-zero exit code";
    EXPECT_GT(callbacks.FramesRendered, 0u) << "No frames were rendered";

    auto errors = logs.GetErrors();
    EXPECT_EQ(errors.size(), 0u);
    if (!errors.empty()) {
        for (size_t i = 0; i < std::min<size_t>(errors.size(), 8); ++i) {
            ADD_FAILURE() << "GPU error [" << i << "]: " << errors[i];
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    ApplicationSmoke,
    ApplicationSmokeTest,
    ::testing::ValuesIn(GetEnabledBackends()),
    [](const ::testing::TestParamInfo<RenderBackend>& info) {
        return std::string(BackendName(info.param));
    });

}  // namespace
