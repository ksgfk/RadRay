#include "gpu_test_fixture.h"

#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>

#if defined(RADRAY_ENABLE_D3D12)
#include <d3d11.h>
#include <radray/gpu_impl/backend/d3d12_impl.h>
#include <radray/scope_guard.h>
#endif

namespace radray::gpu {
namespace {

#if defined(RADRAY_ENABLE_D3D12)
TEST(D3D12SwapChainRecovery, ResizeAndBackBufferRefreshFailuresLeaveARetriableChain) {
    auto factory = d3d12::CreateDXGIFactory({}).Unwrap();
    auto device = d3d12::CreateDevice({.Factory = factory.get()}).Unwrap();
    const auto window = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"RadRay DXGI recovery test",
                                          WS_OVERLAPPEDWINDOW, 0, 0, 160, 120, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(window, nullptr);
    auto closeWindow = MakeScopeGuard([&] { ::DestroyWindow(window); });
    ::ShowWindow(window, SW_SHOWNOACTIVATE);
    auto chain = device->CreateSwapChain({.PresentQueue = &test::DirectQueue(*device), .NativeHandler = window,
        .Width = 64, .Height = 64, .BackBufferCount = 3, .Format = TextureFormat::BGRA8_UNORM, .PresentMode = PresentMode::FIFO}).Unwrap();
    auto& native = *static_cast<d3d12::SwapChainD3D12*>(chain.get());
    auto resize = [&] { return chain->Recreate(64, 64, TextureFormat::BGRA8_UNORM, PresentMode::FIFO); };
    d3d12::ComPtr<ID3D12Resource> held;
    ASSERT_EQ(native._swapchain->GetBuffer(0, IID_PPV_ARGS(&held)), S_OK);
    EXPECT_FALSE(resize());
    EXPECT_TRUE(native._frames.empty());
    EXPECT_EQ(chain->AcquireNext(0).Status, SwapChainStatus::RequireRecreate);
    held.Reset();
    ASSERT_TRUE(resize());

    // A DXGI flip chain from D3D11 can resize, but GetBuffer cannot return ID3D12Resource.
    // Exercise the native refresh failure without a hook in the production backend.
    d3d12::ComPtr<ID3D11Device> otherDevice;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &otherDevice, nullptr, nullptr)));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = 64;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d3d12::ComPtr<IDXGISwapChain1> otherChain;
    auto* dxgi = static_cast<d3d12::DXGIFactoryImpl*>(factory.get())->_factory.Get();
    ASSERT_TRUE(SUCCEEDED(dxgi->CreateSwapChainForComposition(otherDevice.Get(), &desc, nullptr, &otherChain)));
    auto original = native._swapchain;
    ASSERT_EQ(otherChain.As(&native._swapchain), S_OK);
    EXPECT_FALSE(resize());
    EXPECT_EQ(chain->GetBackBufferCount(), 0u);
    EXPECT_EQ(chain->AcquireNext(0).Status, SwapChainStatus::RequireRecreate);
    native._swapchain = original;
    ASSERT_TRUE(resize());
    auto acquired = chain->AcquireNext(1000);
    ASSERT_TRUE(acquired.Frame);
    EXPECT_EQ(chain->Present(std::move(*acquired.Frame)).Status, SwapChainStatus::Success);
    test::DirectQueue(*device).Wait();
}
#endif

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
    EXPECT_NE(device.GetCapabilities().Detail.VendorId, 0u);
    EXPECT_FALSE(device.GetCapabilities().Detail.DriverVersion.empty());
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
}  // namespace radray::gpu
#endif
