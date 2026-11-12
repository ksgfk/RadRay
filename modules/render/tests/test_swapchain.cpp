#include "gpu_test_fixture.h"

#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>

namespace radray::render {
namespace {

class SwapChainBackBufferTest : public test::BackendTest {
protected:
    void SetUp() override {
        BackendTest::SetUp();
        if (HasFatalFailure() || IsSkipped()) return;
        _window = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"RadRay backbuffer access test",
                                    WS_OVERLAPPEDWINDOW, 0, 0, 160, 120, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(_window, nullptr);
        ::ShowWindow(_window, SW_SHOWNOACTIVATE);
        PumpMessages();
    }

    void TearDown() override {
        _swapchain.reset();
        if (_window) ::DestroyWindow(_window);
        BackendTest::TearDown();
    }

    static void PumpMessages() {
        MSG message;
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    RECT ClientRect() const {
        RECT bounds{};
        ::GetClientRect(_window, &bounds);
        return bounds;
    }

    void PresentWithFrameView() {
        auto& device = GetDevice();
        PumpMessages();
        auto acquired = _swapchain->AcquireNext(1000);
        ASSERT_EQ(acquired.Status, SwapChainStatus::Success);
        ASSERT_TRUE(acquired.Frame);
        auto frame = std::move(*acquired.Frame);
        const auto desc = _swapchain->GetDesc();
        ASSERT_NE(frame.GetBackBuffer(), nullptr);
        EXPECT_EQ(frame.GetBackBuffer()->GetDesc().Width, desc.Width);
        EXPECT_EQ(frame.GetBackBuffer()->GetDesc().Height, desc.Height);
        auto view = device.CreateTextureView({frame.GetBackBuffer(), TextureDimension::Dim2D, desc.Format, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
        auto recording = test::BeginRecording(device);
        test::Barrier(*recording.Commands, {BarrierTextureDescriptor{.Target = frame.GetBackBuffer(), .Before = TextureState::Undefined, .After = TextureState::RenderTarget}});
        const RenderPassColorAttachment color{view.get(), LoadAction::Clear, StoreAction::Store, {{0.25f, 0.5f, 0.75f, 1.0f}}};
        auto encoder = recording.Commands->BeginRenderPass({.Colors = std::span{&color, 1}, .Width = desc.Width, .Height = desc.Height}).Unwrap();
        recording.Commands->EndRenderPass(std::move(encoder));
        test::Barrier(*recording.Commands, {BarrierTextureDescriptor{.Target = frame.GetBackBuffer(), .Before = TextureState::RenderTarget, .After = TextureState::Present}});
        recording.Commands->End();
        CommandBuffer* commands[]{recording.Commands.get()};
        SwapChainSyncObject* waits[]{frame.GetWaitToDraw()};
        SwapChainSyncObject* signals[]{frame.GetReadyToPresent()};
        auto& queue = test::DirectQueue(device);
        queue.Submit({.CmdBuffers = commands,
                      .WaitToExecute = {waits, waits[0] ? 1u : 0u},
                      .ReadyToPresent = {signals, signals[0] ? 1u : 0u}});
        EXPECT_EQ(_swapchain->Present(std::move(frame)).Status, SwapChainStatus::Success);
        queue.Wait();
        recording.Allocator->Reset();
        view.reset();
    }

    HWND _window{nullptr};
    unique_ptr<SwapChain> _swapchain;
};

TEST_P(SwapChainBackBufferTest, FrameViewsSurviveSubmissionAndRecreate) {
    auto& device = GetDevice();
    EXPECT_NE(device.GetDetail().VendorId, 0u);
    EXPECT_FALSE(device.GetDetail().DriverVersion.empty());
    EXPECT_FALSE(device.GetExternalResourceExtension());
    const auto bounds = ClientRect();
    _swapchain = device.CreateSwapChain({.PresentQueue = &test::DirectQueue(device), .NativeHandler = _window,
                                        .Width = uint32_t(bounds.right), .Height = uint32_t(bounds.bottom), .BackBufferCount = 3,
                                        .Format = TextureFormat::BGRA8_UNORM, .PresentMode = PresentMode::FIFO}).Unwrap();
    ASSERT_FALSE(HasFatalFailure());
    for (int frame = 0; frame < 3; ++frame) PresentWithFrameView();

    ASSERT_TRUE(::SetWindowPos(_window, nullptr, 0, 0, 240, 180, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    PumpMessages();
    const auto resized = ClientRect();
    ASSERT_TRUE(_swapchain->Recreate(uint32_t(resized.right), uint32_t(resized.bottom), TextureFormat::BGRA8_UNORM, PresentMode::FIFO));
    ASSERT_FALSE(HasFatalFailure());
    for (int frame = 0; frame < 3; ++frame) PresentWithFrameView();
}

INSTANTIATE_TEST_SUITE_P(Backends, SwapChainBackBufferTest, testing::ValuesIn(test::AllBackends), test::BackendParamName);

}  // namespace
}  // namespace radray::render
#endif
