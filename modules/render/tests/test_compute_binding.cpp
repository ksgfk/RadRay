#include "render_test_framework.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace radray::render::test {
namespace {

constexpr std::string_view kDynamicCBufferShader = R"(
struct InputData {
    uint Value;
    uint3 Padding;
};

[[vk::binding(0, 0)]] ConstantBuffer<InputData> gInput : register(b0, space0);
[[vk::binding(0, 1)]] RWStructuredBuffer<uint> gOutput : register(u0, space1);

[numthreads(1, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    gOutput[tid.x] = gInput.Value;
}
)";

constexpr std::string_view kPushConstantShader = R"(
struct PushData {
    uint Value;
    uint3 Padding;
};

[[vk::push_constant]] ConstantBuffer<PushData> gPush : register(b0, space0);
[[vk::binding(0, 1)]] RWStructuredBuffer<uint> gOutput : register(u0, space1);

[numthreads(1, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    gOutput[tid.x] = gPush.Value;
}
)";

struct ConstantData {
    uint32_t Value{0};
    uint32_t Padding[3]{};
};
static_assert(sizeof(ConstantData) == 16);

DescriptorPoolDescriptor MakeTestPoolDescriptor() {
    return DescriptorPoolDescriptor{
        .MaxBindingGroups = 4,
        .MaxSampledTextures = 0,
        .MaxStorageTextures = 0,
        .MaxUniformBuffers = 0,
        .MaxDynamicUniformBuffers = 2,
        .MaxStorageBuffers = 2,
        .MaxReadOnlyTexelBuffers = 0,
        .MaxReadWriteTexelBuffers = 0,
        .MaxSamplers = 0,
        .MaxAccelerationStructures = 0,
        .Lifetime = DescriptorPoolLifetime::Persistent};
}

class ComputeBindingRuntimeTest : public ::testing::TestWithParam<TestBackend> {
protected:
    void SetUp() override {
        string reason{};
        ASSERT_TRUE(_ctx.Initialize(GetParam(), &reason)) << reason;
        _ctx.ClearCapturedErrors();
    }

    void TearDown() override {
        _ctx.Reset();
    }

    unique_ptr<Buffer> CreateBuffer(const BufferDescriptor& desc) {
        string reason{};
        auto buffer = _ctx.CreateBuffer(desc, &reason);
        EXPECT_TRUE(buffer.HasValue()) << reason;
        return buffer.HasValue() ? buffer.Release() : nullptr;
    }

    unique_ptr<DescriptorPool> CreatePool(const DescriptorPoolDescriptor& desc) {
        string reason{};
        auto pool = _ctx.CreateDescriptorPool(desc, &reason);
        EXPECT_TRUE(pool.HasValue()) << reason;
        return pool.HasValue() ? pool.Release() : nullptr;
    }

    unique_ptr<BindingGroup> CreateGroup(
        DescriptorPool* pool,
        PipelineLayout* layout,
        uint32_t groupIndex) {
        string reason{};
        auto group = _ctx.CreateBindingGroup(pool, layout, groupIndex, &reason);
        EXPECT_TRUE(group.HasValue()) << reason;
        return group.HasValue() ? group.Release() : nullptr;
    }

    vector<uint32_t> Readback(Buffer* buffer, size_t count) {
        string reason{};
        auto data = _ctx.ReadHostVisibleBuffer(buffer, count * sizeof(uint32_t), &reason);
        EXPECT_TRUE(data.has_value()) << reason;
        if (!data.has_value()) {
            return {};
        }
        vector<uint32_t> values(count);
        std::memcpy(values.data(), data->data(), data->size());
        return values;
    }

    void RecordOutputReadback(
        CommandBuffer* command,
        Buffer* output,
        Buffer* readback,
        uint64_t size) {
        vector<ResourceBarrierDescriptor> barriers{};
        barriers.push_back(BarrierBufferDescriptor{
            .Target = output,
            .Before = BufferState::UnorderedAccess,
            .After = BufferState::CopySource});
        _ctx.AppendReadbackPreCopyBarrier(barriers, readback);
        command->ResourceBarrier(barriers);
        command->CopyBufferToBuffer(readback, 0, output, 0, size);
        barriers.clear();
        _ctx.AppendReadbackPostCopyBarrier(barriers, readback);
        if (!barriers.empty()) {
            command->ResourceBarrier(barriers);
        }
    }

    ComputeTestContext _ctx{};
};

TEST_P(ComputeBindingRuntimeTest, DynamicUniformBufferOffsetSelectsRecord) {
    string reason{};
    const DynamicBufferBinding dynamicBinding{.Group = 0, .Binding = 0};
    auto programOpt = _ctx.CreateComputeProgram(
        kDynamicCBufferShader,
        "CSMain",
        false,
        &reason,
        {},
        std::span{&dynamicBinding, 1});
    ASSERT_TRUE(programOpt.has_value()) << reason;
    auto program = std::move(programOpt.value());

    const uint64_t alignment = std::max<uint64_t>(
        256, _ctx.GetDeviceDetail().CBufferAlignment);
    const uint64_t constantBufferSize = alignment + sizeof(ConstantData);
    auto constants = CreateBuffer(BufferDescriptor{
        .Size = constantBufferSize,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::CBuffer | BufferUse::MapWrite,
        .Hints = ResourceHint::PersistentMap});
    ASSERT_NE(constants, nullptr);

    vector<byte> constantBytes(constantBufferSize, byte{0});
    const ConstantData first{.Value = 17};
    const ConstantData second{.Value = 91};
    std::memcpy(constantBytes.data(), &first, sizeof(first));
    std::memcpy(constantBytes.data() + alignment, &second, sizeof(second));
    ASSERT_TRUE(_ctx.WriteHostVisibleBuffer(constants.get(), constantBytes, &reason)) << reason;

    auto output = CreateBuffer(BufferDescriptor{
        .Size = sizeof(uint32_t),
        .Memory = MemoryType::Device,
        .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource,
        .Hints = ResourceHint::None});
    auto readback = CreateBuffer(BufferDescriptor{
        .Size = sizeof(uint32_t),
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::CopyDestination | BufferUse::MapRead,
        .Hints = ResourceHint::None});
    ASSERT_NE(output, nullptr);
    ASSERT_NE(readback, nullptr);

    auto pool = CreatePool(MakeTestPoolDescriptor());
    ASSERT_NE(pool, nullptr);
    auto objectGroup = CreateGroup(pool.get(), program.PipelineLayout, 0);
    auto outputGroup = CreateGroup(pool.get(), program.PipelineLayout, 1);
    ASSERT_NE(objectGroup, nullptr);
    ASSERT_NE(outputGroup, nullptr);

    ASSERT_TRUE(objectGroup->SetResource(0, BufferBindingDescriptor{
        .Target = constants.get(),
        .Range = BufferRange{0, sizeof(ConstantData)},
        .Usage = BufferViewUsage::CBuffer}));
    ASSERT_TRUE(outputGroup->SetResource(0, BufferBindingDescriptor{
        .Target = output.get(),
        .Range = BufferRange{0, sizeof(uint32_t)},
        .Stride = sizeof(uint32_t),
        .Usage = BufferViewUsage::ReadWriteStorage}));

    auto commandOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(commandOpt.HasValue()) << reason;
    auto command = commandOpt.Release();
    command->Begin();
    vector<ResourceBarrierDescriptor> preDispatch{};
    _ctx.AppendHostWriteBarrier(preDispatch, constants.get(), BufferState::CBuffer);
    preDispatch.push_back(BarrierBufferDescriptor{
        .Target = output.get(),
        .Before = BufferState::Common,
        .After = BufferState::UnorderedAccess});
    command->ResourceBarrier(preDispatch);

    auto encoderOpt = command->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    encoder->BindComputePipelineState(program.PipelineObject.get());
    const uint32_t dynamicOffset = static_cast<uint32_t>(alignment);
    encoder->BindBindingGroup(0, objectGroup.get(), std::span{&dynamicOffset, 1});
    encoder->BindBindingGroup(1, outputGroup.get());
    encoder->Dispatch(1, 1, 1);
    command->EndComputePass(std::move(encoder));
    RecordOutputReadback(command.get(), output.get(), readback.get(), sizeof(uint32_t));
    command->End();

    ASSERT_TRUE(_ctx.SubmitAndWait(command.get(), &reason)) << reason;
    EXPECT_EQ(Readback(readback.get(), 1), vector<uint32_t>{second.Value});
    EXPECT_TRUE(_ctx.GetCapturedErrors().empty()) << _ctx.JoinCapturedErrors();
}

TEST_P(ComputeBindingRuntimeTest, DynamicOffsetValidationRejectsMisalignment) {
    string reason{};
    const DynamicBufferBinding dynamicBinding{.Group = 0, .Binding = 0};
    auto programOpt = _ctx.CreateComputeProgram(
        kDynamicCBufferShader,
        "CSMain",
        false,
        &reason,
        {},
        std::span{&dynamicBinding, 1});
    ASSERT_TRUE(programOpt.has_value()) << reason;
    auto program = std::move(programOpt.value());

    const uint64_t alignment = std::max<uint64_t>(
        256, _ctx.GetDeviceDetail().CBufferAlignment);
    auto constants = CreateBuffer(BufferDescriptor{
        .Size = alignment * 2,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::CBuffer | BufferUse::MapWrite,
        .Hints = ResourceHint::None});
    auto pool = CreatePool(MakeTestPoolDescriptor());
    auto group = CreateGroup(pool.get(), program.PipelineLayout, 0);
    ASSERT_NE(constants, nullptr);
    ASSERT_NE(group, nullptr);
    ASSERT_TRUE(group->SetResource(0, BufferBindingDescriptor{
        .Target = constants.get(),
        .Range = BufferRange{0, sizeof(ConstantData)},
        .Usage = BufferViewUsage::CBuffer}));

    auto commandOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(commandOpt.HasValue()) << reason;
    auto command = commandOpt.Release();
    command->Begin();
    auto encoderOpt = command->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    encoder->BindComputePipelineState(program.PipelineObject.get());
    const uint32_t misaligned = 4;
    _ctx.ClearCapturedErrors();
    encoder->BindBindingGroup(0, group.get(), std::span{&misaligned, 1});
    command->EndComputePass(std::move(encoder));
    command->End();

    EXPECT_NE(_ctx.JoinCapturedErrors().find("not aligned"), string::npos)
        << _ctx.JoinCapturedErrors();
}

TEST_P(ComputeBindingRuntimeTest, DescriptorPoolCapacityAndResetAreBackendSpecific) {
    string reason{};
    const PushConstantBinding pushConstant{.Group = 0, .Binding = 0};
    auto programOpt = _ctx.CreateComputeProgram(
        kPushConstantShader, "CSMain", false, &reason, {}, {}, std::span{&pushConstant, 1});
    ASSERT_TRUE(programOpt.has_value()) << reason;
    auto program = std::move(programOpt.value());

    DescriptorPoolDescriptor desc = MakeTestPoolDescriptor();
    desc.MaxBindingGroups = 1;
    desc.MaxDynamicUniformBuffers = 0;
    desc.MaxStorageBuffers = 1;
    auto pool = CreatePool(desc);
    ASSERT_NE(pool, nullptr);

    auto first = CreateGroup(pool.get(), program.PipelineLayout, 1);
    ASSERT_NE(first, nullptr);
    const bool hasNativeDescriptorPool = GetParam() == TestBackend::Vulkan;
    EXPECT_EQ(pool->GetAllocatedBindingGroupCount(), hasNativeDescriptorPool ? 1u : 0u);
    EXPECT_EQ(pool->Reset(), !hasNativeDescriptorPool);

    _ctx.ClearCapturedErrors();
    auto second = _ctx.CreateBindingGroup(pool.get(), program.PipelineLayout, 1, &reason);
    EXPECT_EQ(second.HasValue(), !hasNativeDescriptorPool);
    EXPECT_EQ(pool->GetAllocatedBindingGroupCount(), hasNativeDescriptorPool ? 1u : 0u);

    first.reset();
    EXPECT_EQ(pool->GetAllocatedBindingGroupCount(), 0u);
    EXPECT_TRUE(pool->Reset());
    auto reused = CreateGroup(pool.get(), program.PipelineLayout, 1);
    EXPECT_NE(reused, nullptr);
}

TEST_P(ComputeBindingRuntimeTest, DescriptorPoolPerTypeCapacityIsBackendSpecific) {
    string reason{};
    const DynamicBufferBinding dynamicBinding{.Group = 0, .Binding = 0};
    auto programOpt = _ctx.CreateComputeProgram(
        kDynamicCBufferShader,
        "CSMain",
        false,
        &reason,
        {},
        std::span{&dynamicBinding, 1});
    ASSERT_TRUE(programOpt.has_value()) << reason;
    auto program = std::move(programOpt.value());

    DescriptorPoolDescriptor noDynamicCapacity = MakeTestPoolDescriptor();
    noDynamicCapacity.MaxDynamicUniformBuffers = 0;
    auto noDynamicPool = CreatePool(noDynamicCapacity);
    ASSERT_NE(noDynamicPool, nullptr);
    _ctx.ClearCapturedErrors();
    auto rejectedDynamic = _ctx.CreateBindingGroup(
        noDynamicPool.get(), program.PipelineLayout, 0, &reason);
    EXPECT_EQ(rejectedDynamic.HasValue(), GetParam() == TestBackend::D3D12);

    DescriptorPoolDescriptor noStorageCapacity = MakeTestPoolDescriptor();
    noStorageCapacity.MaxDynamicUniformBuffers = 1;
    noStorageCapacity.MaxStorageBuffers = 0;
    auto noStoragePool = CreatePool(noStorageCapacity);
    ASSERT_NE(noStoragePool, nullptr);
    auto objectGroup = CreateGroup(noStoragePool.get(), program.PipelineLayout, 0);
    ASSERT_NE(objectGroup, nullptr);
    _ctx.ClearCapturedErrors();
    auto rejectedStorage = _ctx.CreateBindingGroup(
        noStoragePool.get(), program.PipelineLayout, 1, &reason);
    EXPECT_EQ(rejectedStorage.HasValue(), GetParam() == TestBackend::D3D12);
    _ctx.ClearCapturedErrors();
}

TEST_P(ComputeBindingRuntimeTest, ExplicitPushConstantsWorkAndValidateSize) {
    string reason{};
    const PushConstantBinding pushConstant{.Group = 0, .Binding = 0};
    auto programOpt = _ctx.CreateComputeProgram(
        kPushConstantShader, "CSMain", false, &reason, {}, {}, std::span{&pushConstant, 1});
    ASSERT_TRUE(programOpt.has_value()) << reason;
    auto program = std::move(programOpt.value());

    auto output = CreateBuffer(BufferDescriptor{
        .Size = sizeof(uint32_t),
        .Memory = MemoryType::Device,
        .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource,
        .Hints = ResourceHint::None});
    auto readback = CreateBuffer(BufferDescriptor{
        .Size = sizeof(uint32_t),
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::CopyDestination | BufferUse::MapRead,
        .Hints = ResourceHint::None});
    auto pool = CreatePool(MakeTestPoolDescriptor());
    auto outputGroup = CreateGroup(pool.get(), program.PipelineLayout, 1);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(readback, nullptr);
    ASSERT_NE(outputGroup, nullptr);
    ASSERT_TRUE(outputGroup->SetResource(0, BufferBindingDescriptor{
        .Target = output.get(),
        .Range = BufferRange{0, sizeof(uint32_t)},
        .Stride = sizeof(uint32_t),
        .Usage = BufferViewUsage::ReadWriteStorage}));

    auto commandOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(commandOpt.HasValue()) << reason;
    auto command = commandOpt.Release();
    command->Begin();
    ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{
        .Target = output.get(),
        .Before = BufferState::Common,
        .After = BufferState::UnorderedAccess};
    command->ResourceBarrier(std::span{&toUav, 1});
    auto encoderOpt = command->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    encoder->BindComputePipelineState(program.PipelineObject.get());

    const uint32_t wrongSize = 1;
    _ctx.ClearCapturedErrors();
    EXPECT_FALSE(encoder->SetPushConstants(
        program.PipelineLayout, 0, 0, BytesOf(wrongSize)));
    EXPECT_NE(_ctx.JoinCapturedErrors().find("size mismatch"), string::npos);

    const ConstantData push{.Value = 73};
    _ctx.ClearCapturedErrors();
    ASSERT_TRUE(encoder->SetPushConstants(
        program.PipelineLayout, 0, 0, BytesOf(push)));
    encoder->BindBindingGroup(1, outputGroup.get());
    encoder->Dispatch(1, 1, 1);
    command->EndComputePass(std::move(encoder));
    RecordOutputReadback(command.get(), output.get(), readback.get(), sizeof(uint32_t));
    command->End();

    ASSERT_TRUE(_ctx.SubmitAndWait(command.get(), &reason)) << reason;
    EXPECT_EQ(Readback(readback.get(), 1), vector<uint32_t>{push.Value});
    EXPECT_TRUE(_ctx.GetCapturedErrors().empty()) << _ctx.JoinCapturedErrors();
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    ComputeBindingRuntimeTest,
    ::testing::ValuesIn(GetEnabledTestBackends()),
    [](const ::testing::TestParamInfo<TestBackend>& info) {
        return string{fmt::format("{}", info.param)};
    });

}  // namespace
}  // namespace radray::render::test
