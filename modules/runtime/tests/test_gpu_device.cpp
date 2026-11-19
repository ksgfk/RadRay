#include <gtest/gtest.h>

#include "../../gpu_impl/tests/gpu_test_fixture.h"

#include <radray/runtime/gpu_device.h>

#include "test_fakes.h"

namespace radray {
namespace {

class RecordingPipelineCache final : public gpu::PipelineCache {
public:
    RecordingPipelineCache(vector<byte> data, vector<string>& events) noexcept
        : _data(std::move(data)), _events(events) {}
    ~RecordingPipelineCache() noexcept override { _events.emplace_back("cache_destroy"); }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    vector<byte> GetData() const noexcept override { return _data; }

private:
    vector<byte> _data;
    vector<string>& _events;
};

class CacheOrderFakeQueue final : public gpu::CommandQueue {
public:
    explicit CacheOrderFakeQueue(vector<string>& events) noexcept : _events(events) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    bool Submit(const gpu::CommandQueueSubmitDescriptor&) noexcept override { return true; }
    uint32_t GetTimestampValidBits() const noexcept override { return 0; }
    void Wait() noexcept override { _events.emplace_back("queue_wait"); }
    gpu::QueueType GetQueueType() const noexcept override { return gpu::QueueType::Direct; }
    gpu::TimestampClockCalibrationResult GetTimestampClockCalibration() const noexcept override { return {}; }

private:
    vector<string>& _events;
};

class CacheOrderFakeDevice final : public test::FakeDevice {
public:
    CacheOrderFakeDevice(vector<byte> cacheData, vector<string>& events)
        : OrderQueue(events), _cacheData(std::move(cacheData)), _events(events) {}
    ~CacheOrderFakeDevice() noexcept override { _events.emplace_back("device_destroy"); }
    Nullable<unique_ptr<gpu::PipelineCache>> CreatePipelineCache(std::span<const byte>) noexcept override {
        return make_unique<RecordingPipelineCache>(_cacheData, _events);
    }

    CacheOrderFakeQueue OrderQueue;

private:
    vector<byte> _cacheData;
    vector<string>& _events;
};

TEST(DeviceContextTest, SupportsIndirectRequiresBothIndirectFeatures) {
    test::FakeGpu gpu;
    auto& features = gpu.Device->Capabilities.Features;
    features.IndirectDraw = true;
    EXPECT_FALSE(gpu.Context.SupportsIndirect());
    features.IndirectFirstInstance = true;
    EXPECT_TRUE(gpu.Context.SupportsIndirect());
}

TEST(DeviceContextTest, NullPipelineCacheReturnsEmptyData) {
    test::FakeGpu gpu;
    EXPECT_FALSE(gpu.Context.GetPipelineCache());
    EXPECT_TRUE(gpu.Context.GetPipelineCacheData().empty());
}

TEST(DeviceContextTest, DestroysPipelineCacheBeforeExclusiveDeviceReference) {
    vector<string> events;
    const vector<byte> payload{byte{0x12}, byte{0x34}, byte{0x56}};
    {
        auto device = make_shared<CacheOrderFakeDevice>(payload, events);
        auto* queue = &device->OrderQueue;
        DeviceContext context{std::move(device), queue};
        ASSERT_TRUE(context.GetPipelineCache());
        EXPECT_EQ(context.GetPipelineCacheData(), payload);
    }
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0], "queue_wait");
    EXPECT_EQ(events[1], "cache_destroy");
    EXPECT_EQ(events[2], "device_destroy");
}

class DeviceContextGpuTest : public gpu::test::BackendTest {};

TEST_P(DeviceContextGpuTest, PublicConstructorCreatesExportablePipelineCache) {
    auto& device = GetDevice();
    DeviceContext context{shared_ptr<gpu::Device>{&device, [](gpu::Device*) {}}, &gpu::test::DirectQueue(device)};
    ASSERT_TRUE(context.GetPipelineCache());
    EXPECT_TRUE(context.GetPipelineCache()->IsValid());
    const auto data = context.GetPipelineCacheData();
    EXPECT_EQ(data, context.GetPipelineCache()->GetData());
}

INSTANTIATE_TEST_SUITE_P(Backends, DeviceContextGpuTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);

class DeviceContextFactoryGpuTest : public testing::TestWithParam<gpu::GpuBackend> {};

TEST_P(DeviceContextFactoryGpuTest, FactoryCreatesExportablePipelineCacheAndDestroysCleanly) {
    auto env = Environment::Create(GetParam(), true, "DeviceContextFactoryGpuTest");
    if (!env) GTEST_SKIP() << "backend environment unavailable";
    auto context = DeviceContext::Create(*env);
    ASSERT_TRUE(context);
    auto owner = context.Release();
    ASSERT_TRUE(owner->GetPipelineCache());
    EXPECT_TRUE(owner->GetPipelineCache()->IsValid());
    const auto data = owner->GetPipelineCacheData();
    EXPECT_EQ(data, owner->GetPipelineCache()->GetData());
    // Destroying owner here releases the last shared_ptr<gpu::Device>; must destroy cache before device.
    owner.reset();
}

INSTANTIATE_TEST_SUITE_P(Backends, DeviceContextFactoryGpuTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);

}  // 匿名命名空间
}  // namespace radray
