#include "gpu_test_fixture.h"

#include <chrono>
#include <semaphore>
#include <thread>

#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/gpu_impl/backend/vulkan_impl.h>
#endif

namespace radray::gpu {
namespace {

constexpr uint64_t BufferSize = 256;

vector<byte> Pattern() {
    vector<byte> bytes(BufferSize);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<byte>(i * 7 + 3);
    return bytes;
}

class BufferTest : public test::BackendTest {
protected:
    unique_ptr<Buffer> CreateMappable(MemoryType memory, ResourceHints hints = ResourceHint::None) {
        const BufferUses usage = memory == MemoryType::Upload ? BufferUse::MapWrite | BufferUse::CopySource
                                                              : BufferUse::MapRead | BufferUse::CopyDestination;
        return GetDevice().CreateBuffer({.Size = BufferSize, .Memory = memory, .Usage = usage, .Hints = hints}).Unwrap();
    }

    void CopyToHost(Buffer& upload, Buffer& readback) {
        auto recording = test::BeginRecording(GetDevice());
        auto& commands = *recording.Commands;
        test::Barrier(commands, {BarrierBufferDescriptor{.Target = &upload, .Before = BufferState::HostWrite, .After = BufferState::CopySource}});
        commands.CopyBufferToBuffer(&readback, 0, &upload, 0, BufferSize);
        test::Barrier(commands, {BarrierBufferDescriptor{.Target = &readback, .Before = BufferState::CopyDestination, .After = BufferState::HostRead}});
        test::SubmitAndWait(GetDevice(), recording);
    }
};

#if defined(RADRAY_ENABLE_VULKAN)
class VulkanBufferMappingTest : public test::GpuTest {};

TEST_F(VulkanBufferMappingTest, NativeMapFailureRejectsBothHostMemoryTypesAtCreation) {
    if (!OpenDevice(GpuBackend::Vulkan)) return;
    const VulkanCommandQueueDescriptor queues[]{{QueueType::Direct, 1}};
    auto device = vulkan::CreateDeviceVulkan({.Queues = queues});
    ASSERT_TRUE(device);
    VmaVulkanFunctions functions{};
    VmaAllocatorCreateInfo create{};
    create.physicalDevice = device->_physicalDevice;
    create.device = device->_device;
    create.instance = device->_instance->_instance;
    create.vulkanApiVersion = VK_API_VERSION_1_2;
    create.pVulkanFunctions = &functions;
    create.pAllocationCallbacks = device->GetAllocationCallbacks();
    ASSERT_EQ(vmaImportVulkanFunctionsFromVolk(&create, &functions), VK_SUCCESS);
    functions.vkMapMemory = +[](VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void**) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    };
    VmaAllocator allocator{};
    ASSERT_EQ(vmaCreateAllocator(&create, &allocator), VK_SUCCESS);
    device->_vma = make_unique<vulkan::VMA>(allocator);
    EXPECT_FALSE(device->CreateBuffer({.Size = BufferSize, .Memory = MemoryType::Upload, .Usage = BufferUse::MapWrite | BufferUse::CopySource,
                                       .Hints = ResourceHint::Dedicated}));
    EXPECT_FALSE(device->CreateBuffer({.Size = BufferSize, .Memory = MemoryType::ReadBack, .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
                                       .Hints = ResourceHint::Dedicated}));
    EXPECT_FALSE(device->IsDeviceLost());
    EXPECT_TRUE(device->CreateBuffer({.Size = BufferSize, .Usage = BufferUse::CopySource}));
}
#endif

TEST_P(BufferTest, UploadContentCopiesToReadback) {
    const auto expected = Pattern();
    auto upload = CreateMappable(MemoryType::Upload);
    auto readback = CreateMappable(MemoryType::ReadBack);
    void* written = upload->GetMappedData().Get();
    ASSERT_NE(written, nullptr);
    std::memcpy(written, expected.data(), expected.size());
    ASSERT_TRUE(upload->FlushMappedRange({0, BufferSize}));

    CopyToHost(*upload, *readback);

    EXPECT_EQ(test::MapAndRead(*readback), expected);
}

TEST_P(BufferTest, RejectsIncompatibleMappingDescriptors) {
    auto& device = GetDevice();
    const auto rejected = [&](MemoryType memory, BufferUses usage, ResourceHints hints = {}) {
        EXPECT_FALSE(device.CreateBuffer({.Size = BufferSize, .Memory = memory, .Usage = usage, .Hints = hints}));
        EXPECT_EQ(TakeRhiValidationErrors(), 1u);
    };
    rejected(MemoryType::Upload, BufferUse::CopySource);
    rejected(MemoryType::ReadBack, BufferUse::CopyDestination);
    rejected(MemoryType::Device, BufferUse::MapRead);
    rejected(MemoryType::Upload, BufferUse::MapRead | BufferUse::MapWrite);
    rejected(MemoryType::Upload, BufferUse::MapWrite, ResourceHint::External);
    rejected(MemoryType::ReadBack, BufferUse::MapRead, ResourceHints{1u << 2});
    auto local = device.CreateBuffer({.Size = BufferSize, .Usage = BufferUse::CopySource}).Unwrap();
    EXPECT_FALSE(local->GetMappedData());
}

TEST_P(BufferTest, RejectsSubmissionAfterEitherCopyResourceWasDestroyed) {
    for (bool destroySource : {false, true}) {
        auto src = GetDevice().CreateBuffer({.Size = BufferSize, .Usage = BufferUse::CopySource}).Unwrap();
        auto dst = CreateMappable(MemoryType::ReadBack);
        auto recording = test::BeginRecording(GetDevice());
        recording.Commands->CopyBufferToBuffer(dst.get(), 0, src.get(), 0, BufferSize);
        recording.Commands->End();
        (destroySource ? src : dst).reset();
        CommandBuffer* commands[]{recording.Commands.get()};
        EXPECT_FALSE(test::DirectQueue(GetDevice()).Submit({.CmdBuffers = commands}));
        EXPECT_EQ(TakeRhiValidationErrors(), 1u);
    }
}

TEST_P(BufferTest, PartialUploadFeedsAConcurrentReadbackConsumer) {
    auto& device = GetDevice();
    auto& queue = test::DirectQueue(device);
    auto upload = CreateMappable(MemoryType::Upload);
    auto readback = CreateMappable(MemoryType::ReadBack);
    auto fence = device.CreateFence().Unwrap();
    const auto expected = Pattern();
    auto* mapped = upload->GetMappedData().Get();
    ASSERT_NE(mapped, nullptr);
    std::memcpy(mapped, expected.data(), BufferSize / 2);
    ASSERT_TRUE(upload->FlushMappedRange({0, BufferSize / 2}));

    std::binary_semaphore consumed{0};
    std::jthread reader{[&] {
        for (uint64_t value = 1; value <= 32; ++value) {
            const auto status = fence->WaitForStatus(value, 5'000'000'000);
            EXPECT_EQ(status, FenceStatus::Completed);
            if (status == FenceStatus::Completed) {
                auto* data = readback->GetMappedData().Get();
                EXPECT_NE(data, nullptr);
                if (data) {
                    EXPECT_TRUE(readback->InvalidateMappedRange({0, BufferSize / 2}));
                    EXPECT_EQ(std::memcmp(data, expected.data(), BufferSize / 2), 0);
                }
            }
            consumed.release();
            if (status != FenceStatus::Completed) return;
        }
    }};
    for (uint64_t value = 1; value <= 32; ++value) {
        auto recording = test::BeginRecording(device);
        recording.Commands->CopyBufferToBuffer(readback.get(), 0, upload.get(), 0, BufferSize / 2);
        test::Barrier(*recording.Commands, {BarrierBufferDescriptor{readback.get(), BufferState::CopyDestination, BufferState::HostRead}});
        recording.Commands->End();
        CommandBuffer* commands[]{recording.Commands.get()};
        Fence* signals[]{fence.get()};
        ASSERT_TRUE(queue.Submit({.CmdBuffers = commands, .SignalFences = signals, .SignalValues = {&value, 1}}));
        ASSERT_TRUE(consumed.try_acquire_for(std::chrono::seconds{6}));
    }
}

TEST_P(BufferTest, PipelineReadablePersistentUploadCopiesToReadback) {
    const auto expected = Pattern();
    auto upload = GetDevice().CreateBuffer({.Size = BufferSize,
                                           .Memory = MemoryType::Upload,
                                           .Usage = BufferUse::MapWrite | BufferUse::CopySource | BufferUse::CBuffer | BufferUse::Vertex});
    ASSERT_TRUE(upload);
    auto readback = CreateMappable(MemoryType::ReadBack);
    void* written = upload->GetMappedData().Get();
    ASSERT_NE(written, nullptr);
    std::memcpy(written, expected.data(), expected.size());
    ASSERT_TRUE(upload->FlushMappedRange({0, BufferSize}));
    CopyToHost(*upload, *readback);

    EXPECT_EQ(test::MapAndRead(*readback), expected);
}

TEST_P(BufferTest, CacheOperationsValidateDirectionAndRange) {
    auto upload = CreateMappable(MemoryType::Upload);
    auto readback = CreateMappable(MemoryType::ReadBack);
    EXPECT_FALSE(upload->InvalidateMappedRange({0, BufferSize}));
    EXPECT_FALSE(readback->FlushMappedRange({0, BufferSize}));
    EXPECT_FALSE(upload->FlushMappedRange({1, BufferSize}));
    EXPECT_FALSE(readback->InvalidateMappedRange({1, BufferSize}));
    EXPECT_EQ(TakeRhiValidationErrors(), 4u);
}

TEST_P(BufferTest, WaitingForLaterQueueSubmissionConsumesBothOverwrites) {
    auto& device = GetDevice();
    auto& queue = test::DirectQueue(device);
    const uint32_t value = 42;
    auto upload = test::CreateUploadBuffer(device, test::AsBytes(value), BufferUse::CopySource).Unwrap();
    auto readback = test::CreateReadbackBuffer(device, sizeof(value)).Unwrap();
    auto firstFence = device.CreateFence().Unwrap();
    auto lastFence = device.CreateFence().Unwrap();
    auto first = test::BeginRecording(device);
    auto second = test::BeginRecording(device);
    for (auto* recording : {&first, &second}) {
        recording->Commands->CopyBufferToBuffer(readback.get(), 0, upload.get(), 0, sizeof(value));
        recording->Commands->End();
    }
    CommandBuffer* firstCommands[]{first.Commands.get()}, *lastCommands[]{second.Commands.get()};
    Fence* firstSignal[]{firstFence.get()}, *lastSignal[]{lastFence.get()};
    uint64_t one[]{1}, three[]{3};
    ASSERT_TRUE(queue.Submit({.CmdBuffers = firstCommands, .SignalFences = firstSignal, .SignalValues = one}));
    ASSERT_TRUE(queue.Submit({.CmdBuffers = lastCommands, .SignalFences = lastSignal, .SignalValues = three, .WaitFences = firstSignal, .WaitValues = one}));
    // The first signal on lastFence crosses the CPU's wait threshold.
    ASSERT_EQ(lastFence->WaitForStatus(1, 3'000'000'000), FenceStatus::Completed);
    ASSERT_EQ(test::FromBytes<uint32_t>(test::MapAndRead(*readback)), (vector<uint32_t>{value}));
    ASSERT_NE(readback->GetMappedData().Get(), nullptr);

    EXPECT_EQ(TakeRhiValidationErrors(), 0u);
}

TEST_P(BufferTest, CompletedCopyAllowsIndependentBufferDestruction) {
    auto& device = GetDevice();
    const uint32_t value = 42;
    auto upload = test::CreateUploadBuffer(device, test::AsBytes(value), BufferUse::CopySource).Unwrap();
    auto readback = test::CreateReadbackBuffer(device, sizeof(value)).Unwrap();
    auto recording = test::BeginRecording(device);
    recording.Commands->CopyBufferToBuffer(readback.get(), 0, upload.get(), 0, sizeof(value));
    test::SubmitAndWait(device, recording);
    readback->Destroy();
    EXPECT_FALSE(readback->IsValid());
    EXPECT_EQ(TakeRhiValidationErrors(), 0u);
}

INSTANTIATE_TEST_SUITE_P(Backends, BufferTest, testing::ValuesIn(test::AllBackends), test::BackendParamName);

}  // namespace
}  // namespace radray::gpu
