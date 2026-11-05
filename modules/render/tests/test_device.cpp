#include "gpu_test_fixture.h"

namespace radray::render {
namespace {

class DeviceTest : public test::BackendTest {};

TEST_P(DeviceTest, CreatesDirectQueueAndReportsAdapter) {
    auto& device = GetDevice();
    const auto& capabilities = device.GetCapabilities();

    EXPECT_EQ(device.GetBackend(), GetParam());
    EXPECT_EQ(capabilities.Queues[static_cast<size_t>(QueueType::Direct)].CreatedCount, 1u);
    EXPECT_EQ(test::DirectQueue(device).GetQueueType(), QueueType::Direct);
    EXPECT_FALSE(capabilities.Detail.GpuName.empty());
    RecordProperty("adapter", capabilities.Detail.GpuName);
}

INSTANTIATE_TEST_SUITE_P(Backends, DeviceTest, testing::ValuesIn(test::AllBackends), test::BackendParamName);

}  // namespace
}  // namespace radray::render
