#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "gpu_runtime_test_support.h"

#include <gtest/gtest.h>

#include <semaphore>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/world_manager.h>
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
        const auto worldId = GetWorldManager()->CreateWorld();
        ASSERT_TRUE(GetWorldManager()->GetWorld(worldId));
        ASSERT_NE(GetWorldManager()->GetWorld(worldId)->SpawnActor(), nullptr);
        WorldAvailable = GetWorldManager()->GetWorld(worldId)->GetActors().size() == 1;
        auto deferred = make_shared<int>(1);
        _deferredLifetime = deferred;
        GetAssetManager()->DeferDestroy(std::move(deferred));
        EXPECT_FALSE(_deferredLifetime.expired());
    }
    void OnUpdate(const AppUpdateContext&) override {
        ++_updates;
        if (_updates == 3) _windowTasks.Spawn(CreateAuxiliary());
        if (_updates == 5) _windowTasks.Spawn(ResizeMain());
        if (_updates == 7 && _auxiliary.Id != 0) _windowTasks.Spawn(DestroyAuxiliary());
        if (_updates >= 10) test::CloseMainWindow(*this);
    }
    void OnRender(AppFrameContext& ctx) override {
        ++RecordedFrames;
        auto* prefix = ctx.AllocateCommandBuffer();
        ctx.ReturnCommandBuffers({.CmdBuffers = std::span{&prefix, 1}});
        auto* windows = GetWindowManager().Get();
        auto* registry = GetRenderSystem()->GetRenderPassRegistry();
        ASSERT_NE(registry, nullptr);
        for (size_t index = 0; index < windows->GetWindowCount(); ++index) {
            auto* window = windows->GetWindow(index);
            auto target = ctx.AcquireWindow(window);
            if (!target) continue;
            const auto desc = target->BackBuffer->GetDesc();
            const render::RenderPassColorAttachmentDescriptor attachment{desc.Format, desc.SampleCount, render::LoadAction::Clear, render::StoreAction::Store};
            auto pass = registry->GetOrCreateRenderPass({std::span{&attachment, 1}, {}});
            ASSERT_TRUE(pass);
            auto* view = target->BackBufferView;
            auto framebuffer = registry->GetOrCreateFramebuffer({pass.Get(), std::span{&view, 1}, nullptr, desc.Width, desc.Height, 1});
            ASSERT_TRUE(framebuffer);
            auto* commands = ctx.AllocateCommandBuffer();
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
            auto* endCommands = ctx.AllocateCommandBuffer();
            render::CommandBuffer* batch[]{commands, endCommands};
            ctx.ReturnCommandBuffers({.CmdBuffers = batch}, std::move(target));
            ++Recorded;
            if (!window->IsMainWindow()) ++AuxiliaryRecorded;
        }
        auto* suffix = ctx.AllocateCommandBuffer();
        ctx.ReturnCommandBuffers({.CmdBuffers = std::span{&suffix, 1}});
    }

    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        EXPECT_EQ(std::this_thread::get_id(), _gameThread);
        EXPECT_TRUE(_completedSerials.insert(completion.FrameSerial).second);
        if (completion.GpuWorkCompleted) ++CompletedFrames;
        if (!_callbackWaitStarted) {
            _callbackWaitStarted = true;
            _callbackWaitFrame = GetFrameTimeline().GetFrameIndex();
            _callbackTasks.Spawn(WaitFromCompletion());
            EXPECT_FALSE(CallbackWaitResumed);
            // A modal tick during a completion callback must not reenter the runner's frame preparation.
            GetWindowManager()->EventModalLoopTick()(GetWindowManager()->GetMainWindow()->GetNativeWindow());
            EXPECT_FALSE(CallbackWaitResumed);
        }
    }

    void OnShutdown() override {
        EXPECT_TRUE(_deferredLifetime.expired());
        EXPECT_EQ(_completedSerials.size(), GetFrameTimeline().GetFrameIndex());
        ShutdownCompletionsDrained = true;
        EXPECT_TRUE(CallbackWaitResumed);
        _callbackTasks.RequestStop();
    }

private:
    task<void> CreateAuxiliary() {
        WindowCreateDescriptor desc{};
        desc.Title = "Runtime auxiliary";
        desc.Width = 80;
        desc.Height = 60;
        auto result = co_await GetWindowManager()->CreateWindow(std::move(desc));
        EXPECT_EQ(result.Status, WindowOperationStatus::Completed);
        _auxiliary = result.Handle;
        WindowSwapChainDescriptor swapchain{};
        swapchain.Format = render::TextureFormat::BGRA8_UNORM;
        EXPECT_EQ(co_await GetWindowManager()->AttachSwapChain(_auxiliary, swapchain), WindowOperationStatus::Completed);
    }
    task<void> ResizeMain() {
        EXPECT_EQ(co_await GetWindowManager()->SetSize(GetWindowManager()->GetMainWindow()->GetHandle(), 192, 144), WindowOperationStatus::Completed);
        Resized = true;
    }
    task<void> DestroyAuxiliary() {
        EXPECT_EQ(co_await GetWindowManager()->DestroyWindow(_auxiliary), WindowOperationStatus::Completed);
        _auxiliary = {};
        AuxiliaryDestroyed = GetWindowManager()->GetWindowCount() == 1;
    }

    task<void> WaitFromCompletion() {
        co_await GetFrameTimeline().Wait();
        EXPECT_EQ(std::this_thread::get_id(), _gameThread);
        EXPECT_GT(GetFrameTimeline().GetFrameIndex(), _callbackWaitFrame);
        CallbackWaitResumed = true;
    }

    uint32_t _updates{0};
    weak_ptr<int> _deferredLifetime;
    WindowHandle _auxiliary{};
    TaskScope _windowTasks;
    const std::thread::id _gameThread{std::this_thread::get_id()};
    unordered_set<uint64_t> _completedSerials;
    bool _callbackWaitStarted{false};
    uint64_t _callbackWaitFrame{0};
    TaskScope _callbackTasks;
};

void RunFoundation(render::RenderBackend backend, bool threaded) {
    test::RuntimeLogCapture logs;
    FoundationApp app;
    auto run = test::RunApplication(app, {
        .FlightDataCount = 2,
        .Window = WindowOptions{.Title = "Runtime foundation", .Width = 160, .Height = 120},
        .Gpu = GpuOptions{
            .Backend = backend,
            .EnableValidation = true,
            .EnableSynchronizationValidation = true,
            .Multithreaded = threaded,
            .EnableFrameProfiler = false,
            .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
            .PresentMode = render::PresentMode::FIFO,
        },
    });
    if (test::CanSkipRuntimeStartup(backend, run.Startup)) GTEST_SKIP() << run.Startup.Reason;
    ASSERT_EQ(run.Startup.Status, RuntimeStartupStatus::Started) << run.Startup.Reason;
    ASSERT_EQ(run.ExitCode, 0);
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

class FlightReuseOverlapApp final : public Application {
protected:
    void OnUpdate(const AppUpdateContext&) override {
        ++_updates;
        if (_updates == 2) {
            EXPECT_TRUE(_firstRecording.try_acquire_for(std::chrono::seconds{5}));
            _secondUpdate.release();
        } else if (_updates == 3) {
            EXPECT_TRUE(_secondRecording.try_acquire_for(std::chrono::seconds{5}));
            _thirdUpdate.release();
        }
        if (_updates >= 3) RequestExit();
    }

    void OnRender(AppFrameContext& ctx) override {
        if (ctx.FrameSerial() == 1) {
            _firstRecording.release();
            _firstOverlap = _secondUpdate.try_acquire_for(std::chrono::seconds{5});
        } else if (ctx.FrameSerial() == 2) {
            _secondRecording.release();
            _reuseOverlap = _thirdUpdate.try_acquire_for(std::chrono::seconds{5});
        }
    }

    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        EXPECT_TRUE(_completed.insert(completion.FrameSerial).second);
    }

    void OnShutdown() override {
        EXPECT_TRUE(_firstOverlap);
        EXPECT_TRUE(_reuseOverlap);
        EXPECT_EQ(_completed.size(), GetFrameTimeline().GetFrameIndex());
    }

private:
    std::binary_semaphore _firstRecording{0}, _secondRecording{0}, _secondUpdate{0}, _thirdUpdate{0};
    uint32_t _updates{0};
    bool _firstOverlap{false}, _reuseOverlap{false};
    unordered_set<uint64_t> _completed;
};

void RunFlightReuseOverlap(render::RenderBackend backend) {
    test::RuntimeLogCapture logs;
    FlightReuseOverlapApp app;
    auto run = test::RunApplication(app, {
        .FlightDataCount = 2,
        .Window = std::nullopt,
        .Gpu = GpuOptions{
            .Backend = backend,
            .EnableValidation = true,
            .EnableSynchronizationValidation = true,
            .Multithreaded = true,
            .EnableFrameProfiler = false,
        },
        .Render = std::nullopt,
        .World = std::nullopt,
        .Asset = std::nullopt,
    });
    if (test::CanSkipRuntimeStartup(backend, run.Startup)) GTEST_SKIP() << run.Startup.Reason;
    ASSERT_EQ(run.Startup.Status, RuntimeStartupStatus::Started) << run.Startup.Reason;
    ASSERT_EQ(run.ExitCode, 0);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(RuntimeFoundation, D3D12SlotReuseOverlapsTheNextRecording) { RunFlightReuseOverlap(render::RenderBackend::D3D12); }
TEST(RuntimeFoundation, VulkanSlotReuseOverlapsTheNextRecording) { RunFlightReuseOverlap(render::RenderBackend::Vulkan); }

#if defined(_WIN32)
class DroppedPresentationApp final : public Application {
public:
    bool Dropped{false}, Recovered{false};

protected:
    void OnInit() override {
        auto* device = GetGpuSystem()->GetDevice();
        _vulkan = device->GetBackend() == render::RenderBackend::Vulkan;
        _upload = device->CreateBuffer({.Size = 8, .Memory = render::MemoryType::Upload, .Usage = render::BufferUse::CopySource | render::BufferUse::MapWrite}).Unwrap();
        _readback = device->CreateBuffer({.Size = 4, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
        ScopedBufferMap map{_upload.get(), {0, 8}};
        ASSERT_TRUE(map);
        const uint32_t values[]{7, 99};
        std::memcpy(map.Data(), values, sizeof(values));
        _upload->FlushMappedRange({0, 8});
    }
    void OnUpdate(const AppUpdateContext&) override {
        if (++_updates > 20 || Recovered) test::CloseMainWindow(*this);
    }
    void OnRender(AppFrameContext& ctx) override {
        if (_step >= 3) return;
        auto* window = GetWindowManager()->GetMainWindow();
        if (_step < 2) {
            auto* commands = ctx.AllocateCommandBuffer();
            if (_vulkan) {
                const render::ResourceBarrierDescriptor before = render::BarrierBufferDescriptor{
                    .Target = _readback.get(), .Before = _step == 0 ? render::BufferState::Undefined : render::BufferState::HostRead, .After = render::BufferState::CopyDestination};
                commands->ResourceBarrier(std::span{&before, 1});
            }
            commands->CopyBufferToBuffer(_readback.get(), 0, _upload.get(), _step * 4, 4);
            if (_vulkan) {
                const render::ResourceBarrierDescriptor after = render::BarrierBufferDescriptor{
                    .Target = _readback.get(), .Before = render::BufferState::CopyDestination, .After = render::BufferState::HostRead};
                commands->ResourceBarrier(std::span{&after, 1});
            }
            ctx.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
        }
        if (_step != 0) {
            auto target = ctx.AcquireWindow(window);
            ASSERT_TRUE(target);
            auto* commands = ctx.AllocateCommandBuffer();
            if (_step == 2) {
                const render::ResourceBarrierDescriptor present = render::BarrierTextureDescriptor{
                    .Target = target->BackBuffer, .Before = window->GetBackBufferState(target->BackBufferIndex), .After = render::TextureState::Present};
                commands->ResourceBarrier(std::span{&present, 1});
            }
            ctx.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}}, std::move(target));
        }
        if (_step == 1) {
            _droppedSerial = ctx.FrameSerial();
            const auto hwnd = static_cast<HWND>(window->GetNativeWindow()->GetNativeHandler());
            // Single-thread test: reproduce an external hide between acquire and submit.
            ::ShowWindow(hwnd, SW_HIDE);
            EXPECT_FALSE(window->IsSwapChainPresentable());
            ctx.SubmitFrame();
            ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        } else if (_step == 2) {
            _recoveredSerial = ctx.FrameSerial();
        }
        ++_step;
    }
    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        if (completion.FrameSerial == _droppedSerial) {
            EXPECT_FALSE(completion.GpuWorkCompleted);
            ScopedBufferMap map{_readback.get(), {0, 4}};
            ASSERT_TRUE(map);
            _readback->InvalidateMappedRange({0, 4});
            uint32_t value{0};
            std::memcpy(&value, map.Data(), sizeof(value));
            EXPECT_EQ(value, 7u);
            Dropped = true;
        }
        if (completion.FrameSerial == _recoveredSerial) {
            EXPECT_TRUE(completion.GpuWorkCompleted);
            Recovered = true;
        }
    }
    void OnShutdown() override {
        _readback.reset();
        _upload.reset();
    }

private:
    unique_ptr<render::Buffer> _upload, _readback;
    bool _vulkan{false};
    uint32_t _step{0}, _updates{0};
    uint64_t _droppedSerial{0}, _recoveredSerial{0};
};

void RunDroppedPresentation(render::RenderBackend backend) {
    test::RuntimeLogCapture logs;
    DroppedPresentationApp app;
    auto run = test::RunApplication(app, {
        .Window = WindowOptions{.Title = "Dropped presentation", .Width = 80, .Height = 60},
        .Gpu = GpuOptions{
            .Backend = backend,
            .EnableValidation = true,
            .EnableSynchronizationValidation = true,
            .EnableFrameProfiler = false,
            .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
            .PresentMode = render::PresentMode::FIFO,
        },
        .Render = std::nullopt,
        .World = std::nullopt,
        .Asset = std::nullopt,
    });
    if (test::CanSkipRuntimeStartup(backend, run.Startup)) GTEST_SKIP() << run.Startup.Reason;
    ASSERT_EQ(run.Startup.Status, RuntimeStartupStatus::Started) << run.Startup.Reason;
    ASSERT_EQ(run.ExitCode, 0);
    EXPECT_TRUE(app.Dropped);
    EXPECT_TRUE(app.Recovered);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(RuntimeFoundation, D3D12DroppedPresentationRecovers) { RunDroppedPresentation(render::RenderBackend::D3D12); }
TEST(RuntimeFoundation, VulkanDroppedPresentationRecovers) { RunDroppedPresentation(render::RenderBackend::Vulkan); }

class TargetContractApp final : public Application {
protected:
    void OnUpdate(const AppUpdateContext&) override {
        if (_done) test::CloseMainWindow(*this);
    }
    void OnRender(AppFrameContext& ctx) override {
        if (_done) return;
        auto target = ctx.AcquireWindow(GetWindowManager()->GetMainWindow());
        ASSERT_TRUE(target);
        EXPECT_DEATH(ctx.SubmitFrame(), "");
        EXPECT_DEATH(ctx.AcquireWindow(target->Window), "");
        EXPECT_DEATH(ctx.ReturnCommandBuffers({}, std::move(target)), "");
        auto* commands = ctx.AllocateCommandBuffer();
        EXPECT_DEATH(ctx.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}}, AppFrameTarget{}), "");
        EXPECT_DEATH({
            auto other = GetGpuSystem()->BeginFrameRecord((ctx.FlightIndex() + 1) % 2, {}, {}, false);
            auto* foreign = other.AllocateCommandBuffer();
            other.ReturnCommandBuffers({.CmdBuffers = std::span{&foreign, 1}}, std::move(target)); }, "");
        const render::ResourceBarrierDescriptor present = render::BarrierTextureDescriptor{
            .Target = target->BackBuffer, .Before = target->Window->GetBackBufferState(target->BackBufferIndex), .After = render::TextureState::Present};
        commands->ResourceBarrier(std::span{&present, 1});
        ctx.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}}, std::move(target));
        auto* another = ctx.AllocateCommandBuffer();
        EXPECT_DEATH(ctx.ReturnCommandBuffers({.CmdBuffers = std::span{&another, 1}}, std::move(target)), "");
        ctx.ReturnCommandBuffers({.CmdBuffers = std::span{&another, 1}});
        _done = true;
    }
    bool _done{false};
};

TEST(RuntimeFoundationDeathTest, RejectsUnreturnedForeignAndConsumedTargets) {
    TargetContractApp app;
    auto run = test::RunApplication(app, {
        .Window = WindowOptions{.Title = "Target contract", .Width = 80, .Height = 60},
        .Gpu = GpuOptions{
            .Backend = render::RenderBackend::D3D12,
            .EnableFrameProfiler = false,
            .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
            .PresentMode = render::PresentMode::FIFO,
        },
        .Render = std::nullopt,
        .World = std::nullopt,
        .Asset = std::nullopt,
    });
    if (test::CanSkipRuntimeStartup(render::RenderBackend::D3D12, run.Startup)) GTEST_SKIP() << run.Startup.Reason;
    ASSERT_EQ(run.Startup.Status, RuntimeStartupStatus::Started) << run.Startup.Reason;
    EXPECT_EQ(run.ExitCode, 0);
}

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
        EXPECT_EQ(GetFrameTimeline().GetFrameIndex(), _updates);
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
            EXPECT_GE(GetFrameTimeline().GetLastFrameLatency(), _eventDuration);
        }
        if (!_closing) {
            ++_postedCompletions;
            EXPECT_NE(::PostMessageW(_hwnd, WM_CHAR, 'c', 0), 0);
        }
    }

    void OnShutdown() override {
        EXPECT_GT(_handledCompletions, 0u);
        EXPECT_EQ(_handledCompletions, _postedCompletions);
        EXPECT_EQ(_completions, GetFrameTimeline().GetFrameIndex());
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
    for (bool modal : {false, true}) {
        SCOPED_TRACE(modal ? "modal dispatch" : "normal dispatch");
        test::RuntimeLogCapture logs;
        FrameBoundaryApp app{modal};
        auto run = test::RunApplication(app, {
            .FlightDataCount = 1,
            .Window = WindowOptions{.Title = "Frame boundary", .Width = 80, .Height = 60},
            .Gpu = GpuOptions{
                .Backend = backend,
                .EnableValidation = true,
                .EnableSynchronizationValidation = true,
                .Multithreaded = threaded,
                .EnableFrameProfiler = false,
                .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
                .PresentMode = render::PresentMode::FIFO,
            },
            .Render = std::nullopt,
            .World = std::nullopt,
            .Asset = std::nullopt,
        });
        if (test::CanSkipRuntimeStartup(backend, run.Startup)) GTEST_SKIP() << run.Startup.Reason;
        ASSERT_EQ(run.Startup.Status, RuntimeStartupStatus::Started) << run.Startup.Reason;
        ASSERT_EQ(run.ExitCode, 0);
        EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    }
}

TEST(RuntimeFoundation, D3D12SingleThreadCompletionInputBeforeUpdate) { RunFrameBoundary(render::RenderBackend::D3D12, false); }
TEST(RuntimeFoundation, D3D12ThreadedCompletionInputBeforeUpdate) { RunFrameBoundary(render::RenderBackend::D3D12, true); }
TEST(RuntimeFoundation, VulkanSingleThreadCompletionInputBeforeUpdate) { RunFrameBoundary(render::RenderBackend::Vulkan, false); }
TEST(RuntimeFoundation, VulkanThreadedCompletionInputBeforeUpdate) { RunFrameBoundary(render::RenderBackend::Vulkan, true); }

class StalledGpuInputApp final : public Application {
public:
    explicit StalledGpuInputApp(render::RenderBackend backend, bool mutateWindow = false) : _backend(backend), _mutateWindow(mutateWindow) {}

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
        if (_mutateWindow) {
            _mutationTasks.Spawn(ResizeDuringGpuStall());
            return;
        }
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
        ctx.ReturnCommandBuffers({.SignalFences = signals, .SignalValues = values, .WaitFences = waits, .WaitValues = values});
        ctx.SubmitFrame();
        _inputThread = std::thread([this] {
            if (_mutateWindow) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
                while (!_mutationRequested.load() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                EXPECT_TRUE(_mutationRequested.load());
                std::this_thread::sleep_for(std::chrono::milliseconds{30});
                EXPECT_FALSE(_mutationCompleted.load());
                _released.store(true, std::memory_order_release);
                EXPECT_TRUE(ReleaseGpu());
                return;
            }
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
        EXPECT_EQ(_events.size(), _mutateWindow ? 0u : 18u);
        EXPECT_EQ(_completions, GetFrameTimeline().GetFrameIndex());
        _keyboardConnection.disconnect();
        _textConnection.disconnect();
        _touchConnection.disconnect();
        _wheelConnection.disconnect();
        _completedFence.reset();
        _gate.reset();
    }

private:
    task<void> ResizeDuringGpuStall() {
        _mutationRequested = true;
        auto* windows = GetWindowManager().Get();
        const auto status = co_await windows->SetSize(windows->GetMainWindow()->GetHandle(), 360, 240);
        EXPECT_EQ(status, WindowOperationStatus::Completed);
        EXPECT_TRUE(_released.load());
        EXPECT_GE(_completedFence->GetCompletedValue(), 1u);
        _mutationCompleted = true;
        _verified = true;
        test::CloseMainWindow(*this);
    }

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
    bool _mutateWindow;
    std::atomic_bool _mutationRequested{false}, _mutationCompleted{false};
    TaskScope _mutationTasks;
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

void RunStalledGpuInput(render::RenderBackend backend, bool threaded, bool mutateWindow = false) {
    test::RuntimeLogCapture logs;
    StalledGpuInputApp app{backend, mutateWindow};
    // Host-signaled queue waits run without validation-layer semaphore tracking.
    auto run = test::RunApplication(app, {
        .FlightDataCount = mutateWindow ? 2u : 1u,
        .Window = WindowOptions{.Title = "Stalled GPU input", .Width = 80, .Height = 60},
        .Gpu = GpuOptions{
            .Backend = backend,
            .Multithreaded = threaded,
            .EnableFrameProfiler = false,
            .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
            .PresentMode = render::PresentMode::FIFO,
        },
        .Render = std::nullopt,
        .World = std::nullopt,
        .Asset = std::nullopt,
    });
    if (test::CanSkipRuntimeStartup(backend, run.Startup)) GTEST_SKIP() << run.Startup.Reason;
    ASSERT_EQ(run.Startup.Status, RuntimeStartupStatus::Started) << run.Startup.Reason;
    ASSERT_EQ(run.ExitCode, 0);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(RuntimeFoundation, D3D12SingleThreadInputDuringGpuStall) { RunStalledGpuInput(render::RenderBackend::D3D12, false); }
TEST(RuntimeFoundation, D3D12ThreadedInputDuringGpuStall) { RunStalledGpuInput(render::RenderBackend::D3D12, true); }
TEST(RuntimeFoundation, VulkanSingleThreadInputDuringGpuStall) { RunStalledGpuInput(render::RenderBackend::Vulkan, false); }
TEST(RuntimeFoundation, VulkanThreadedInputDuringGpuStall) { RunStalledGpuInput(render::RenderBackend::Vulkan, true); }

TEST(RuntimeFoundation, D3D12SingleThreadWindowMutationWaitsForGpu) { RunStalledGpuInput(render::RenderBackend::D3D12, false, true); }
TEST(RuntimeFoundation, D3D12ThreadedWindowMutationWaitsForGpu) { RunStalledGpuInput(render::RenderBackend::D3D12, true, true); }
TEST(RuntimeFoundation, VulkanSingleThreadWindowMutationWaitsForGpu) { RunStalledGpuInput(render::RenderBackend::Vulkan, false, true); }
TEST(RuntimeFoundation, VulkanThreadedWindowMutationWaitsForGpu) { RunStalledGpuInput(render::RenderBackend::Vulkan, true, true); }

class WindowMutationApp final : public Application {
public:
    bool Completed{false}, ShutdownCanceled{false};

protected:
    void OnInit() override {
        auto* windows = GetWindowManager().Get();
        _main = windows->GetMainWindow()->GetHandle();
        _surface = windows->GetMainWindow()->GetNativeWindow()->EventBeforeSurfaceChange().connect([this] {
            EXPECT_FALSE(_recording.load());
            for (uint32_t i = 0; i < GetGpuSystem()->GetFlightDataCount(); ++i) {
                EXPECT_FALSE(GetGpuSystem()->GetFlightGpuSignal(i).IsValid());
            }
            const auto recorded = _recorded.load();
            auto* native = GetWindowManager()->ResolveWindow(_main)->GetNativeWindow();
            GetWindowManager()->EventModalLoopTick()(native);
            EXPECT_EQ(_recorded.load(), recorded);
        });
        _tasks.Spawn(Mutate());
    }

    void OnUpdate(const AppUpdateContext&) override {
        // Bounded failure exit, so a broken maintenance scheduler cannot hang this test.
        if (++_updates == 40) test::CloseMainWindow(*this);
    }

    void OnRender(AppFrameContext&) override {
        _recording = true;
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        ++_recorded;
        _recording = false;
    }

    void OnShutdown() override {
        EXPECT_TRUE(Completed);
        EXPECT_TRUE(ShutdownCanceled);
        EXPECT_GT(_recorded.load(), 0u);
        _surface.disconnect();
    }

private:
    task<void> Mutate() {
        auto* windows = GetWindowManager().Get();
        WindowCreateDescriptor desc{};
        desc.Title = "Deferred swapchain";
        desc.Width = 320;
        desc.Height = 200;
        desc.StartVisible = false;
        auto created = co_await windows->CreateWindow(desc);
        EXPECT_EQ(created.Status, WindowOperationStatus::Completed);
        WindowSwapChainDescriptor swapchain{};
        swapchain.Format = render::TextureFormat::BGRA8_UNORM;
        EXPECT_EQ(co_await windows->AttachSwapChain(created.Handle, swapchain), WindowOperationStatus::Deferred);
        EXPECT_EQ(co_await windows->Show(created.Handle, NativeWindowShowMode::NoActivate), WindowOperationStatus::Completed);
        EXPECT_NE(windows->ResolveWindow(created.Handle)->GetSwapChain(), nullptr);
        // Simulate OS-driven minimize/restore, which bypasses runtime mutation APIs.
        ::ShowWindow(static_cast<HWND>(windows->ResolveWindow(created.Handle)->GetNativeWindow()->GetNativeHandler()), SW_MINIMIZE);
        EXPECT_TRUE(windows->ResolveWindow(created.Handle)->IsMinimized());
        EXPECT_EQ(co_await windows->SetPosition(created.Handle, 40, 40), WindowOperationStatus::Deferred);
        EXPECT_FALSE(windows->ResolveWindow(created.Handle)->IsSwapChainPresentable());
        ::ShowWindow(static_cast<HWND>(windows->ResolveWindow(created.Handle)->GetNativeWindow()->GetNativeHandler()), SW_RESTORE);
        EXPECT_EQ(co_await windows->SetSize(created.Handle, 360, 240), WindowOperationStatus::Completed);
        EXPECT_TRUE(windows->ResolveWindow(created.Handle)->IsSwapChainPresentable());
        EXPECT_EQ(windows->ResolveWindow(created.Handle)->GetSwapChain()->GetDesc().Width, 360u);
        EXPECT_EQ(co_await windows->SetSize(_main, 340, 220), WindowOperationStatus::Completed);
        EXPECT_EQ(windows->ResolveWindow(_main)->GetSwapChain()->GetDesc().Width, 340u);
        const auto waitFrame = GetFrameTimeline().GetFrameIndex();
        co_await GetFrameTimeline().Wait();
        EXPECT_GT(GetFrameTimeline().GetFrameIndex(), waitFrame);
        EXPECT_EQ(co_await windows->SetOwner(created.Handle, _main), WindowOperationStatus::Completed);
        EXPECT_EQ(co_await windows->SetPosition(created.Handle, 50, 50), WindowOperationStatus::Completed);
        EXPECT_EQ(co_await windows->SetAlpha(created.Handle, 1.0f), WindowOperationStatus::Completed);
        EXPECT_EQ(co_await windows->SetDecorated(created.Handle, false), WindowOperationStatus::Completed);
        EXPECT_EQ(co_await windows->SetShowInTaskbar(created.Handle, false), WindowOperationStatus::Completed);
        EXPECT_EQ(co_await windows->SetTopMost(created.Handle, false), WindowOperationStatus::Completed);
        EXPECT_EQ(co_await windows->SetPresentMode(render::PresentMode::FIFO), WindowOperationStatus::Completed);
        auto released = co_await windows->ReleaseSwapChain(created.Handle);
        EXPECT_EQ(released.Status, WindowOperationStatus::Completed);
        EXPECT_NE(released.SwapChain, nullptr);
        released.SwapChain.reset();
        EXPECT_EQ(windows->ResolveWindow(created.Handle)->GetSwapChain(), nullptr);
        EXPECT_EQ(co_await windows->AttachSwapChain(created.Handle, swapchain), WindowOperationStatus::Completed);
        EXPECT_EQ(co_await windows->DetachSwapChain(created.Handle), WindowOperationStatus::Completed);
        EXPECT_EQ(co_await windows->DestroyWindow(created.Handle), WindowOperationStatus::Completed);
        _surface.disconnect();
        EXPECT_EQ(co_await windows->DestroyWindow(_main), WindowOperationStatus::Completed);
        Completed = true;
        const auto result = co_await AwaitWithStopToken(windows->SetSize(_main, 320, 200), {});
        ShutdownCanceled = !result.has_value();
    }

    WindowHandle _main;
    sigslot::scoped_connection _surface;
    std::atomic_bool _recording{false};
    std::atomic<uint32_t> _recorded{0};
    uint32_t _updates{0};
    TaskScope _tasks;
};

void RunWindowMutations(render::RenderBackend backend, bool threaded) {
    test::RuntimeLogCapture logs;
    WindowMutationApp app;
    auto run = test::RunApplication(app, {
        .FlightDataCount = 2,
        .Window = WindowOptions{.Title = "Window coroutine lifecycle", .Width = 320, .Height = 200},
        .Gpu = GpuOptions{
            .Backend = backend,
            .EnableValidation = true,
            .EnableSynchronizationValidation = true,
            .Multithreaded = threaded,
            .EnableFrameProfiler = false,
            .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
            .PresentMode = render::PresentMode::FIFO,
        },
        .Render = std::nullopt,
        .World = std::nullopt,
        .Asset = std::nullopt,
    });
    if (test::CanSkipRuntimeStartup(backend, run.Startup)) GTEST_SKIP() << run.Startup.Reason;
    ASSERT_EQ(run.Startup.Status, RuntimeStartupStatus::Started) << run.Startup.Reason;
    ASSERT_EQ(run.ExitCode, 0);
    EXPECT_TRUE(app.Completed);
    EXPECT_TRUE(app.ShutdownCanceled);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(RuntimeFoundation, D3D12SingleThreadWindowOperations) { RunWindowMutations(render::RenderBackend::D3D12, false); }
TEST(RuntimeFoundation, D3D12ThreadedWindowOperations) { RunWindowMutations(render::RenderBackend::D3D12, true); }
TEST(RuntimeFoundation, VulkanSingleThreadWindowOperations) { RunWindowMutations(render::RenderBackend::Vulkan, false); }
TEST(RuntimeFoundation, VulkanThreadedWindowOperations) { RunWindowMutations(render::RenderBackend::Vulkan, true); }
#endif

}  // namespace
}  // namespace radray
