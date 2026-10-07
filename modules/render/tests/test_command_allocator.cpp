#include "gpu_test_fixture.h"

namespace radray::render::test {
namespace {

class CommandAllocatorTest : public testing::TestWithParam<RenderBackend> {
protected:
    void SetUp() override {
        if (!TryCreateDevice(GetParam(), Context, true)) GTEST_SKIP() << Context.Reason;
    }
    void TearDown() override { EXPECT_EQ(Context.ValidationErrors.load(), 0u); }
    DeviceContext Context;
};

TEST_P(CommandAllocatorTest, SharesStorageAtRecordingHighWaterAndReusesAfterCompletion) {
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
    size_t peak = 0;
    for (uint32_t overlap : {1u, 2u, 4u}) {
        for (uint32_t count : {1u, 8u, 32u, 128u, 1u}) {
            storage->Reset();
            vector<CommandBuffer*> submission;
            for (uint32_t begin = 0; begin < count; begin += overlap) {
                const uint32_t end = std::min(count, begin + overlap);
                peak = std::max(peak, size_t(end - begin));
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
            if (GetParam() == RenderBackend::D3D12) {
                auto* native = static_cast<d3d12::CommandAllocatorD3D12*>(storage.get());
                EXPECT_EQ(native->_native.size(), peak);
                EXPECT_EQ(native->_children.size(), commands.size());
            }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
            if (GetParam() == RenderBackend::Vulkan) {
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

TEST_P(CommandAllocatorTest, ReclaimsInactiveChildEncoderStateAtGroupedReset) {
    auto storage = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
    vector<unique_ptr<CommandBuffer>> commands;
    for (uint32_t i = 0; i < 64; ++i) {
        auto command = Context.Device->CreateCommandBuffer(storage.get()).Unwrap();
        command->Begin();
        auto encoder = command->BeginComputePass().Unwrap();
        command->EndComputePass(std::move(encoder));
        command->End();
        commands.push_back(std::move(command));
    }
    // Unsubmitted recordings may be discarded as a group.
    storage->Reset();
#if defined(RADRAY_ENABLE_VULKAN)
    if (GetParam() == RenderBackend::Vulkan)
        for (const auto& command : commands) EXPECT_TRUE(static_cast<vulkan::CommandBufferVulkan*>(command.get())->_endedEncoders.empty());
#endif
    commands.front()->Begin();
    commands.front()->End();
}

class CommandAllocatorDeathTest : public CommandAllocatorTest {};

TEST_P(CommandAllocatorDeathTest, RejectsInvalidRecordingAndOwnershipTransitions) {
    auto storage = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = Context.Device->CreateCommandBuffer(storage.get()).Unwrap();
    auto* raw = command.get();
    EXPECT_DEATH(storage->Destroy(), "");
    EXPECT_DEATH(command->End(), "");
    command->Begin();
    EXPECT_DEATH(command->Begin(), "");
    EXPECT_DEATH(storage->Reset(), "");
    auto encoder = command->BeginComputePass().Unwrap();
    EXPECT_DEATH(command->End(), "");
    command->EndComputePass(std::move(encoder));
    command->End();
    EXPECT_DEATH(command->Begin(), "");
    EXPECT_DEATH(command->End(), "");
    storage->Reset();
    EXPECT_DEATH(Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}}), "");
}

INSTANTIATE_TEST_SUITE_P(Backends, CommandAllocatorTest, testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan));
INSTANTIATE_TEST_SUITE_P(Backends, CommandAllocatorDeathTest, testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan));
}  // namespace
}  // namespace radray::render::test
