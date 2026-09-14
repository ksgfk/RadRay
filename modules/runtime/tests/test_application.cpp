#include "runtime_test_support.h"
#include "gpu_test_fixture.h"

#include <gtest/gtest.h>

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
    std::atomic<uint32_t> Recorded{0}, Completed{0}, AuxiliaryRecorded{0};
    bool Resized{false}, AuxiliaryDestroyed{false}, WorldAvailable{false};

    void Render(AppFrameContext& ctx) override {
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
            auto submission = make_shared<FrameSubmission>(ctx.FrameSerial());
            submission->OnSubmitted = [window, bufferIndex = target->BackBufferIndex] {
                window->SetBackBufferState(bufferIndex, render::TextureState::Present);
            };
            submission->OnCompleted = [this](bool success) {
                if (success) ++Completed;
            };
            ASSERT_TRUE(submission->Record());
            ctx.TrackSubmission(std::move(submission));
            ++Recorded;
            if (!window->IsMainWindow()) ++AuxiliaryRecorded;
        }
    }

protected:
    void OnInit() override {
        ASSERT_NE(GetWorld(), nullptr);
        ASSERT_NE(GetWorld()->SpawnActor(), nullptr);
        WorldAvailable = GetWorld()->GetActors().size() == 1;
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

private:
    uint32_t _updates{0};
    Nullable<AppWindow*> _auxiliary{nullptr};
};

void RunFoundation(render::RenderBackend backend, bool threaded) {
    {
        render::test::DeviceContext device;
        if (!render::test::TryCreateDevice(backend, device)) GTEST_SKIP() << device.Reason;
    }
    test::RuntimeLogCapture logs;
    FoundationApp app;
    ASSERT_EQ(app.Run({.Backend = backend, .EnableValidation = true, .Multithreaded = threaded,
                      .EnableSynchronizationValidation = true, .WindowTitle = "Runtime foundation",
                      .WindowWidth = 160, .WindowHeight = 120, .FlightDataCount = 2,
                      .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
    EXPECT_TRUE(app.WorldAvailable);
    EXPECT_TRUE(app.Resized);
    EXPECT_TRUE(app.AuxiliaryDestroyed);
    EXPECT_GT(app.AuxiliaryRecorded.load(), 0u);
    EXPECT_GT(app.Recorded.load(), 0u);
    EXPECT_EQ(app.Completed.load(), app.Recorded.load());
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(RuntimeFoundation, D3D12SingleThreadWindowLifecycle) { RunFoundation(render::RenderBackend::D3D12, false); }
TEST(RuntimeFoundation, D3D12ThreadedWindowLifecycle) { RunFoundation(render::RenderBackend::D3D12, true); }
TEST(RuntimeFoundation, VulkanSingleThreadWindowLifecycle) { RunFoundation(render::RenderBackend::Vulkan, false); }
TEST(RuntimeFoundation, VulkanThreadedWindowLifecycle) { RunFoundation(render::RenderBackend::Vulkan, true); }

}  // namespace
}  // namespace radray
