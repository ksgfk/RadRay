#include "runtime_test_support.h"
#include "gpu_runtime_test_support.h"

#include <gtest/gtest.h>
#include <semaphore>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/asset_manager.h>

#if defined(_WIN32)

namespace radray {
namespace {

ApplicationRuntimeDescriptor LoopDescriptor(render::RenderBackend backend, bool threaded, uint32_t flights) {
    return {.FlightDataCount = flights,
            .Window = WindowOptions{.Title = "Loop protocol", .Width = 80, .Height = 60},
            .Gpu = GpuOptions{.Backend = backend, .EnableValidation = true, .EnableSynchronizationValidation = true, .Multithreaded = threaded, .EnableFrameProfiler = false, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO},
            .Render = std::nullopt,
            .World = std::nullopt,
            .Asset = std::nullopt};
}

enum class MessageWaitPhase { Slot,
                              Maintenance,
                              Stop };

class CpuMessageWaitApp final : public Application {
public:
    explicit CpuMessageWaitApp(MessageWaitPhase phase) : _phase(phase) {}
    bool SentMessageHandled{false}, MutationDelivered{false};

protected:
    void OnInit() override {
        auto* native = GetWindowManager()->GetMainWindow()->GetNativeWindow();
        _hwnd = static_cast<HWND>(native->GetNativeHandler());
        native->EventBeforeSurfaceChange().connect([this] { EXPECT_FALSE(_recording.load()); });
    }
    void OnUpdate(const AppUpdateContext&) override {
        ++_updates;
        if (_updates == 2 && _phase != MessageWaitPhase::Slot) {
            EXPECT_TRUE(_recordStarted.try_acquire_for(std::chrono::seconds{5}));
            if (_phase == MessageWaitPhase::Maintenance)
                _tasks.Spawn(Resize());
            else
                RequestExit();
            _operationReady.release();
        }
        if (_updates == 3 && _phase == MessageWaitPhase::Slot) RequestExit();
        if (_updates > 10) RequestExit();
    }
    void OnRender(AppFrameContext&) override {
        if (_rendered++ != 0) return;
        _recording = true;
        _recordStarted.release();
        if (_phase != MessageWaitPhase::Slot) EXPECT_TRUE(_operationReady.try_acquire_for(std::chrono::seconds{5}));
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        DWORD_PTR result{};
        SentMessageHandled = ::SendMessageTimeoutW(_hwnd, WM_NULL, 0, 0, SMTO_BLOCK, 3000, &result) != 0;
        EXPECT_TRUE(SentMessageHandled);
        EXPECT_FALSE(GetFrameTimeline().IsStopping());
        _recording = false;
    }
    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        EXPECT_TRUE(_completions.insert(completion.FrameSerial).second);
    }
    void OnShutdown() override {
        EXPECT_TRUE(SentMessageHandled);
        EXPECT_EQ(_completions.size(), GetFrameTimeline().GetFrameIndex());
    }

private:
    task<void> Resize() {
        auto* windows = GetWindowManager().Get();
        EXPECT_EQ(co_await windows->SetSize(windows->GetMainWindow()->GetHandle(), 160, 120), WindowOperationStatus::Completed);
        EXPECT_TRUE(SentMessageHandled);
        MutationDelivered = true;
        RequestExit();
    }
    MessageWaitPhase _phase;
    HWND _hwnd{};
    std::binary_semaphore _recordStarted{0}, _operationReady{0};
    std::atomic_bool _recording{false};
    uint32_t _updates{0}, _rendered{0};
    unordered_set<uint64_t> _completions;
    TaskScope _tasks;
};

void RunCpuMessageWait(render::RenderBackend backend) {
    for (auto phase : {MessageWaitPhase::Slot, MessageWaitPhase::Maintenance, MessageWaitPhase::Stop}) {
        SCOPED_TRACE(static_cast<int>(phase));
        test::RuntimeLogCapture logs;
        CpuMessageWaitApp app{phase};
        auto result = test::RunApplication(app, LoopDescriptor(backend, true, phase == MessageWaitPhase::Slot ? 1 : 2));
        if (test::CanSkipRuntimeStartup(backend, result.Startup)) GTEST_SKIP() << result.Startup.Reason;
        ASSERT_EQ(result.Startup.Status, RuntimeStartupStatus::Started) << result.Startup.Reason;
        EXPECT_EQ(result.ExitCode, 0);
        EXPECT_TRUE(app.SentMessageHandled);
        EXPECT_EQ(app.MutationDelivered, phase == MessageWaitPhase::Maintenance);
        EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    }
}

TEST(RuntimeFoundation, D3D12CpuHandoffWaitServicesSentMessages) { RunCpuMessageWait(render::RenderBackend::D3D12); }
TEST(RuntimeFoundation, VulkanCpuHandoffWaitServicesSentMessages) { RunCpuMessageWait(render::RenderBackend::Vulkan); }

enum class NativeModalKind { Move,
                             Size,
                             Menu };

class NativeModalApp final : public Application {
public:
    explicit NativeModalApp(NativeModalKind kind) : _kind(kind) {}
    ~NativeModalApp() noexcept override {
        _finished.release();
        if (_watchdog.joinable()) _watchdog.join();
    }

protected:
    void OnInit() override {
        auto* native = GetWindowManager()->GetMainWindow()->GetNativeWindow();
        _hwnd = static_cast<HWND>(native->GetNativeHandler());
        const auto timer = ::SetTimer(_hwnd, reinterpret_cast<UINT_PTR>(this), 10,
                                      [](HWND owner, UINT, UINT_PTR id, DWORD) {
                                          ::KillTimer(owner, id);
                                          reinterpret_cast<NativeModalApp*>(id)->EnterModal();
                                      });
        EXPECT_NE(timer, 0u);
        if (timer == 0) RequestExit();
        _watchdog = std::thread([this] {
            if (!_finished.try_acquire_for(std::chrono::seconds{5})) {
                _neededFallback = true;
                RequestExit();
                ::PostMessageW(_hwnd, WM_CANCELMODE, 0, 0);
                ::PostMessageW(_hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
            }
        });
    }
    void OnUpdate(const AppUpdateContext&) override {
        EXPECT_EQ(GetFrameTimeline().GetFrameIndex(), _updates++);
        if (_nativeModalActive) {
            ++_modalUpdates;
            if (_modalRenders.load() >= 6 && !_leaving) {
                _leaving = true;
                if (_kind == NativeModalKind::Menu)
                    RequestExit();
                else
                    EXPECT_TRUE(::PostMessageW(_hwnd, WM_KEYDOWN, VK_RETURN, 0));
            }
        } else if (_modalReturned) {
            RequestExit();
        }
    }
    void OnRender(AppFrameContext& ctx) override {
        auto* window = GetWindowManager()->GetMainWindow();
        auto target = ctx.AcquireWindow(window);
        if (!target) return;
        auto* commands = ctx.AllocateCommandBuffer();
        const render::ResourceBarrierDescriptor before = render::BarrierTextureDescriptor{
            .Target = target->BackBuffer, .Before = window->GetBackBufferState(target->BackBufferIndex), .After = render::TextureState::RenderTarget};
        commands->ResourceBarrier(std::span{&before, 1});
        const auto desc = target->BackBuffer->GetDesc();
        const render::ColorClearValue clear{{.1f, .2f, .3f, 1}};
        auto encoder = commands->BeginRenderPass({array{render::RenderPassColorAttachment{target->BackBufferView, render::LoadAction::Clear, render::StoreAction::Store, clear}}, {}, desc.Width, desc.Height, 1, {}, "Native modal clear"});
        ASSERT_TRUE(encoder);
        commands->EndRenderPass(encoder.Release());
        const render::ResourceBarrierDescriptor after = render::BarrierTextureDescriptor{
            .Target = target->BackBuffer, .Before = render::TextureState::RenderTarget, .After = render::TextureState::Present};
        commands->ResourceBarrier(std::span{&after, 1});
        ctx.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}}, std::move(target));
        if (ctx.IsInModalLoop()) ++_modalRenders;
    }
    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        EXPECT_TRUE(_completedSerials.insert(completion.FrameSerial).second);
    }
    void OnShutdown() override {
        ::KillTimer(_hwnd, reinterpret_cast<UINT_PTR>(this));
        EXPECT_TRUE(_modalReturned);
        EXPECT_FALSE(_nativeModalActive);
        EXPECT_FALSE(_neededFallback.load());
        EXPECT_GE(_modalUpdates, 6u);
        EXPECT_GE(_modalRenders.load(), 6u);
        EXPECT_EQ(_completedSerials.size(), GetFrameTimeline().GetFrameIndex());
    }

private:
    void EnterModal() {
        _nativeModalActive = true;
        // Only the window's real WM_TIMER path advances frames inside these native loops.
        if (_kind == NativeModalKind::Menu) {
            HMENU menu = ::CreatePopupMenu();
            EXPECT_NE(menu, nullptr);
            if (menu != nullptr) {
                EXPECT_TRUE(::AppendMenuW(menu, MF_STRING, 1, L"Modal rendering test"));
                RECT rect{};
                ::GetWindowRect(_hwnd, &rect);
                ::TrackPopupMenu(menu, TPM_RETURNCMD, rect.left + 20, rect.top + 20, 0, _hwnd, nullptr);
                ::DestroyMenu(menu);
            }
        } else {
            ::SendMessageW(_hwnd, WM_SYSCOMMAND, _kind == NativeModalKind::Move ? SC_MOVE : SC_SIZE | WMSZ_BOTTOMRIGHT, 0);
        }
        _nativeModalActive = false;
        _modalReturned = true;
        EXPECT_GE(_modalRenders.load(), 6u);
    }

    NativeModalKind _kind;
    HWND _hwnd{};
    std::binary_semaphore _finished{0};
    std::thread _watchdog;
    std::atomic_bool _neededFallback{false};
    std::atomic<uint32_t> _modalRenders{0};
    uint64_t _updates{0};
    uint32_t _modalUpdates{0};
    bool _nativeModalActive{false}, _modalReturned{false}, _leaving{false};
    unordered_set<uint64_t> _completedSerials;
};

void RunNativeModal(render::RenderBackend backend) {
    for (bool threaded : {false, true}) {
        for (auto kind : {NativeModalKind::Move, NativeModalKind::Size, NativeModalKind::Menu}) {
            SCOPED_TRACE(fmt::format("threaded={} kind={}", threaded, static_cast<int>(kind)));
            test::RuntimeLogCapture logs;
            NativeModalApp app{kind};
            const auto result = test::RunApplication(app, LoopDescriptor(backend, threaded, 2));
            if (test::CanSkipRuntimeStartup(backend, result.Startup)) GTEST_SKIP() << result.Startup.Reason;
            ASSERT_EQ(result.Startup.Status, RuntimeStartupStatus::Started) << result.Startup.Reason;
            EXPECT_EQ(result.ExitCode, 0);
            EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
        }
    }
}

TEST(RuntimeFoundation, D3D12NativeModalTimerRendersAndReturnsToOuterLoop) { RunNativeModal(render::RenderBackend::D3D12); }
TEST(RuntimeFoundation, VulkanNativeModalTimerRendersAndReturnsToOuterLoop) { RunNativeModal(render::RenderBackend::Vulkan); }

bool SignalHostGate(render::Fence* gate) {
#if defined(RADRAY_ENABLE_D3D12)
    if (auto* fence = dynamic_cast<render::d3d12::FenceD3D12*>(gate)) return SUCCEEDED(fence->_fence->Signal(1));
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (auto* fence = dynamic_cast<render::vulkan::FenceVulkan*>(gate)) {
        const VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, fence->_fence->_semaphore, 1};
        return fence->_device->_ftb.vkSignalSemaphore(fence->_device->_device, &signal) == VK_SUCCESS;
    }
#endif
    return false;
}

class QueueTailApp final : public Application {
protected:
    void OnInit() override {
        _gate = GetGpuSystem()->GetDevice()->CreateFence().Unwrap();
        _hwnd = static_cast<HWND>(GetWindowManager()->GetMainWindow()->GetNativeWindow()->GetNativeHandler());
    }
    void OnUpdate(const AppUpdateContext&) override {
        ++_updates;
        EXPECT_EQ(_updates, 1u);
        if (_updates > 1) RequestExit();
    }
    void OnRenderFrameComplete(const FlightCompletion&) override {
        ASSERT_FALSE(_helper.joinable());
        EXPECT_GT(GetGpuSystem()->GetMainFence()->GetCompletedValue(), 0u);
        // F=1: the producer has handed off, and the final flight fence is already retired.
        // This independent queue tail is deliberately not represented by another flight fence.
        render::Fence* waits[]{_gate.get()};
        uint64_t values[]{1};
        GetGpuSystem()->GetMainQueue()->Submit({.WaitFences = waits, .WaitValues = values});
        RequestExit();
        _helper = std::thread([this] {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            EXPECT_FALSE(_shutdown.load());
            DWORD_PTR result{};
            EXPECT_NE(::SendMessageTimeoutW(_hwnd, WM_NULL, 0, 0, SMTO_BLOCK, 3000, &result), 0);
            EXPECT_FALSE(_shutdown.load());
            _released = true;
            EXPECT_TRUE(SignalHostGate(_gate.get()));
        });
    }
    void OnShutdown() override {
        _shutdown = true;
        EXPECT_TRUE(_released.load());
        if (_helper.joinable()) _helper.join();
        EXPECT_EQ(GetFrameTimeline().GetFrameIndex(), 1u);
        _gate.reset();
    }

private:
    unique_ptr<render::Fence> _gate;
    std::thread _helper;
    std::atomic_bool _shutdown{false}, _released{false};
    uint32_t _updates{0};
    HWND _hwnd{};
};

void RunQueueTail(render::RenderBackend backend) {
    for (bool threaded : {false, true}) {
        SCOPED_TRACE(threaded);
        QueueTailApp app;
        auto desc = LoopDescriptor(backend, threaded, 1);
        // Native host-signaled GPU gates bypass validation semaphore bookkeeping, as in existing gate tests.
        desc.Gpu->EnableValidation = false;
        desc.Gpu->EnableSynchronizationValidation = false;
        desc.Gpu->RhiValidation.IsEnabled = false;
        auto result = test::RunApplication(app, desc);
        if (test::CanSkipRuntimeStartup(backend, result.Startup)) GTEST_SKIP() << result.Startup.Reason;
        ASSERT_EQ(result.Startup.Status, RuntimeStartupStatus::Started) << result.Startup.Reason;
        EXPECT_EQ(result.ExitCode, 0);
    }
}

TEST(RuntimeFoundation, D3D12ShutdownDrainsQueueTailAfterFinalFlightFence) { RunQueueTail(render::RenderBackend::D3D12); }
TEST(RuntimeFoundation, VulkanShutdownDrainsQueueTailAfterFinalFlightFence) { RunQueueTail(render::RenderBackend::Vulkan); }

class GpuStopApp final : public Application {
public:
    GpuStopApp(bool close, bool maintenance) : _close(close), _maintenance(maintenance) {}

protected:
    void OnInit() override {
        _gate = GetGpuSystem()->GetDevice()->CreateFence().Unwrap();
        auto* native = GetWindowManager()->GetMainWindow()->GetNativeWindow();
        _hwnd = static_cast<HWND>(native->GetNativeHandler());
        native->EventTextInput().connect([this](std::string_view) { ++_inputs; });
        native->EventCloseRequested().connect([this] { ++_closes; EXPECT_TRUE(_released.load()); });
        native->EventBeforeSurfaceChange().connect([this] { EXPECT_TRUE(_released.load()); });
        GetWindowManager()->EventModalLoopTick().connect([this](NativeWindow* window) {
            if (!_close || !window->ShouldClose() || _modalStopObserved) return;
            _modalStopObserved = true;
            // Keep an actual native dispatch stack alive while stop progresses through modal ticks.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (_completions == 0 && std::chrono::steady_clock::now() < deadline) {
                GetWindowManager()->EventModalLoopTick()(window);
                MSG message{};
                for (uint32_t i = 0; i < 256 && ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
                    ::TranslateMessage(&message);
                    ::DispatchMessageW(&message);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            EXPECT_EQ(_completions, 1u);
            EXPECT_FALSE(_shutdown.load());
        });
    }
    void OnUpdate(const AppUpdateContext& context) override {
        EXPECT_EQ(++_updates, 1u);
        auto owner = make_shared<int>(1);
        _owner = owner;
        GetGpuSystem()->RetainForFrameGT(context.FlightIndex, std::move(owner));
        if (_maintenance) _tasks.Spawn(Resize());
    }
    void OnRender(AppFrameContext& context) override {
        render::Fence* waits[]{_gate.get()};
        uint64_t values[]{1};
        context.ReturnCommandBuffers({.WaitFences = waits, .WaitValues = values});
        context.SubmitFrame();
        _helper = std::thread([this] {
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
            const auto send = [this](UINT message, WPARAM value = 0) {
                DWORD_PTR result{};
                EXPECT_NE(::SendMessageTimeoutW(_hwnd, message, value, 0, SMTO_BLOCK, 3000, &result), 0);
            };
            // A real native modal timer runs while the old GPU slot is unavailable.
            send(WM_ENTERSIZEMOVE);
            send(WM_CHAR, 'a');
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
            EXPECT_EQ(_updates.load(), 1u);
            EXPECT_EQ(_inputs.load(), 0u);
            if (_close)
                send(WM_CLOSE);
            else
                RequestExit();
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
            send(WM_CHAR, 'b');
            send(WM_NULL);
            send(WM_EXITSIZEMOVE);
            EXPECT_FALSE(_shutdown.load());
            EXPECT_FALSE(_owner.expired());
            EXPECT_EQ(_inputs.load(), 0u);
            EXPECT_EQ(_updates.load(), 1u);
            _released = true;
            // This runs even after a failed EXPECT, so the failure cannot strand the GPU queue.
            EXPECT_TRUE(SignalHostGate(_gate.get()));
        });
    }
    void OnRenderFrameComplete(const FlightCompletion&) override {
        EXPECT_TRUE(_released.load());
        EXPECT_TRUE(_owner.expired());
        ++_completions;
    }
    void OnShutdown() override {
        _shutdown = true;
        EXPECT_TRUE(_released.load());
        if (_helper.joinable()) _helper.join();
        EXPECT_EQ(_inputs.load(), 0u);
        EXPECT_EQ(_completions, 1u);
        EXPECT_EQ(_closes, _close ? 1u : 0u);
        if (_close) EXPECT_TRUE(_modalStopObserved);
        EXPECT_FALSE(_mutated);
        _gate.reset();
    }

private:
    task<void> Resize() {
        auto* windows = GetWindowManager().Get();
        co_await windows->SetSize(windows->GetMainWindow()->GetHandle(), 160, 120);
        _mutated = true;
    }
    bool _close, _maintenance, _mutated{false}, _modalStopObserved{false};
    TaskScope _tasks;
    unique_ptr<render::Fence> _gate;
    weak_ptr<int> _owner;
    std::thread _helper;
    std::atomic_bool _released{false}, _shutdown{false};
    std::atomic<uint32_t> _updates{0}, _inputs{0};
    uint32_t _closes{0}, _completions{0};
    HWND _hwnd{};
};

void RunGpuStop(render::RenderBackend backend) {
    for (bool threaded : {false, true}) {
        for (int scenario : {0, 1, 2}) {
            SCOPED_TRACE(fmt::format("threaded={} scenario={}", threaded, scenario));
            GpuStopApp app{scenario != 0, scenario == 2};
            auto desc = LoopDescriptor(backend, threaded, 1);
            desc.Gpu->EnableValidation = false;
            desc.Gpu->EnableSynchronizationValidation = false;
            desc.Gpu->RhiValidation.IsEnabled = false;
            auto result = test::RunApplication(app, desc);
            if (test::CanSkipRuntimeStartup(backend, result.Startup)) GTEST_SKIP() << result.Startup.Reason;
            ASSERT_EQ(result.Startup.Status, RuntimeStartupStatus::Started) << result.Startup.Reason;
            EXPECT_EQ(result.ExitCode, 0);
        }
    }
}

TEST(RuntimeFoundation, D3D12GpuStallModalCloseAndMaintenanceStop) { RunGpuStop(render::RenderBackend::D3D12); }
TEST(RuntimeFoundation, VulkanGpuStallModalCloseAndMaintenanceStop) { RunGpuStop(render::RenderBackend::Vulkan); }

class LoopMatrixApp final : public Application {
public:
    explicit LoopMatrixApp(uint32_t flights) : _owners(flights) {}

protected:
    void OnInit() override {
        auto* window = GetWindowManager()->GetMainWindow()->GetNativeWindow();
        window->EventCloseRequested().connect([this] { ++_closes; });
    }
    void OnUpdate(const AppUpdateContext& context) override {
        EXPECT_TRUE(_owners[context.FlightIndex].expired());
        if (++_updates == 20) {
            RequestExit();
            return;
        }
        auto owner = make_shared<int>(1);
        _owners[context.FlightIndex] = owner;
        GetGpuSystem()->RetainForFrameGT(context.FlightIndex, std::move(owner));
    }
    void OnRender(AppFrameContext& context) override {
        EXPECT_FALSE(_owners[context.FlightIndex()].expired());
    }
    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        EXPECT_EQ(std::this_thread::get_id(), _gt);
        EXPECT_TRUE(_serials.insert(completion.FrameSerial).second);
        EXPECT_GT(completion.FrameSerial, _lastSerial);
        _lastSerial = completion.FrameSerial;
        EXPECT_TRUE(_owners[completion.FlightIndex].expired());
    }
    void OnShutdown() override {
        EXPECT_EQ(_serials.size(), 19u);
        for (const auto& owner : _owners) EXPECT_TRUE(owner.expired());
        EXPECT_EQ(_closes, 0u);
    }

private:
    const std::thread::id _gt{std::this_thread::get_id()};
    vector<weak_ptr<int>> _owners;
    unordered_set<uint64_t> _serials;
    uint64_t _lastSerial{0};
    uint32_t _updates{0}, _closes{0};
};

void RunLoopMatrix(render::RenderBackend backend) {
    for (bool threaded : {false, true}) {
        for (uint32_t flights : {1u, 2u, 3u, 4u}) {
            SCOPED_TRACE(fmt::format("threaded={} flights={}", threaded, flights));
            test::RuntimeLogCapture logs;
            LoopMatrixApp app{flights};
            auto result = test::RunApplication(app, LoopDescriptor(backend, threaded, flights));
            if (test::CanSkipRuntimeStartup(backend, result.Startup)) GTEST_SKIP() << result.Startup.Reason;
            ASSERT_EQ(result.Startup.Status, RuntimeStartupStatus::Started) << result.Startup.Reason;
            EXPECT_EQ(result.ExitCode, 0);
            EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
        }
    }
}

TEST(RuntimeFoundation, D3D12FlightCountAndOwnerRetirementMatrix) { RunLoopMatrix(render::RenderBackend::D3D12); }
TEST(RuntimeFoundation, VulkanFlightCountAndOwnerRetirementMatrix) { RunLoopMatrix(render::RenderBackend::Vulkan); }

class TerminalInputApp final : public Application {
public:
    explicit TerminalInputApp(bool close) : _close(close) {}
    uint32_t Closed{0}, ShutdownCount{0}, Updates{0};

protected:
    void OnInit() override {
        auto* native = GetWindowManager()->GetMainWindow()->GetNativeWindow();
        const auto hwnd = static_cast<HWND>(native->GetNativeHandler());
        GetWindowManager()->EventModalLoopTick()(native);
        EXPECT_EQ(Updates, 0u);
        native->EventTextInput().connect([this](std::string_view) {
            ++_inputs;
            RequestExit();
            // Public shutdown requests made in a callback cannot destroy its dispatch stack.
            Shutdown({});
            EXPECT_EQ(ShutdownCount, 0u);
        });
        native->EventCloseRequested().connect([this] { ++Closed; EXPECT_EQ(ShutdownCount, 0u); });
        if (_close) {
            ::SendMessageW(hwnd, WM_CLOSE, 0, 0);
            ::SendMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        ::SendMessageW(hwnd, WM_CHAR, 'a', 0);
        ::SendMessageW(hwnd, WM_CHAR, 'b', 0);
        EXPECT_EQ(_inputs, 0u);
        EXPECT_EQ(Closed, 0u);
    }
    void OnUpdate(const AppUpdateContext&) override {
        ++Updates;
        RequestExit();
    }
    void OnShutdown() override {
        ++ShutdownCount;
        EXPECT_EQ(Closed, _close ? 1u : 0u);
        EXPECT_EQ(_inputs, _close ? 0u : 1u);
    }

private:
    bool _close;
    uint32_t _inputs{0};
};

TEST(RuntimeFoundation, CpuOnlyTerminalEventsAndDeferredShutdown) {
    for (bool close : {false, true}) {
        TerminalInputApp app{close};
        ApplicationRuntimeDescriptor desc{.Window = WindowOptions{.Title = "Terminal input", .Width = 80, .Height = 60},
                                          .Gpu = std::nullopt,
                                          .Render = std::nullopt,
                                          .World = std::nullopt,
                                          .Asset = std::nullopt};
        ASSERT_EQ(app.Run(desc), 0);
        EXPECT_EQ(app.Updates, 0u);
        EXPECT_EQ(app.ShutdownCount, 1u);
    }
}

}  // namespace
}  // namespace radray

#endif
