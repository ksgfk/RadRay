#include "gpu_test_fixture.h"
#include "foundation_shader_fixture.h"

#include <gtest/gtest.h>
#include <radray/utility.h>

namespace radray {
namespace {

class FoundationComputeTest : public testing::TestWithParam<render::RenderBackend> {};

TEST_P(FoundationComputeTest, SameStateUavOrdersBufferAndTextureWrites) {
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(GetParam(), context, true)) GTEST_SKIP() << "Backend unavailable";
    auto& device = *context.Device;
    constexpr std::string_view source = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) RWStructuredBuffer<uint> BufferOutput : register(u0);
VK_BINDING(1, 0) RWTexture2D<uint> TextureOutput : register(u1);
[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint next = BufferOutput[0] * 3 + 7;
    BufferOutput[0] = next;
    TextureOutput[uint2(0, 0)] = next;
}
)hlsl";
    const auto support = device.QueryTextureSupport({render::TextureDimension::Dim2D, render::TextureFormat::R32_UINT,
                                                     render::TextureUse::UnorderedAccess | render::TextureUse::CopySource});
    ASSERT_TRUE(support.Supported);
    auto program = test::CompileFoundationCompute(device, source);
    ASSERT_TRUE(program);
    auto* pipelineState = program->GetOrCreateComputePipelineState().Get();
    ASSERT_NE(pipelineState, nullptr);
    EXPECT_EQ(program->GetOrCreateComputePipelineState().Get(), pipelineState);
    EXPECT_EQ(program->GetComputePipelineStateCount(), 1u);
    auto buffer = device.CreateBuffer({4, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource | render::BufferUse::CopyDestination, {}});
    auto texture = device.CreateTexture({render::TextureDimension::Dim2D, 1, 1, 1, 1, 1, render::TextureFormat::R32_UINT, render::MemoryType::Device, render::TextureUse::UnorderedAccess | render::TextureUse::CopySource, {}});
    ASSERT_TRUE(buffer);
    ASSERT_TRUE(texture);
    auto view = device.CreateTextureView({texture.Get(), render::TextureDimension::Dim2D, render::TextureFormat::R32_UINT, {0, 1, 0, 1}, render::TextureViewUsage::UnorderedAccess});
    ASSERT_TRUE(view);
    const uint32_t initial = 5;
    auto upload = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{&initial, 1}), render::BufferUse::CopySource);
    ASSERT_TRUE(upload);
    const uint64_t textureOffset = Align(uint64_t{4}, device.GetCapabilities().Detail.TextureDataPlacementAlignment);
    const uint64_t readbackSize = textureOffset + Align(uint64_t{4}, device.GetCapabilities().Detail.TextureDataPitchAlignment);
    auto readback = device.CreateBuffer({readbackSize, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(readback);
    auto set = device.CreateShaderParameterSet({program->GetPipelineLayout(), 0});
    ASSERT_TRUE(set);
    ASSERT_TRUE(set->Set(program->GetPipelineLayout()->FindBinding("BufferOutput"), 0, render::ShaderBufferBinding{buffer.Get(), {0, 4}, 4}));
    ASSERT_TRUE(set->Set(program->GetPipelineLayout()->FindBinding("TextureOutput"), 0, view.Get()));
    ASSERT_TRUE(set->FlushWrites());
    auto command = device.CreateCommandBuffer(context.Queue);
    ASSERT_TRUE(command);
    command->Begin();
    const render::ResourceBarrierDescriptor initialBarrier = render::BarrierBufferDescriptor{
        .Target = buffer.Get(), .Before = render::BufferState::Undefined, .After = render::BufferState::CopyDestination};
    command->ResourceBarrier(std::span{&initialBarrier, 1});
    command->CopyBufferToBuffer(buffer.Get(), 0, upload.Get(), 0, 4);
    const render::ResourceBarrierDescriptor toUav[]{
        render::BarrierBufferDescriptor{.Target = buffer.Get(), .Before = render::BufferState::CopyDestination, .After = render::BufferState::UnorderedAccess},
        render::BarrierTextureDescriptor{.Target = texture.Get(), .Before = render::TextureState::Undefined, .After = render::TextureState::UnorderedAccess}};
    command->ResourceBarrier(toUav);
    for (uint32_t dispatch = 0; dispatch < 2; ++dispatch) {
        if (dispatch != 0) {
            const render::ResourceBarrierDescriptor barriers[]{render::BarrierUavDescriptor{buffer.Get()}, render::BarrierUavDescriptor{texture.Get()}};
            command->ResourceBarrier(barriers);
        }
        auto encoder = command->BeginComputePass();
        ASSERT_TRUE(encoder);
        encoder->BindComputePipelineState(pipelineState);
        encoder->BindShaderParameterSet(0, set.Get());
        encoder->Dispatch(1, 1, 1);
        command->EndComputePass(encoder.Release());
    }
    const render::ResourceBarrierDescriptor toCopy[]{
        render::BarrierBufferDescriptor{.Target = buffer.Get(), .Before = render::BufferState::UnorderedAccess, .After = render::BufferState::CopySource},
        render::BarrierTextureDescriptor{.Target = texture.Get(), .Before = render::TextureState::UnorderedAccess, .After = render::TextureState::CopySource}};
    command->ResourceBarrier(toCopy);
    command->CopyBufferToBuffer(readback.Get(), 0, buffer.Get(), 0, 4);
    command->CopyTextureToBuffer(readback.Get(), textureOffset, texture.Get(), {0, 1, 0, 1});
    const render::ResourceBarrierDescriptor host = render::BarrierBufferDescriptor{
        .Target = readback.Get(), .Before = render::BufferState::CopyDestination, .After = render::BufferState::HostRead};
    command->ResourceBarrier(std::span{&host, 1});
    command->End();
    render::CommandBuffer* raw = command.Get();
    context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
    context.Queue->Wait();
    auto* mapped = static_cast<const byte*>(readback->Map(0, readbackSize));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, readbackSize});
    uint32_t bufferValue{}, textureValue{};
    std::memcpy(&bufferValue, mapped, 4);
    std::memcpy(&textureValue, mapped + textureOffset, 4);
    EXPECT_EQ(bufferValue, (initial * 3 + 7) * 3 + 7);
    EXPECT_EQ(textureValue, bufferValue);
    readback->Unmap();
    EXPECT_EQ(context.ValidationErrors.load(), 0u);
}

INSTANTIATE_TEST_SUITE_P(Backends, FoundationComputeTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
