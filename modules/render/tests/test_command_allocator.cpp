#include "gpu_test_fixture.h"
#include <radray/basic_math.h>

namespace radray::render::test {
namespace {

class CommandAllocatorTest : public testing::TestWithParam<RenderBackend> {
protected:
    void SetUp() override {
        if (!TryCreateDevice(GetParam(), Context, true)) GTEST_SKIP() << Context.Reason;
    }
    void TearDown() override { EXPECT_EQ(Context.ValidationErrors.load(), 0u); }
    void CheckSharedStorage(bool wrapped);
    DeviceContext Context;
};

TEST_P(CommandAllocatorTest, Copies2dTextureIntoCubeFaceAndReadsThatLayer) {
    auto& device = *Context.Device;
    const uint64_t rowPitch = Align(2 * sizeof(uint32_t), std::max(1u, device.GetDetail().TextureDataPitchAlignment));
    const uint64_t bufferSize = rowPitch * 2;
    const size_t rowWords = static_cast<size_t>(rowPitch / sizeof(uint32_t));
    auto source = device.CreateTexture({TextureDimension::Dim2D, 2, 2, 1, 1, 1, TextureFormat::RGBA8_UNORM,
                                        MemoryType::Device, TextureUse::CopySource | TextureUse::CopyDestination});
    auto cube = device.CreateTexture({TextureDimension::Cube, 2, 2, 6, 1, 1, TextureFormat::RGBA8_UNORM,
                                      MemoryType::Device, TextureUse::CopySource | TextureUse::CopyDestination | TextureUse::Resource});
    auto upload = device.CreateBuffer({bufferSize, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite});
    auto readback = device.CreateBuffer({bufferSize, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead});
    auto storage = device.CreateCommandAllocator(Context.Queue);
    ASSERT_TRUE(source);
    ASSERT_TRUE(cube);
    ASSERT_TRUE(upload);
    ASSERT_TRUE(readback);
    ASSERT_TRUE(storage);
    auto command = device.CreateCommandBuffer(storage.Get());
    auto fence = device.CreateFence();
    ASSERT_TRUE(command);
    ASSERT_TRUE(fence);
    auto* pixels = static_cast<uint32_t*>(upload->Map(0, bufferSize));
    ASSERT_NE(pixels, nullptr);
    pixels[0] = 0x80402010;
    pixels[1] = 0x12345678;
    pixels[rowWords] = 0xff010203;
    pixels[rowWords + 1] = 0xffffffff;
    upload->FlushMappedRange({0, bufferSize});
    upload->Unmap();
    command->Begin();
    const ResourceBarrierDescriptor initial[]{BarrierTextureDescriptor{source.Get(), TextureState::Undefined, TextureState::CopyDestination},
                                              BarrierTextureDescriptor{cube.Get(), TextureState::Undefined, TextureState::CopyDestination}};
    command->ResourceBarrier(initial);
    ASSERT_TRUE(command->CopyBufferToTextureRegion({upload.Get(), source.Get(), {.RowPitch = static_cast<uint32_t>(rowPitch), .Width = 2, .Height = 2}}));
    ResourceBarrierDescriptor barrier = BarrierTextureDescriptor{source.Get(), TextureState::CopyDestination, TextureState::CopySource};
    command->ResourceBarrier(std::span{&barrier, 1});
    command->CopyTextureToTexture({.Destination = cube.Get(), .DestinationArrayLayer = 5, .Source = source.Get(), .Width = 2, .Height = 2});
    barrier = BarrierTextureDescriptor{cube.Get(), TextureState::CopyDestination, TextureState::CopySource};
    command->ResourceBarrier(std::span{&barrier, 1});
    command->CopyTextureToBuffer(readback.Get(), 0, cube.Get(), {5, 1, 0, 1});
    command->End();
    CommandBuffer* commands[]{command.Get()};
    Fence* fences[]{fence.Get()};
    uint64_t values[]{1};
    Context.Queue->Submit({.CmdBuffers = commands, .SignalFences = fences, .SignalValues = values});
    ASSERT_TRUE(fence->WaitFor(1, 5000));
    auto* observed = static_cast<const uint32_t*>(readback->Map(0, bufferSize));
    ASSERT_NE(observed, nullptr);
    readback->InvalidateMappedRange({0, bufferSize});
    EXPECT_EQ(observed[0], 0x80402010u);
    EXPECT_EQ(observed[1], 0x12345678u);
    EXPECT_EQ(observed[rowWords], 0xff010203u);
    EXPECT_EQ(observed[rowWords + 1], 0xffffffffu);
    readback->Unmap();
}

void CommandAllocatorTest::CheckSharedStorage(bool wrapped) {
    auto& device = *Context.Device;
    auto storage = device.CreateCommandAllocator(Context.Queue).Unwrap();
    storage->SetDebugName("Shared recording storage");
    vector<unique_ptr<CommandBuffer>> commands;
    for (uint32_t i = 0; i < 128; ++i) commands.push_back(device.CreateCommandBuffer(storage.get()).Unwrap());
    auto upload = device.CreateBuffer({512, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite}).Unwrap();
    auto readback = device.CreateBuffer({512, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    auto* mapped = static_cast<uint32_t*>(upload->Map(0, 512));
    ASSERT_NE(mapped, nullptr);
    for (uint32_t i = 0; i < 128; ++i) mapped[i] = 17 + i * 31;
    upload->FlushMappedRange({0, 512});
    upload->Unmap();
    for (uint32_t overlap : {1u, 2u, 4u}) {
        if (GetParam() == RenderBackend::D3D12 && overlap != 1) continue;
        for (uint32_t count : {1u, 8u, 32u, 128u, 1u}) {
            storage->Reset();
            vector<CommandBuffer*> submission;
            for (uint32_t begin = 0; begin < count; begin += overlap) {
                const uint32_t end = std::min(count, begin + overlap);
                for (uint32_t i = begin; i < end; ++i) {
                    auto* command = commands[i].get();
                    command->Begin();
                    if (i == 0 && GetParam() == RenderBackend::Vulkan) {
                        const ResourceBarrierDescriptor barriers[]{
                            BarrierBufferDescriptor{upload.get(), BufferState::HostWrite, BufferState::CopySource},
                            BarrierBufferDescriptor{readback.get(), BufferState::Undefined, BufferState::CopyDestination}};
                        command->ResourceBarrier(barriers);
                    }
                    command->CopyBufferToBuffer(readback.get(), i * 4, upload.get(), i * 4, 4);
                    if (i + 1 == count && GetParam() == RenderBackend::Vulkan) {
                        const ResourceBarrierDescriptor barrier = BarrierBufferDescriptor{readback.get(), BufferState::CopyDestination, BufferState::HostRead};
                        command->ResourceBarrier(std::span{&barrier, 1});
                    }
                    submission.push_back(command);
                }
                // End order is independent of submission order.
                for (uint32_t i = end; i > begin; --i) commands[i - 1]->End();
            }
#if defined(RADRAY_ENABLE_D3D12)
            if (!wrapped && GetParam() == RenderBackend::D3D12) {
                auto* native = static_cast<d3d12::CommandAllocatorD3D12*>(storage.get());
                EXPECT_NE(native->_native.Get(), nullptr);
                for (const auto& command : commands) EXPECT_EQ(static_cast<d3d12::CmdListD3D12*>(command.get())->_allocator, native);
            }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
            if (!wrapped && GetParam() == RenderBackend::Vulkan) {
                auto* native = static_cast<vulkan::CommandAllocatorVulkan*>(storage.get());
                EXPECT_EQ(native->_children.size(), commands.size());
                for (const auto& command : commands) EXPECT_EQ(static_cast<vulkan::CommandBufferVulkan*>(command.get())->_cmdPool, native);
            }
#endif
            Context.Queue->Submit({.CmdBuffers = submission});
            Context.Queue->Wait();
            auto* values = static_cast<uint32_t*>(readback->Map(0, count * 4));
            ASSERT_NE(values, nullptr);
            readback->InvalidateMappedRange({0, count * 4});
            for (uint32_t i = 0; i < count; ++i) EXPECT_EQ(values[i], 17 + i * 31);
            readback->Unmap();
        }
    }
    storage->Reset();
    commands.front()->Destroy();
    commands.front()->Destroy();
    commands.erase(commands.begin());
    auto* sibling = commands.back().get();
    sibling->Begin();
    sibling->End();
    Context.Queue->Submit({.CmdBuffers = std::span{&sibling, 1}});
    Context.Queue->Wait();
    commands.clear();
    storage->Destroy();
    storage->Destroy();
    EXPECT_FALSE(storage->IsValid());
}

TEST_P(CommandAllocatorTest, SharesNativeStorageAndReusesAfterCompletion) {
    CheckSharedStorage(false);
}

TEST_P(CommandAllocatorTest, IndependentAllocatorsAllowInterleavedRecordings) {
    vector<unique_ptr<CommandAllocator>> storages;
    vector<unique_ptr<CommandBuffer>> commands;
    vector<CommandBuffer*> submission;
    for (uint32_t i = 0; i < 4; ++i) {
        storages.push_back(Context.Device->CreateCommandAllocator(Context.Queue).Unwrap());
        commands.push_back(Context.Device->CreateCommandBuffer(storages.back().get()).Unwrap());
        commands.back()->Begin();
        submission.push_back(commands.back().get());
    }
    for (auto it = commands.rbegin(); it != commands.rend(); ++it) (*it)->End();
    Context.Queue->Submit({.CmdBuffers = submission});
    Context.Queue->Wait();
    for (auto& storage : storages) storage->Reset();
}

TEST_P(CommandAllocatorTest, ConsumesEncodersImmediatelyAndAllowsGroupedReset) {
    auto storage = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
    vector<unique_ptr<CommandBuffer>> commands;
    for (uint32_t i = 0; i < 64; ++i) {
        auto command = Context.Device->CreateCommandBuffer(storage.get()).Unwrap();
        command->Begin();
        auto encoder = command->BeginComputePass().Unwrap();
        command->EndComputePass(std::move(encoder));
        EXPECT_FALSE(encoder);
        command->End();
        commands.push_back(std::move(command));
    }
    // Unsubmitted recordings may be discarded as a group.
    storage->Reset();

    commands.front()->Begin();
    commands.front()->End();
}

class CommandAllocatorValidationTest : public CommandAllocatorTest {
    void SetUp() override {
        if (!TryCreateDevice(GetParam(), Context, true, true)) GTEST_SKIP() << Context.Reason;
    }
};

TEST_P(CommandAllocatorValidationTest, SharesNativeStorageAndReusesAfterCompletion) {
    CheckSharedStorage(true);
    EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
}

TEST_P(CommandAllocatorValidationTest, RecordingOverlapFollowsBackendAndRejectedBeginCanRetry) {
    auto storage = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
    auto first = Context.Device->CreateCommandBuffer(storage.get()).Unwrap();
    auto second = Context.Device->CreateCommandBuffer(storage.get()).Unwrap();
    first->Begin();
    second->Begin();
    if (GetParam() == RenderBackend::D3D12) {
        EXPECT_EQ(Context.RhiValidationErrors.load(), 1u);
        first->End();
        second->Begin();
        second->End();
        EXPECT_EQ(Context.RhiValidationErrors.load(), 1u);
    } else {
        second->End();
        first->End();
        EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
    }
    CommandBuffer* submission[]{first.get(), second.get()};
    Context.Queue->Submit({.CmdBuffers = submission});
    Context.Queue->Wait();
    storage->Reset();
}

TEST_P(CommandAllocatorValidationTest, RejectsInvalidRecordingAndOwnershipTransitions) {
    auto storage = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = Context.Device->CreateCommandBuffer(storage.get()).Unwrap();
    auto* raw = command.get();
    {
        const auto errors = Context.RhiValidationErrors.load();
        storage->Destroy();
        EXPECT_GT(Context.RhiValidationErrors.load(), errors);
    }
    {
        const auto errors = Context.RhiValidationErrors.load();
        command->End();
        EXPECT_GT(Context.RhiValidationErrors.load(), errors);
    }
    command->Begin();
    {
        const auto errors = Context.RhiValidationErrors.load();
        command->Begin();
        EXPECT_GT(Context.RhiValidationErrors.load(), errors);
    }
    {
        const auto errors = Context.RhiValidationErrors.load();
        storage->Reset();
        EXPECT_GT(Context.RhiValidationErrors.load(), errors);
    }
    auto encoder = command->BeginComputePass().Unwrap();
    {
        const auto errors = Context.RhiValidationErrors.load();
        command->End();
        EXPECT_GT(Context.RhiValidationErrors.load(), errors);
    }
    command->EndComputePass(std::move(encoder));
    command->End();
    {
        const auto errors = Context.RhiValidationErrors.load();
        command->Begin();
        EXPECT_GT(Context.RhiValidationErrors.load(), errors);
    }
    {
        const auto errors = Context.RhiValidationErrors.load();
        command->End();
        EXPECT_GT(Context.RhiValidationErrors.load(), errors);
    }
    storage->Reset();
    {
        const auto errors = Context.RhiValidationErrors.load();
        Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
        EXPECT_GT(Context.RhiValidationErrors.load(), errors);
    }
}

INSTANTIATE_TEST_SUITE_P(Backends, CommandAllocatorTest, testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan));
INSTANTIATE_TEST_SUITE_P(Backends, CommandAllocatorValidationTest, testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan));
}  // namespace
}  // namespace radray::render::test
