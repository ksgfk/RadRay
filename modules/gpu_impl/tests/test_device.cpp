#include "gpu_test_fixture.h"

#include <radray/gpu/external_resource.h>

#if defined(RADRAY_ENABLE_D3D12)
#include <algorithm>
#include <radray/gpu_impl/backend/d3d12_impl.h>
#endif

namespace radray::gpu {
namespace {

#if defined(RADRAY_ENABLE_D3D12)
TEST(D3D12DeviceLoss, WaitForRejectsCachedValueOnFirstAndSubsequentLoss) {
    auto factory = d3d12::CreateDXGIFactory({});
    ASSERT_TRUE(factory);
    const auto adapters = factory->GetAdapters();
    const auto software = std::ranges::find_if(adapters, [](const DXGIAdapterInfo& info) {
        return info.IsSoftware && info.IsD3D12Supported;
    });
    if (software == adapters.end()) GTEST_SKIP() << "D3D12 software adapter unavailable";
    auto device = d3d12::CreateDevice({.Factory = factory.Get(), .AdapterIndex = software->Index, .QueueCounts = {}});
    ASSERT_TRUE(device);
    auto fence = device->CreateFenceD3D12(7);
    ASSERT_TRUE(fence);
    ASSERT_TRUE(fence->WaitFor(7, 0));
    EXPECT_FALSE(fence->WaitFor(8, 0));

    d3d12::ComPtr<ID3D12Device5> native;
    if (FAILED(device->_device.As(&native))) GTEST_SKIP() << "ID3D12Device5 unavailable";
    native->RemoveDevice();
    ASSERT_FALSE(device->IsDeviceLost());
    EXPECT_FALSE(fence->WaitFor(7, 0));
    ASSERT_TRUE(device->IsDeviceLost());
    EXPECT_EQ(fence->GetCompletedValue(), 7u);
    EXPECT_FALSE(fence->WaitFor(7, 0));
}
#endif

class DeviceTest : public test::BackendTest {};

TEST_P(DeviceTest, CreatesDirectQueueAndReportsAdapter) {
    auto& device = GetDevice();
    const auto& capabilities = device.GetCapabilities();

    EXPECT_EQ(device.GetBackend(), GetParam());
    EXPECT_EQ(capabilities.Queues[static_cast<size_t>(QueueType::Direct)].CreatedCount, 1u);
    EXPECT_EQ(test::DirectQueue(device).GetQueueType(), QueueType::Direct);
    EXPECT_FALSE(capabilities.Detail.GpuName.empty());
    RecordProperty("adapter", capabilities.Detail.GpuName.c_str());
}

TEST_P(DeviceTest, ExternalExtensionsHaveIndependentLifetimes) {
    auto& device = GetDevice();
    auto first = device.CreateExternalResourceExtension().Release();
#if defined(RADRAY_PLATFORM_WINDOWS)
    ASSERT_TRUE(first);
    auto second = device.CreateExternalResourceExtension().Release();
    ASSERT_TRUE(second);
    ASSERT_NE(first.get(), second.get());
    first.reset();

    device.Destroy();
    EXPECT_TRUE(device.IsValid());
    EXPECT_GT(TakeRhiValidationErrors(), 0u);
    const auto identity = second->GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) {
        EXPECT_FALSE(error->Message.empty());
    } else {
        EXPECT_NE(std::get<ExternalDeviceIdentity>(identity).NodeMask, 0u);
    }
#else
    EXPECT_FALSE(first);
#endif
}

INSTANTIATE_TEST_SUITE_P(Backends, DeviceTest, testing::ValuesIn(test::AllBackends), test::BackendParamName);

}  // namespace
}  // namespace radray::gpu
