// Test coverage and temporary exclusions: docs/guide/build-test.md
#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "gpu_runtime_test_support.h"

#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>

#ifdef CreateWindow
#undef CreateWindow
#endif

namespace radray {
namespace {

void MultiWindowBufferBarrier(render::CommandBuffer* commands, render::Buffer* buffer, render::BufferStates before, render::BufferStates after) {
    const render::ResourceBarrierDescriptor barrier = render::BarrierBufferDescriptor{.Target = buffer, .Before = before, .After = after};
    commands->ResourceBarrier(std::span{&barrier, 1});
}

class MultiWindowApp final : public Application {
public:
    explicit MultiWindowApp(bool lifecycle) : _lifecycle(lifecycle) {}

    bool Completed{false};

protected:
    void OnInit() override {
        _vulkan = GetGpuSystem()->GetDevice()->GetBackend() == render::RenderBackend::Vulkan;
        _traceWsi = std::getenv("RADRAY_TEST_WSI_TRACE") != nullptr;
        _tasks.Spawn(RunScenario());
    }

    void OnUpdate(const AppUpdateContext&) override {
        if (++_updates == 600) {
            ADD_FAILURE() << "multi-window scenario did not finish";
            _tasks.RequestStop();
            test::CloseMainWindow(*this);
        }
    }

    void OnRender(AppFrameContext& ctx) override {
        const uint32_t phase = _phase.load();
        if (phase == 0) return;
        SCOPED_TRACE(fmt::format("phase={}, serial={}, flight={}", phase, ctx.FrameSerial(), ctx.FlightIndex()));
        auto probe = make_shared<FrameProbe>();
        probe->Phase = phase;
        probe->Flight = ctx.FlightIndex();
        auto* device = ctx.GetDevice();
        probe->Upload = device->CreateBuffer({.Size = 16, .Memory = render::MemoryType::Upload, .Usage = render::BufferUse::CopySource | render::BufferUse::MapWrite}).Unwrap();
        probe->Readback = device->CreateBuffer({.Size = 16, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
        probe->Fence = device->CreateFence().Unwrap();
        const uint32_t base = static_cast<uint32_t>(ctx.FrameSerial()) * 10;
        const uint32_t values[]{0, base + 1, base + 2, base + 3};
        {
            ScopedBufferMap map{probe->Upload.get(), {0, 16}};
            ASSERT_TRUE(map);
            std::memcpy(map.Data(), values, sizeof(values));
            probe->Upload->FlushMappedRange({0, 16});
        }

        auto* windows = GetWindowManager().Get();
        EXPECT_EQ(windows->GetWindowCount(), phase == 9 ? 2u : 3u);
        vector<AppFrameTarget> targets;
        vector<uint32_t> identities;
        uint32_t acquiredMask = 0;
        for (size_t i = 0; i < windows->GetWindowCount(); ++i) {
            auto* window = windows->GetWindow(i);
            const uint32_t identity = window->IsMainWindow() ? 0 : (window->GetHandle().Id == _mutableId.load() ? 1 : 2);
            const bool unavailable = identity == 1 && (phase == 3 || phase == 5 || phase == 7);
            auto target = ctx.AcquireWindow(window);
            if (unavailable) {
                EXPECT_FALSE(target);
                if (phase == 5 || phase == 7) EXPECT_EQ(window->GetSwapChain(), nullptr);
            } else {
                EXPECT_TRUE(target) << "window=" << identity;
            }
            if (!target) continue;
#if defined(RADRAY_ENABLE_VULKAN)
            if (_vulkan && _traceWsi) {
                auto* chain = render::vulkan::CastVkObject(window->GetSwapChain());
                const auto& acquired = chain->_outstandingAcquire;
                RADRAY_INFO_LOG("WSI phase={} serial={} flight={} window={} chain={} image={} acquire={} present={}",
                                phase, ctx.FrameSerial(), ctx.FlightIndex(), identity, fmt::ptr(chain->_swapchain),
                                target->BackBufferIndex, fmt::ptr(acquired.waitToDraw->_semaphore->_semaphore),
                                fmt::ptr(acquired.readyToPresent->_semaphore->_semaphore));
            }
#endif
            const auto desc = target->BackBuffer->GetDesc();
            EXPECT_EQ(desc.Width, identity == 0 ? 280u : (identity == 2 ? 400u : (phase >= 2 && phase <= 8 ? 360u : 320u)));
            EXPECT_EQ(desc.Height, identity == 0 ? 200u : (identity == 2 ? 280u : (phase >= 2 && phase <= 8 ? 260u : 240u)));
            EXPECT_EQ(desc.Format, render::TextureFormat::BGRA8_UNORM);
            const uint32_t requestedBuffers = identity == 1 ? 2u : 3u;
            if (_vulkan) {
                EXPECT_GE(window->GetSwapChain()->GetDesc().BackBufferCount, requestedBuffers);
            } else {
                EXPECT_EQ(window->GetSwapChain()->GetDesc().BackBufferCount, requestedBuffers);
            }
            EXPECT_NE(target->BackBufferView, nullptr);
            for (const auto& other : targets) {
                EXPECT_NE(target->BackBuffer, other.BackBuffer);
                EXPECT_NE(target->BackBufferView, other.BackBufferView);
                EXPECT_NE(window->GetSwapChain(), other.Window->GetSwapChain());
            }
            acquiredMask |= 1u << identity;
            identities.push_back(identity);
            targets.push_back(std::move(*target));
        }
        EXPECT_EQ(acquiredMask, phase == 3 || phase == 5 || phase == 7 || phase == 9 ? 5u : 7u);

        // Allocate in acquire order; return in alternating forward/reverse order.
        vector<render::CommandBuffer*> drawCommands;
        vector<render::CommandBuffer*> copyCommands;
        for (size_t i = 0; i < targets.size(); ++i) {
            drawCommands.push_back(ctx.AllocateCommandBuffer());
            copyCommands.push_back(ctx.AllocateCommandBuffer());
        }
        auto* prefix = ctx.AllocateCommandBuffer();
        if (_vulkan) {
            MultiWindowBufferBarrier(prefix, probe->Upload.get(), render::BufferState::HostWrite, render::BufferState::CopySource);
            MultiWindowBufferBarrier(prefix, probe->Readback.get(), render::BufferState::Undefined, render::BufferState::CopyDestination);
        }
        for (uint64_t offset = 0; offset < 16; offset += 4) {
            prefix->CopyBufferToBuffer(probe->Readback.get(), offset, probe->Upload.get(), 0, 4);
        }
        ReturnBatch(ctx, *probe, std::span{&prefix, 1});
        for (size_t step = 0; step < targets.size(); ++step) {
            const size_t index = ctx.FrameSerial() % 2 == 0 ? step : targets.size() - 1 - step;
            const uint32_t identity = identities[index];
            RecordClear(drawCommands[index], targets[index], identity);
            auto* copy = copyCommands[index];
            if (_vulkan) MultiWindowBufferBarrier(copy, probe->Readback.get(), render::BufferState::CopyDestination, render::BufferState::CopyDestination);
            const uint64_t offset = (identity + 1) * 4;
            copy->CopyBufferToBuffer(probe->Readback.get(), offset, probe->Upload.get(), offset, 4);
            copy->CopyBufferToBuffer(probe->Readback.get(), 0, probe->Upload.get(), offset, 4);
            probe->Expected[identity + 1] = values[identity + 1];
            probe->Expected[0] = values[identity + 1];
            render::CommandBuffer* batch[]{drawCommands[index], copy};
            ReturnBatch(ctx, *probe, batch, std::move(targets[index]));
            // A pure synchronization batch between window submissions must survive unchanged.
            ReturnBatch(ctx, *probe, {});
        }
        auto* suffix = ctx.AllocateCommandBuffer();
        if (_vulkan) MultiWindowBufferBarrier(suffix, probe->Readback.get(), render::BufferState::CopyDestination, render::BufferState::HostRead);
        ReturnBatch(ctx, *probe, std::span{&suffix, 1});
        {
            std::lock_guard lock{_probeMutex};
            EXPECT_TRUE(_pending.emplace(ctx.FrameSerial(), probe).second);
        }
        ++_recorded;
        if (ctx.FrameSerial() % 2 == 0) {
            ++_explicitSubmissions;
            ctx.SubmitFrame();
        } else {
            ++_automaticSubmissions;
        }
    }

    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        EXPECT_EQ(std::this_thread::get_id(), _gameThread);
        EXPECT_TRUE(_completedSerials.insert(completion.FrameSerial).second);
        shared_ptr<FrameProbe> probe;
        {
            std::lock_guard lock{_probeMutex};
            const auto found = _pending.find(completion.FrameSerial);
            if (found == _pending.end()) return;
            probe = std::move(found->second);
            _pending.erase(found);
        }
        SCOPED_TRACE(fmt::format("completed phase={}, serial={}", probe->Phase, completion.FrameSerial));
        EXPECT_TRUE(completion.GpuWorkCompleted);
        EXPECT_EQ(completion.FlightIndex, probe->Flight);
        EXPECT_EQ(probe->Fence->GetCompletedValue(), probe->Signal);
        ScopedBufferMap map{probe->Readback.get(), {0, 16}};
        ASSERT_TRUE(map);
        probe->Readback->InvalidateMappedRange({0, 16});
        uint32_t actual[4]{};
        std::memcpy(actual, map.Data(), sizeof(actual));
        for (size_t i = 0; i < 4; ++i) EXPECT_EQ(actual[i], probe->Expected[i]) << "readback slot=" << i;
        ++_completedPhases[probe->Phase];
        ++_verified;
        _flights.insert(completion.FlightIndex);
    }

    void OnShutdown() override {
        _tasks.RequestStop();
        EXPECT_TRUE(Completed);
        EXPECT_EQ(_verified, _recorded.load());
        EXPECT_EQ(_completedSerials.size(), GetFrameTimeline().GetFrameIndex());
        EXPECT_EQ(_flights.size(), GetGpuSystem()->GetFlightDataCount());
        EXPECT_GT(_explicitSubmissions.load(), 0u);
        EXPECT_GT(_automaticSubmissions.load(), 0u);
        std::lock_guard lock{_probeMutex};
        EXPECT_TRUE(_pending.empty());
        _pending.clear();
    }

private:
    struct FrameProbe {
        unique_ptr<render::Buffer> Upload, Readback;
        unique_ptr<render::Fence> Fence;
        uint32_t Phase{0}, Flight{0};
        uint32_t Expected[4]{};
        uint64_t Signal{0};
    };

    void ReturnBatch(AppFrameContext& ctx, FrameProbe& probe, std::span<render::CommandBuffer*> commands, std::optional<AppFrameTarget> target = std::nullopt) {
        render::Fence* fences[]{probe.Fence.get()};
        uint64_t wait[]{probe.Signal};
        uint64_t signal[]{++probe.Signal};
        ctx.ReturnCommandBuffers({.CmdBuffers = commands,
                                  .SignalFences = fences,
                                  .SignalValues = signal,
                                  .WaitFences = wait[0] == 0 ? std::span<render::Fence*>{} : std::span{fences},
                                  .WaitValues = wait[0] == 0 ? std::span<uint64_t>{} : std::span{wait}},
                                 std::move(target));
    }

    void RecordClear(render::CommandBuffer* commands, AppFrameTarget& target, uint32_t identity) {
        auto* registry = GetRenderSystem()->GetRenderPassRegistry();
        const auto desc = target.BackBuffer->GetDesc();
        const render::RenderPassColorAttachmentDescriptor attachment{desc.Format, desc.SampleCount, render::LoadAction::Clear, render::StoreAction::Store};
        auto pass = registry->GetOrCreateRenderPass({std::span{&attachment, 1}, {}});
        ASSERT_TRUE(pass);
        auto* view = target.BackBufferView;
        auto framebuffer = registry->GetOrCreateFramebuffer({pass.Get(), std::span{&view, 1}, nullptr, desc.Width, desc.Height, 1});
        ASSERT_TRUE(framebuffer);
        const render::ResourceBarrierDescriptor before = render::BarrierTextureDescriptor{
            .Target = target.BackBuffer, .Before = target.Window->GetBackBufferState(target.BackBufferIndex), .After = render::TextureState::RenderTarget};
        commands->ResourceBarrier(std::span{&before, 1});
        render::ColorClearValue clear{{0, 0, 0, 1}};
        clear.Value[identity] = 1.0f;
        auto encoder = commands->BeginRenderPass({pass.Get(), framebuffer.Get(), std::span{&clear, 1}, {}, "Multi-window clear"});
        ASSERT_TRUE(encoder);
        commands->EndRenderPass(encoder.Release());
        const render::ResourceBarrierDescriptor after = render::BarrierTextureDescriptor{
            .Target = target.BackBuffer, .Before = render::TextureState::RenderTarget, .After = render::TextureState::Present};
        commands->ResourceBarrier(std::span{&after, 1});
    }

    task<void> ObservePhase(uint32_t phase) {
        _phase = phase;
        const uint32_t required = _lifecycle ? 4 : 12;
        while (_completedPhases[phase] < required) {
            co_await GetFrameTimeline().Wait();
            // Inspect completion counts in Update, after the completion callback batch.
            co_await GetScheduler().SwitchTo();
        }
        _phase = 0;
    }

    task<void> RunScenario() {
        auto* windows = GetWindowManager().Get();
        WindowSwapChainDescriptor swapchain{};
        swapchain.Format = render::TextureFormat::BGRA8_UNORM;
        swapchain.BackBufferCount = 2;
        WindowCreateDescriptor desc{};
        desc.Title = "Mutable auxiliary";
        desc.Width = 320;
        desc.Height = 240;
        desc.X = 320;
        desc.ActivateOnShow = false;
        auto created = co_await windows->CreateWindow(desc);
        EXPECT_EQ(created.Status, WindowOperationStatus::Completed);
        if (created.Status != WindowOperationStatus::Completed) co_return;
        auto auxiliary = created.Handle;
        _mutableId = auxiliary.Id;
        EXPECT_EQ(co_await windows->AttachSwapChain(auxiliary, swapchain), WindowOperationStatus::Completed);
        auto stableDesc = desc;
        stableDesc.Title = "Stable auxiliary";
        stableDesc.Width = 400;
        stableDesc.Height = 280;
        stableDesc.X = 680;
        const auto stable = co_await windows->CreateWindow(stableDesc);
        EXPECT_EQ(stable.Status, WindowOperationStatus::Completed);
        if (stable.Status != WindowOperationStatus::Completed) co_return;
        auto stableSwapchain = swapchain;
        stableSwapchain.BackBufferCount = 0;
        EXPECT_EQ(co_await windows->AttachSwapChain(stable.Handle, stableSwapchain), WindowOperationStatus::Completed);
        co_await ObservePhase(1);
        if (_lifecycle) {
            EXPECT_EQ(co_await windows->SetSize(auxiliary, 360, 260), WindowOperationStatus::Completed);
            co_await ObservePhase(2);
            EXPECT_EQ(co_await windows->DestroyWindow(auxiliary), WindowOperationStatus::Completed);
            auto hiddenDesc = desc;
            hiddenDesc.StartVisible = false;
            hiddenDesc.Width = 360;
            hiddenDesc.Height = 260;
            created = co_await windows->CreateWindow(hiddenDesc);
            EXPECT_EQ(created.Status, WindowOperationStatus::Completed);
            if (created.Status != WindowOperationStatus::Completed) co_return;
            auxiliary = created.Handle;
            _mutableId = auxiliary.Id;
            EXPECT_EQ(co_await windows->AttachSwapChain(auxiliary, swapchain), WindowOperationStatus::Deferred);
            co_await ObservePhase(3);
            EXPECT_EQ(co_await windows->Show(auxiliary, NativeWindowShowMode::NoActivate), WindowOperationStatus::Completed);
            co_await ObservePhase(4);
            EXPECT_EQ(co_await windows->DetachSwapChain(auxiliary), WindowOperationStatus::Completed);
            co_await ObservePhase(5);
            EXPECT_EQ(co_await windows->AttachSwapChain(auxiliary, swapchain), WindowOperationStatus::Completed);
            co_await ObservePhase(6);
            auto released = co_await windows->ReleaseSwapChain(auxiliary);
            EXPECT_EQ(released.Status, WindowOperationStatus::Completed);
            EXPECT_NE(released.SwapChain, nullptr);
            released.SwapChain.reset();
            co_await ObservePhase(7);
            EXPECT_EQ(co_await windows->AttachSwapChain(auxiliary, swapchain), WindowOperationStatus::Completed);
            co_await ObservePhase(8);
            EXPECT_EQ(co_await windows->DestroyWindow(auxiliary), WindowOperationStatus::Completed);
            EXPECT_FALSE(windows->ResolveWindow(auxiliary));
            EXPECT_FALSE(windows->ShouldExit());
            co_await ObservePhase(9);
            created = co_await windows->CreateWindow(desc);
            EXPECT_EQ(created.Status, WindowOperationStatus::Completed);
            if (created.Status != WindowOperationStatus::Completed) co_return;
            EXPECT_NE(created.Handle.Id, auxiliary.Id);
            EXPECT_FALSE(windows->ResolveWindow(auxiliary));
            auxiliary = created.Handle;
            _mutableId = auxiliary.Id;
            EXPECT_EQ(co_await windows->AttachSwapChain(auxiliary, swapchain), WindowOperationStatus::Completed);
            co_await ObservePhase(10);
        }
        EXPECT_EQ(co_await windows->DestroyWindow(windows->GetMainWindow()->GetHandle()), WindowOperationStatus::Completed);
        EXPECT_TRUE(windows->ShouldExit());
        EXPECT_EQ(windows->GetWindowCount(), 2u);
        Completed = true;
    }

    const bool _lifecycle;
    bool _vulkan{false};
    bool _traceWsi{false};
    const std::thread::id _gameThread{std::this_thread::get_id()};
    uint32_t _updates{0}, _verified{0};
    uint32_t _completedPhases[11]{};
    unordered_set<uint64_t> _completedSerials;
    unordered_set<uint32_t> _flights;
    std::atomic<uint32_t> _phase{0}, _recorded{0}, _explicitSubmissions{0}, _automaticSubmissions{0};
    std::atomic<uint64_t> _mutableId{0};
    std::mutex _probeMutex;
    unordered_map<uint64_t, shared_ptr<FrameProbe>> _pending;
    TaskScope _tasks;
};

void RunMultiWindow(render::RenderBackend backend, bool threaded, bool lifecycle) {
#if !defined(RADRAY_PLATFORM_WINDOWS)
    GTEST_SKIP() << "Application window initialization currently requires Windows";
#endif
    test::RuntimeLogCapture logs;
    {
        MultiWindowApp app{lifecycle};
        auto run = test::RunApplication(app, {
            .FlightDataCount = lifecycle ? 3u : 2u,
            .Window = WindowOptions{.Title = "Multi-window main", .Width = 280, .Height = 200},
            .Gpu = GpuOptions{
                .Backend = backend,
                .EnableValidation = true,
                .EnableSynchronizationValidation = true,
                .Multithreaded = threaded,
                .EnableFrameProfiler = false,
                .BackBufferCount = 3,
                .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
                .PresentMode = render::PresentMode::FIFO,
            },
            .World = std::nullopt,
            .Asset = std::nullopt,
        });
        if (test::CanSkipRuntimeStartup(backend, run.Startup)) GTEST_SKIP() << run.Startup.Reason;
        ASSERT_EQ(run.Startup.Status, RuntimeStartupStatus::Started) << run.Startup.Reason;
        ASSERT_EQ(run.ExitCode, 0);
        EXPECT_TRUE(app.Completed);
    }
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(RuntimeMultiWindow, D3D12SingleThreadOrderedSubmissions) { RunMultiWindow(render::RenderBackend::D3D12, false, false); }
TEST(RuntimeMultiWindow, D3D12ThreadedOrderedSubmissions) { RunMultiWindow(render::RenderBackend::D3D12, true, false); }
// TODO(VVL #13117): restore these multi-swapchain cases after upstream resolution.
// TEST(RuntimeMultiWindow, VulkanSingleThreadOrderedSubmissions) { RunMultiWindow(render::RenderBackend::Vulkan, false, false); }
// TEST(RuntimeMultiWindow, VulkanThreadedOrderedSubmissions) { RunMultiWindow(render::RenderBackend::Vulkan, true, false); }
TEST(RuntimeMultiWindow, D3D12SingleThreadSwapChainLifecycle) { RunMultiWindow(render::RenderBackend::D3D12, false, true); }
TEST(RuntimeMultiWindow, D3D12ThreadedSwapChainLifecycle) { RunMultiWindow(render::RenderBackend::D3D12, true, true); }
// TEST(RuntimeMultiWindow, VulkanSingleThreadSwapChainLifecycle) { RunMultiWindow(render::RenderBackend::Vulkan, false, true); }
// TEST(RuntimeMultiWindow, VulkanThreadedSwapChainLifecycle) { RunMultiWindow(render::RenderBackend::Vulkan, true, true); }

#if defined(RADRAY_ENABLE_VULKAN) && defined(RADRAY_PLATFORM_WINDOWS)

TEST(RuntimeVulkanSwapChainLimits, UnlimitedMaximumPreservesRequestedCount) {
    VkSurfaceCapabilitiesKHR caps{};
    caps.minImageCount = 2;
    EXPECT_EQ(render::vulkan::ResolveSwapChainImageCount(4, caps), 4u);
    EXPECT_EQ(render::vulkan::ResolveSwapChainImageCount(1, caps), 2u);
    caps.maxImageCount = 3;
    EXPECT_EQ(render::vulkan::ResolveSwapChainImageCount(4, caps), 3u);
    EXPECT_EQ(render::vulkan::ResolveSwapChainImageCount(2, caps), 2u);
}

class RuntimeVulkanSwapChain : public testing::Test {
protected:
    void SetUp() override {
        if (!render::test::TryCreateDevice(render::RenderBackend::Vulkan, _context, true)) GTEST_SKIP() << _context.Reason;
        NativeWindow::GlobalInit();
        _windowInitialized = true;
        _window = NativeWindow::Create(Win32WindowCreateDescriptor{
                                           .Title = "Vulkan swapchain regression", .Width = 240, .Height = 180, .ActivateOnShow = false})
                      .Unwrap();
        _device = static_cast<render::vulkan::DeviceVulkan*>(_context.Device.get());
        _chain = _device->CreateSwapChain({.PresentQueue = _context.Queue,
                                           .NativeHandler = _window->GetNativeHandler(),
                                           .Width = 240,
                                           .Height = 180,
                                           .BackBufferCount = 3,
                                           .Format = render::TextureFormat::BGRA8_UNORM,
                                           .PresentMode = render::PresentMode::FIFO})
                     .Release();
        ASSERT_NE(_chain, nullptr);
        _fence = _device->CreateFence().Unwrap();
    }

    void TearDown() override {
        if (_context.Queue) _context.Queue->Wait();
        _chain.reset();
        _fence.reset();
        _window.reset();
        _context.Reset();
        if (_windowInitialized) NativeWindow::GlobalShutdown();
        auto errors = _logs.Errors();
        for (const auto& expected : _expectedErrors) {
            const auto at = errors.find(expected);
            EXPECT_NE(at, string::npos) << expected;
            if (at != string::npos) errors.erase(at, expected.size());
        }
        EXPECT_TRUE(errors.empty()) << errors;
    }

    void PresentOne(bool waitPreviousValue = false) {
        auto acquired = _chain->AcquireNext(2000);
        ASSERT_EQ(acquired.Status, render::SwapChainStatus::Success);
        ASSERT_TRUE(acquired.Frame);
        auto& frame = *acquired.Frame;
        auto commands = _device->CreateCommandBuffer(_context.Queue).Unwrap();
        commands->Begin();
        const render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
            .Target = frame.GetBackBuffer(), .Before = render::TextureState::Undefined, .After = render::TextureState::Present};
        commands->ResourceBarrier(std::span{&barrier, 1});
        commands->End();
        render::CommandBuffer* cmd[]{commands.get()};
        render::Fence* fences[]{_fence.get()};
        uint64_t values[]{++_signal};
        render::SwapChainSyncObject* wait[]{frame.GetWaitToDraw()};
        render::SwapChainSyncObject* ready[]{frame.GetReadyToPresent()};
        _context.Queue->Submit({.CmdBuffers = cmd, .SignalFences = fences, .SignalValues = values, .WaitToExecute = wait, .ReadyToPresent = ready});
        if (waitPreviousValue) {
            const auto previous = _signal - 1;
            const auto semaphore = render::vulkan::CastVkObject(_fence.get())->_fence->_semaphore;
            const VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &semaphore, &previous};
            EXPECT_EQ(_device->_ftb.vkWaitSemaphores(_device->_device, &waitInfo, UINT64_MAX), VK_SUCCESS);
        }
        // Force rendering to finish before Present, exercising host-wait/WSI tracking.
        _fence->Wait(_signal);
        EXPECT_EQ(_chain->Present(std::move(frame)).Status, render::SwapChainStatus::Success);
    }

    bool Recreate() { return _chain->Recreate(240, 180, render::TextureFormat::BGRA8_UNORM, render::PresentMode::FIFO); }

    test::RuntimeLogCapture _logs;
    render::test::DeviceContext _context;
    Nullable<render::vulkan::DeviceVulkan*> _device;
    unique_ptr<NativeWindow> _window;
    unique_ptr<render::SwapChain> _chain;
    unique_ptr<render::Fence> _fence;
    vector<string> _expectedErrors;
    uint64_t _signal{0};
    bool _windowInitialized{false};
};

TEST_F(RuntimeVulkanSwapChain, RetryAfterImageEnumerationFailure) {
    PresentOne();
    _context.Queue->Wait();
    const auto original = _device->_ftb.vkGetSwapchainImagesKHR;
    _device->_ftb.vkGetSwapchainImagesKHR = [](VkDevice, VkSwapchainKHR, uint32_t*, VkImage*) -> VkResult {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    };
    const bool recreated = Recreate();
    _device->_ftb.vkGetSwapchainImagesKHR = original;
    _expectedErrors.emplace_back(fmt::format("vkGetSwapchainImagesKHR failed: {}\n", VK_ERROR_OUT_OF_DEVICE_MEMORY));
    EXPECT_FALSE(recreated);
    ASSERT_FALSE(_chain->IsValid());
    EXPECT_EQ(_chain->GetBackBufferCount(), 0u);
    EXPECT_EQ(_chain->AcquireNext(0).Status, render::SwapChainStatus::RequireRecreate);
    ASSERT_TRUE(Recreate());
    EXPECT_GE(_chain->GetBackBufferCount(), 3u);
    PresentOne();
}

TEST_F(RuntimeVulkanSwapChain, RetryAfterNativeCreationRetiresOldChain) {
    PresentOne();
    _context.Queue->Wait();
    const auto original = _device->_ftb.vkCreateSwapchainKHR;
    _device->_ftb.vkCreateSwapchainKHR = [](VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSwapchainKHR* result) -> VkResult {
        auto create = reinterpret_cast<PFN_vkCreateSwapchainKHR>(vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR"));
        auto destroy = reinterpret_cast<PFN_vkDestroySwapchainKHR>(vkGetDeviceProcAddr(device, "vkDestroySwapchainKHR"));
        VkSwapchainKHR temporary{};
        const auto status = create(device, info, allocator, &temporary);
        EXPECT_EQ(status, VK_SUCCESS);
        if (status != VK_SUCCESS) return status;
        destroy(device, temporary, allocator);
        *result = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    };
    const bool recreated = Recreate();
    _device->_ftb.vkCreateSwapchainKHR = original;
    _expectedErrors.emplace_back(fmt::format("vkCreateSwapchainKHR failed: {}\n", VK_ERROR_OUT_OF_DEVICE_MEMORY));
    _expectedErrors.emplace_back("vkCreateSwapchainKHR failed during swapchain recreate\n");
    EXPECT_FALSE(recreated);
    ASSERT_FALSE(_chain->IsValid());
    EXPECT_EQ(_chain->GetBackBufferCount(), 0u);
    EXPECT_EQ(_chain->AcquireNext(0).Status, render::SwapChainStatus::RequireRecreate);
    ASSERT_TRUE(Recreate());
    EXPECT_GE(_chain->GetBackBufferCount(), 3u);
    PresentOne();
}

TEST_F(RuntimeVulkanSwapChain, RepeatedPresentAndImmediateRecreation) {
    for (uint32_t cycle = 0; cycle < 12; ++cycle) {
        SCOPED_TRACE(cycle);
        for (uint32_t frame = 0; frame < 8; ++frame) PresentOne();
        _context.Queue->Wait();
        ASSERT_TRUE(Recreate());
    }
    PresentOne();
    // TearDown destroys the chain immediately after its last present.
}

TEST_F(RuntimeVulkanSwapChain, WaitPreviousTimelineBeforePresent) {
    for (uint32_t frame = 0; frame < 12; ++frame) {
        SCOPED_TRACE(frame);
        ASSERT_NO_FATAL_FAILURE(PresentOne(true));
    }
}

// TODO(VVL #13117): restore this multi-swapchain case after upstream resolution.
/*
TEST_F(RuntimeVulkanSwapChain, MultiSwapChainHostWaitBeforePresent) {
    // Acquire all images, chain submissions through one timeline, wait on the host,
    // then present all windows. Syncval must preserve prior-present dependencies.
    vector<unique_ptr<NativeWindow>> windows;
    vector<unique_ptr<render::SwapChain>> chains;
    for (int i = 0; i < 3; ++i) {
        windows.push_back(NativeWindow::Create(Win32WindowCreateDescriptor{
                                                   .Title = "Host wait reproduction", .Width = 240, .Height = 180, .X = 260 * i, .ActivateOnShow = false})
                              .Unwrap());
        auto desc = _chain->GetDesc();
        desc.NativeHandler = windows.back()->GetNativeHandler();
        chains.push_back(_device->CreateSwapChain(desc).Unwrap());
    }
    unordered_set<render::Texture*> initialized;
    const render::RenderPassColorAttachmentDescriptor attachment{render::TextureFormat::BGRA8_UNORM, 1, render::LoadAction::Clear, render::StoreAction::Store};
    auto pass = _device->CreateRenderPass({std::span{&attachment, 1}, {}}).Unwrap();
    for (uint32_t round = 0; round < 12; ++round) {
        SCOPED_TRACE(round);
        vector<render::SwapChainFrame> frames;
        vector<unique_ptr<render::CommandBuffer>> commands;
        vector<unique_ptr<render::TextureView>> views;
        vector<unique_ptr<render::Framebuffer>> framebuffers;
        for (auto& chain : chains) {
            auto acquired = chain->AcquireNext(2000);
            ASSERT_EQ(acquired.Status, render::SwapChainStatus::Success);
            frames.push_back(std::move(*acquired.Frame));
        }
        for (size_t step = 0; step < frames.size(); ++step) {
            const size_t index = round % 2 == 0 ? step : frames.size() - 1 - step;
            auto& frame = frames[index];
            commands.push_back(_device->CreateCommandBuffer(_context.Queue).Unwrap());
            auto* command = commands.back().get();
            command->Begin();
            const render::ResourceBarrierDescriptor before = render::BarrierTextureDescriptor{
                .Target = frame.GetBackBuffer(), .Before = initialized.insert(frame.GetBackBuffer()).second ? render::TextureState::Undefined : render::TextureState::Present, .After = render::TextureState::RenderTarget};
            command->ResourceBarrier(std::span{&before, 1});
            views.push_back(_device->CreateTextureView({.Target = frame.GetBackBuffer(), .Dim = render::TextureDimension::Dim2D, .Format = render::TextureFormat::BGRA8_UNORM, .Range = {.BaseArrayLayer = 0, .ArrayLayerCount = 1, .BaseMipLevel = 0, .MipLevelCount = 1}, .Usage = render::TextureViewUsage::RenderTarget}).Unwrap());
            auto* view = views.back().get();
            framebuffers.push_back(_device->CreateFramebuffer({pass.get(), std::span{&view, 1}, nullptr, 240, 180, 1}).Unwrap());
            const render::ColorClearValue clear{{0, 0, 0, 1}};
            auto encoder = command->BeginRenderPass({pass.get(), framebuffers.back().get(), std::span{&clear, 1}, {}, "Reproduce present synchronization"}).Unwrap();
            command->EndRenderPass(std::move(encoder));
            const render::ResourceBarrierDescriptor after = render::BarrierTextureDescriptor{
                .Target = frame.GetBackBuffer(), .Before = render::TextureState::RenderTarget, .After = render::TextureState::Present};
            command->ResourceBarrier(std::span{&after, 1});
            command->End();
            render::Fence* fences[]{_fence.get()};
            uint64_t previous[]{_signal};
            uint64_t values[]{++_signal};
            render::SwapChainSyncObject* wait[]{frame.GetWaitToDraw()};
            render::SwapChainSyncObject* ready[]{frame.GetReadyToPresent()};
            _context.Queue->Submit({.CmdBuffers = std::span{&command, 1}, .SignalFences = fences, .SignalValues = values, .WaitFences = fences, .WaitValues = previous, .WaitToExecute = wait, .ReadyToPresent = ready});
        }
        render::Fence* fences[]{_fence.get()};
        uint64_t previous[]{_signal};
        uint64_t values[]{++_signal};
        _context.Queue->Submit({.SignalFences = fences, .SignalValues = values, .WaitFences = fences, .WaitValues = previous});
        _fence->Wait(_signal);
        for (size_t step = 0; step < chains.size(); ++step) {
            const size_t index = round % 2 == 0 ? step : chains.size() - 1 - step;
            EXPECT_EQ(chains[index]->Present(std::move(frames[index])).Status, render::SwapChainStatus::Success);
        }
    }
    _context.Queue->Wait();
}
*/

#endif

}  // namespace
}  // namespace radray
