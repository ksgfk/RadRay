#include "runtime_test_support.h"
#include "gpu_test_fixture.h"

#include <gtest/gtest.h>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>

#ifdef CreateWindow
#undef CreateWindow
#endif

namespace radray {
namespace {

class FoundationApp final : public Application {
public:
    std::atomic<uint32_t> Recorded{0}, RecordedFrames{0}, AuxiliaryRecorded{0};
    uint32_t CompletedFrames{0};
    bool Resized{false}, AuxiliaryDestroyed{false}, WorldAvailable{false};
    bool ShutdownCompletionsDrained{false}, CallbackWaitResumed{false};

protected:
    void OnInit() override {
        ASSERT_NE(GetWorld(), nullptr);
        ASSERT_NE(GetWorld()->SpawnActor(), nullptr);
        WorldAvailable = GetWorld()->GetActors().size() == 1;
        auto deferred = make_shared<int>(1);
        _deferredLifetime = deferred;
        GetAssetManager()->DeferDestroy(std::move(deferred));
        EXPECT_FALSE(_deferredLifetime.expired());
    }
    void OnUpdate(const AppUpdateContext&) override {
        ++_updates;
#if defined(_WIN32)
        if (_updates == 3) {
            Win32WindowCreateDescriptor desc{};
            desc.Title = "Runtime auxiliary";
            desc.Width = 80;
            desc.Height = 60;
            desc.StartVisible = true;
            _auxiliary = GetWindowManager()->CreateWindow(desc, false);
            ASSERT_TRUE(_auxiliary);
            render::SwapChainDescriptor swapchain{};
            swapchain.Width = 80;
            swapchain.Height = 60;
            swapchain.Format = render::TextureFormat::BGRA8_UNORM;
            swapchain.PresentMode = render::PresentMode::FIFO;
            ASSERT_TRUE(_auxiliary->AttachSwapChain(swapchain));
        }
#endif
        if (_updates == 5) {
            GetWindowManager()->GetMainWindow()->GetNativeWindow()->SetSize(192, 144);
            Resized = true;
        }
        if (_updates == 7 && _auxiliary) {
            GetWindowManager()->DestroyWindow(_auxiliary.Get());
            _auxiliary = nullptr;
            AuxiliaryDestroyed = GetWindowManager()->GetWindowCount() == 1;
        }
        if (_updates >= 10) test::CloseMainWindow(*this);
    }
    void OnRender(AppFrameContext& ctx) override {
        ++RecordedFrames;
        auto* windows = GetWindowManager();
        auto* registry = GetRenderSystem()->GetRenderPassRegistry();
        ASSERT_NE(registry, nullptr);
        for (size_t index = 0; index < windows->GetWindowCount(); ++index) {
            auto* window = windows->GetWindow(index);
            const auto target = ctx.AcquireWindow(window);
            if (!target) continue;
            const auto desc = target->BackBuffer->GetDesc();
            const render::RenderPassColorAttachmentDescriptor attachment{desc.Format, desc.SampleCount, render::LoadAction::Clear, render::StoreAction::Store};
            auto pass = registry->GetOrCreateRenderPass({std::span{&attachment, 1}, {}});
            ASSERT_TRUE(pass);
            auto* view = target->BackBufferView;
            auto framebuffer = registry->GetOrCreateFramebuffer({pass.Get(), std::span{&view, 1}, nullptr, desc.Width, desc.Height, 1});
            ASSERT_TRUE(framebuffer);
            auto* commands = ctx.GetCommandBufferForTexture(target->BackBuffer);
            const render::ResourceBarrierDescriptor before = render::BarrierTextureDescriptor{
                .Target = target->BackBuffer, .Before = window->GetBackBufferState(target->BackBufferIndex), .After = render::TextureState::RenderTarget};
            commands->ResourceBarrier(std::span{&before, 1});
            const render::ColorClearValue clear{{.1f, .2f, .3f, 1}};
            auto encoder = commands->BeginRenderPass({pass.Get(), framebuffer.Get(), std::span{&clear, 1}, {}, "Runtime foundation"});
            ASSERT_TRUE(encoder);
            commands->EndRenderPass(encoder.Release());
            const render::ResourceBarrierDescriptor after = render::BarrierTextureDescriptor{
                .Target = target->BackBuffer, .Before = render::TextureState::RenderTarget, .After = render::TextureState::Present};
            commands->ResourceBarrier(std::span{&after, 1});
            ++Recorded;
            if (!window->IsMainWindow()) ++AuxiliaryRecorded;
        }
    }

    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        EXPECT_EQ(std::this_thread::get_id(), _gameThread);
        EXPECT_TRUE(_completedSerials.insert(completion.FrameSerial).second);
        if (completion.GpuWorkCompleted) ++CompletedFrames;
        if (!_callbackWaitStarted) {
            _callbackWaitStarted = true;
            _callbackWaitFrame = GetGpuSystem()->GetFrameIndex();
            _callbackTasks.Spawn(WaitFromCompletion());
            EXPECT_FALSE(CallbackWaitResumed);
            // A nested pump must not consume another batch or apply the old completion again.
            BeginUpdateForFlight(GetGpuSystem()->GetCurrentFlightIndex());
            EXPECT_FALSE(CallbackWaitResumed);
        }
    }

    void OnShutdown() override {
        EXPECT_TRUE(_deferredLifetime.expired());
        EXPECT_EQ(_completedSerials.size(), GetGpuSystem()->GetFrameIndex());
        ShutdownCompletionsDrained = true;
        EXPECT_TRUE(CallbackWaitResumed);
        _callbackTasks.RequestStop();
    }

private:
    task<void> WaitFromCompletion() {
        co_await GetGpuSystem()->Wait();
        EXPECT_EQ(std::this_thread::get_id(), _gameThread);
        EXPECT_GT(GetGpuSystem()->GetFrameIndex(), _callbackWaitFrame);
        CallbackWaitResumed = true;
    }

    uint32_t _updates{0};
    weak_ptr<int> _deferredLifetime;
    Nullable<AppWindow*> _auxiliary{nullptr};
    const std::thread::id _gameThread{std::this_thread::get_id()};
    unordered_set<uint64_t> _completedSerials;
    bool _callbackWaitStarted{false};
    uint64_t _callbackWaitFrame{0};
    TaskScope _callbackTasks;
};

void RunFoundation(render::RenderBackend backend, bool threaded) {
    {
        render::test::DeviceContext device;
        if (!render::test::TryCreateDevice(backend, device)) GTEST_SKIP() << device.Reason;
    }
    test::RuntimeLogCapture logs;
    FoundationApp app;
    ASSERT_EQ(app.Run({.Backend = backend, .EnableValidation = true, .Multithreaded = threaded, .EnableSynchronizationValidation = true, .WindowTitle = "Runtime foundation", .WindowWidth = 160, .WindowHeight = 120, .FlightDataCount = 2, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
    EXPECT_TRUE(app.WorldAvailable);
    EXPECT_TRUE(app.Resized);
    EXPECT_TRUE(app.AuxiliaryDestroyed);
    EXPECT_GT(app.AuxiliaryRecorded.load(), 0u);
    EXPECT_GT(app.Recorded.load(), 0u);
    EXPECT_EQ(app.CompletedFrames, app.RecordedFrames.load());
    EXPECT_TRUE(app.ShutdownCompletionsDrained);
    EXPECT_TRUE(app.CallbackWaitResumed);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(RuntimeFoundation, D3D12SingleThreadWindowLifecycle) { RunFoundation(render::RenderBackend::D3D12, false); }
TEST(RuntimeFoundation, D3D12ThreadedWindowLifecycle) { RunFoundation(render::RenderBackend::D3D12, true); }
TEST(RuntimeFoundation, VulkanSingleThreadWindowLifecycle) { RunFoundation(render::RenderBackend::Vulkan, false); }
TEST(RuntimeFoundation, VulkanThreadedWindowLifecycle) { RunFoundation(render::RenderBackend::Vulkan, true); }

#if defined(_WIN32)
class FrameBoundaryApp final : public Application {
public:
    explicit FrameBoundaryApp(bool modal) : _modal(modal) {}

protected:
    void OnInit() override {
        auto* window = GetWindowManager()->GetMainWindow()->GetNativeWindow();
        _hwnd = static_cast<HWND>(window->GetNativeHandler());
        _inputConnection = window->EventTextInput().connect(&FrameBoundaryApp::OnTextInput, this);
        ASSERT_NE(::PostMessageW(_hwnd, WM_CHAR, 'i', 0), 0);
    }

    void OnTextInput(std::string_view text) {
        if (text == "i") {
            const auto before = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            _eventDuration = std::chrono::steady_clock::now() - before;
            _initialInputHandled = true;
            if (_modal) {
                auto* window = GetWindowManager()->GetMainWindow()->GetNativeWindow();
                GetWindowManager()->EventModalLoopTick()(window);
                EXPECT_EQ(_updates, 1u);
            }
        } else if (text == "c") {
            ++_handledCompletions;
        }
    }

    void OnUpdate(const AppUpdateContext&) override {
        EXPECT_TRUE(_initialInputHandled);
        EXPECT_EQ(_handledCompletions, _postedCompletions);
        EXPECT_EQ(GetGpuSystem()->GetFrameIndex(), _updates);
        ++_updates;
        if (_updates == 2) {
            auto* window = GetWindowManager()->GetMainWindow()->GetNativeWindow();
            GetWindowManager()->EventModalLoopTick()(window);
            EXPECT_EQ(_updates, 2u);
        }
        if (_updates >= 5 && !_closing) {
            _closing = true;
            test::CloseMainWindow(*this);
        }
    }

    void OnRender(AppFrameContext& ctx) override {
        if (ctx.IsInModalLoop()) ++_modalFrames;
    }

    void OnRenderFrameComplete(const FlightCompletion&) override {
        if (_completions++ == 0) {
            EXPECT_GE(GetGpuSystem()->GetLastFrameLatency(), _eventDuration);
        }
        if (!_closing) {
            ++_postedCompletions;
            EXPECT_NE(::PostMessageW(_hwnd, WM_CHAR, 'c', 0), 0);
        }
    }

    void OnShutdown() override {
        EXPECT_GT(_handledCompletions, 0u);
        EXPECT_EQ(_handledCompletions, _postedCompletions);
        EXPECT_EQ(_completions, GetGpuSystem()->GetFrameIndex());
        EXPECT_EQ(_modalFrames.load(), _modal ? 1u : 0u);
        _inputConnection.disconnect();
    }

private:
    bool _modal;
    bool _initialInputHandled{false}, _closing{false};
    uint64_t _updates{0}, _completions{0}, _postedCompletions{0}, _handledCompletions{0};
    std::atomic<uint32_t> _modalFrames{0};
    std::chrono::duration<float> _eventDuration{};
    HWND _hwnd{nullptr};
    sigslot::scoped_connection _inputConnection;
};

void RunFrameBoundary(render::RenderBackend backend, bool threaded) {
    {
        render::test::DeviceContext device;
        if (!render::test::TryCreateDevice(backend, device)) GTEST_SKIP() << device.Reason;
    }
    for (bool modal : {false, true}) {
        SCOPED_TRACE(modal ? "modal dispatch" : "normal dispatch");
        test::RuntimeLogCapture logs;
        FrameBoundaryApp app{modal};
        ASSERT_EQ(app.Run({.Backend = backend, .EnableValidation = true, .Multithreaded = threaded, .EnableSynchronizationValidation = true, .WindowTitle = "Frame boundary", .WindowWidth = 80, .WindowHeight = 60, .FlightDataCount = 1, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
        EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    }
}

TEST(RuntimeFoundation, D3D12SingleThreadCompletionInputBeforeUpdate) { RunFrameBoundary(render::RenderBackend::D3D12, false); }
TEST(RuntimeFoundation, D3D12ThreadedCompletionInputBeforeUpdate) { RunFrameBoundary(render::RenderBackend::D3D12, true); }
TEST(RuntimeFoundation, VulkanSingleThreadCompletionInputBeforeUpdate) { RunFrameBoundary(render::RenderBackend::Vulkan, false); }
TEST(RuntimeFoundation, VulkanThreadedCompletionInputBeforeUpdate) { RunFrameBoundary(render::RenderBackend::Vulkan, true); }

class StalledGpuInputApp final : public Application {
public:
    explicit StalledGpuInputApp(render::RenderBackend backend) : _backend(backend) {}

    ~StalledGpuInputApp() noexcept override {
        if (_inputThread.joinable()) _inputThread.join();
    }

protected:
    void OnInit() override {
        auto gate = GetGpuSystem()->GetDevice()->CreateFence();
        ASSERT_TRUE(gate);
        _gate = gate.Release();
        auto completed = GetGpuSystem()->GetDevice()->CreateFence();
        ASSERT_TRUE(completed);
        _completedFence = completed.Release();
        auto* window = GetWindowManager()->GetMainWindow()->GetNativeWindow();
        _hwnd = static_cast<HWND>(window->GetNativeHandler());
        _keyboardConnection = window->EventKeyboard().connect([this](KeyCode key, Action action) {
            if (key != KeyCode::F6) return;
            RecordInput(action == Action::PRESSED ? "key down" : "key up");
            if (action == Action::PRESSED) ++_keyPresses;
            _keyDown = action != Action::RELEASED;
        });
        _textConnection = window->EventTextInput().connect([this](std::string_view text) {
            RecordInput(string{text});
            _text.append(text);
        });
        _touchConnection = window->EventTouch().connect([this](int x, int y, MouseButton button, Action action) {
            if (button != MouseButton::BUTTON_LEFT || x != 11 || y != 13 || (action != Action::PRESSED && action != Action::RELEASED)) return;
            RecordInput(action == Action::PRESSED ? "mouse down" : "mouse up");
            if (action == Action::PRESSED) ++_clicks;
            _mouseDown = action != Action::RELEASED;
        });
        _wheelConnection = window->EventMouseWheel().connect([this](int delta) {
            RecordInput(delta > 0 ? "wheel up" : "wheel down");
            _wheelTotal += delta;
        });
    }

    void OnUpdate(const AppUpdateContext&) override {
        const uint32_t update = ++_updates;
        if (!_gate || !_completedFence) {
            test::CloseMainWindow(*this);
            return;
        }
        if (update != 2) return;
        EXPECT_TRUE(_released.load(std::memory_order_acquire));
        EXPECT_GE(_completedFence->GetCompletedValue(), 1u);
        const vector<string> expected{
            "key down", "key up", "a", "mouse down", "mouse up", "wheel up",
            "key down", "key up", "b", "mouse down", "mouse up", "wheel down",
            "key down", "key up", "c", "mouse down", "mouse up", "wheel up"};
        EXPECT_EQ(_events, expected);
        EXPECT_EQ(_keyPresses, 3u);
        EXPECT_EQ(_clicks, 3u);
        EXPECT_EQ(_text, "abc");
        EXPECT_EQ(_wheelTotal, WHEEL_DELTA);
        EXPECT_FALSE(_keyDown);
        EXPECT_FALSE(_mouseDown);
        _verified = true;
        test::CloseMainWindow(*this);
    }

    void OnRender(AppFrameContext& ctx) override {
        if (_submitted || !_gate || !_completedFence) return;
        _submitted = true;
        render::Fence* waits[]{_gate.get()};
        render::Fence* signals[]{_completedFence.get()};
        uint64_t values[]{1};
        ctx.SubmitFrame({.SignalFences = signals, .SignalValues = values, .WaitFences = waits, .WaitValues = values});
        _inputThread = std::thread([this] {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            for (uint32_t i = 0; i < 3; ++i) {
                EXPECT_EQ(_updates.load(), 1u);
                EXPECT_EQ(_completedFence->GetCompletedValue(), 0u);
                EXPECT_NE(::PostMessageW(_hwnd, WM_KEYDOWN, VK_F6, 1), 0);
                EXPECT_NE(::PostMessageW(_hwnd, WM_KEYUP, VK_F6, static_cast<LPARAM>(0xC0000001u)), 0);
                EXPECT_NE(::PostMessageW(_hwnd, WM_CHAR, 'a' + i, 1), 0);
                EXPECT_NE(::PostMessageW(_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(11, 13)), 0);
                EXPECT_NE(::PostMessageW(_hwnd, WM_LBUTTONUP, 0, MAKELPARAM(11, 13)), 0);
                const int delta = i == 1 ? -WHEEL_DELTA : WHEEL_DELTA;
                EXPECT_NE(::PostMessageW(_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)), 0), 0);
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            EXPECT_EQ(_updates.load(), 1u);
            EXPECT_EQ(_completedFence->GetCompletedValue(), 0u);
            _released.store(true, std::memory_order_release);
            EXPECT_TRUE(ReleaseGpu());
        });
    }

    void OnRenderFrameComplete(const FlightCompletion&) override { ++_completions; }

    void OnShutdown() override {
        if (_inputThread.joinable()) _inputThread.join();
        EXPECT_TRUE(_verified);
        EXPECT_EQ(_events.size(), 18u);
        EXPECT_EQ(_completions, GetGpuSystem()->GetFrameIndex());
        _keyboardConnection.disconnect();
        _textConnection.disconnect();
        _touchConnection.disconnect();
        _wheelConnection.disconnect();
        _completedFence.reset();
        _gate.reset();
    }

private:
    void RecordInput(string event) {
        EXPECT_TRUE(_released.load(std::memory_order_acquire));
        EXPECT_EQ(_updates.load(), 1u);
        EXPECT_GT(_completions, 0u);
        _events.push_back(std::move(event));
    }

    bool ReleaseGpu() {
#if defined(RADRAY_ENABLE_D3D12)
        if (_backend == render::RenderBackend::D3D12) {
            auto* fence = static_cast<render::d3d12::FenceD3D12*>(_gate.get());
            return SUCCEEDED(fence->_fence->Signal(1));
        }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
        if (_backend == render::RenderBackend::Vulkan) {
            auto* fence = static_cast<render::vulkan::FenceVulkan*>(_gate.get());
            const VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, fence->_fence->_semaphore, 1};
            return fence->_device->_ftb.vkSignalSemaphore(fence->_device->_device, &signal) == VK_SUCCESS;
        }
#endif
        return false;
    }

    render::RenderBackend _backend;
    unique_ptr<render::Fence> _gate, _completedFence;
    HWND _hwnd{nullptr};
    sigslot::scoped_connection _keyboardConnection, _textConnection, _touchConnection, _wheelConnection;
    vector<string> _events;
    string _text;
    uint32_t _keyPresses{0}, _clicks{0};
    int _wheelTotal{0};
    bool _keyDown{false}, _mouseDown{false}, _submitted{false}, _verified{false};
    uint64_t _completions{0};
    std::atomic<uint32_t> _updates{0};
    std::atomic_bool _released{false};
    std::thread _inputThread;
};

void RunStalledGpuInput(render::RenderBackend backend, bool threaded) {
    {
        render::test::DeviceContext device;
        if (!render::test::TryCreateDevice(backend, device)) GTEST_SKIP() << device.Reason;
    }
    test::RuntimeLogCapture logs;
    StalledGpuInputApp app{backend};
    // Host-signaled queue waits run without validation-layer semaphore tracking.
    ASSERT_EQ(app.Run({.Backend = backend, .Multithreaded = threaded, .WindowTitle = "Stalled GPU input", .WindowWidth = 80, .WindowHeight = 60, .FlightDataCount = 1, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(RuntimeFoundation, D3D12SingleThreadInputDuringGpuStall) { RunStalledGpuInput(render::RenderBackend::D3D12, false); }
TEST(RuntimeFoundation, D3D12ThreadedInputDuringGpuStall) { RunStalledGpuInput(render::RenderBackend::D3D12, true); }
TEST(RuntimeFoundation, VulkanSingleThreadInputDuringGpuStall) { RunStalledGpuInput(render::RenderBackend::Vulkan, false); }
TEST(RuntimeFoundation, VulkanThreadedInputDuringGpuStall) { RunStalledGpuInput(render::RenderBackend::Vulkan, true); }
#endif

}  // namespace
}  // namespace radray
