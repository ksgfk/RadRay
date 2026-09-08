#include "gpu_test_fixture.h"

namespace radray::render::test {

class D3D12DeviceLossDeathTest : public testing::TestWithParam<int> {};

TEST_P(D3D12DeviceLossDeathTest, StopsBeforeReportingCompletion) {
#if defined(RADRAY_ENABLE_D3D12)
    DeviceContext context;
    if (!TryCreateDevice(RenderBackend::D3D12, context)) {
        GTEST_SKIP() << context.Reason;
    }
    auto* device = d3d12::CastD3D12Object(context.Device.get());
    d3d12::ComPtr<ID3D12Device5> native;
    if (FAILED(device->_device.As(&native))) {
        GTEST_SKIP() << "ID3D12Device5 unavailable";
    }
    auto fenceResult = context.Device->CreateFence();
    ASSERT_TRUE(fenceResult);
    auto fence = fenceResult.Release();
    // Exit explicitly if the operation returns: teardown must not mask a missed guard.
    EXPECT_EXIT({
        native->RemoveDevice();
        switch (GetParam()) {
            case 0: (void)fence->GetCompletedValue(); break;
            case 1: fence->Wait(); break;
            case 2: fence->Wait(1); break;
            case 3: context.Queue->Wait(); break;
            case 4: context.Queue->Submit({}); break;
        }
        std::_Exit(0);
    }, testing::ExitedWithCode(3), "");
#else
    GTEST_SKIP() << "D3D12 backend was not built";
#endif
}

INSTANTIATE_TEST_SUITE_P(D3D12, D3D12DeviceLossDeathTest, testing::Range(0, 5));

}  // namespace radray::render::test
