#include "render_test_framework.h"

#include <algorithm>
#include <array>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <radray/runtime/gpu_system.h>

namespace radray::render::test {
namespace {

class ResourceUploaderRuntimeTest : public ::testing::TestWithParam<TestBackend> {
protected:
    void SetUp() override {
        string reason;
        ASSERT_TRUE(_ctx.Initialize(GetParam(), &reason)) << reason;
        _ctx.ClearCapturedErrors();
    }

    void TearDown() override { _ctx.Reset(); }

    unique_ptr<Buffer> CreateBuffer(const BufferDescriptor& desc) {
        string reason;
        auto buffer = _ctx.CreateBuffer(desc, &reason);
        EXPECT_TRUE(buffer.HasValue()) << reason;
        return buffer.HasValue() ? buffer.Release() : nullptr;
    }

    unique_ptr<Texture> CreateTexture(const TextureDescriptor& desc) {
        string reason;
        auto texture = _ctx.CreateTexture(desc, &reason);
        EXPECT_TRUE(texture.HasValue()) << reason;
        return texture.HasValue() ? texture.Release() : nullptr;
    }

    ComputeTestContext _ctx;
};

TEST_P(ResourceUploaderRuntimeTest, UploadsBufferAndTextureFromNonZeroPageOffsets) {
    constexpr uint32_t textureWidth = 3;
    constexpr uint32_t textureHeight = 2;
    constexpr uint32_t textureBpp = 4;
    constexpr uint64_t textureRowBytes = textureWidth * textureBpp;

    std::array<byte, 12> dummyData{};
    std::array<byte, 20> bufferData{};
    std::array<byte, textureRowBytes * textureHeight> textureData{};
    for (size_t i = 0; i < dummyData.size(); ++i) {
        dummyData[i] = static_cast<byte>(0x20u + i);
    }
    for (size_t i = 0; i < bufferData.size(); ++i) {
        bufferData[i] = static_cast<byte>(0x40u + i);
    }
    for (size_t i = 0; i < textureData.size(); ++i) {
        textureData[i] = static_cast<byte>(0x80u + i);
    }

    auto dummy = CreateBuffer(BufferDescriptor{
        .Size = dummyData.size(),
        .Memory = MemoryType::Device,
        .Usage = BufferUse::CopyDestination});
    auto targetBuffer = CreateBuffer(BufferDescriptor{
        .Size = bufferData.size(),
        .Memory = MemoryType::Device,
        .Usage = BufferUse::CopySource | BufferUse::CopyDestination});
    auto targetTexture = CreateTexture(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = textureWidth,
        .Height = textureHeight,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::CopySource | TextureUse::CopyDestination});
    ASSERT_NE(dummy, nullptr);
    ASSERT_NE(targetBuffer, nullptr);
    ASSERT_NE(targetTexture, nullptr);

    const uint64_t textureRowPitch = AlignUp<uint64_t>(
        textureRowBytes,
        std::max<uint64_t>(1, _ctx.GetDeviceDetail().TextureDataPitchAlignment));
    const uint64_t textureReadbackSize = textureRowPitch * textureHeight;
    auto bufferReadback = CreateBuffer(BufferDescriptor{
        .Size = bufferData.size(),
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::CopyDestination | BufferUse::MapRead});
    auto textureReadback = CreateBuffer(BufferDescriptor{
        .Size = textureReadbackSize,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::CopyDestination | BufferUse::MapRead});
    ASSERT_NE(bufferReadback, nullptr);
    ASSERT_NE(textureReadback, nullptr);

    string reason;
    auto commandOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(commandOpt.HasValue()) << reason;
    auto command = commandOpt.Release();
    ResourceUploader uploader{_ctx.GetDevicePtr(), 1};

    command->Begin();
    uploader.UploadBuffer(command.get(), BufferUploadRequest{
                                             .SrcData = dummyData,
                                             .DstBuffer = dummy.get(),
                                             .Before = BufferState::Common,
                                             .After = BufferState::Common});
    uploader.UploadBuffer(command.get(), BufferUploadRequest{
                                             .SrcData = bufferData,
                                             .DstBuffer = targetBuffer.get(),
                                             .Before = BufferState::Common,
                                             .After = BufferState::CopySource});
    uploader.UploadTexture(command.get(), TextureUploadRequest{
                                              .SrcData = textureData,
                                              .DstTexture = targetTexture.get(),
                                              .DstRange = SubresourceRange{0, 1, 0, 1},
                                              .SrcRowPitch = textureRowBytes,
                                              .Before = TextureState::Undefined,
                                              .After = TextureState::CopySource});

    vector<ResourceBarrierDescriptor> readbackBarriers;
    _ctx.AppendReadbackPreCopyBarrier(readbackBarriers, bufferReadback.get());
    _ctx.AppendReadbackPreCopyBarrier(readbackBarriers, textureReadback.get());
    if (!readbackBarriers.empty()) {
        command->ResourceBarrier(readbackBarriers);
    }
    command->CopyBufferToBuffer(
        bufferReadback.get(),
        0,
        targetBuffer.get(),
        0,
        bufferData.size());
    command->CopyTextureToBuffer(
        textureReadback.get(),
        0,
        targetTexture.get(),
        SubresourceRange{0, 1, 0, 1});

    readbackBarriers.clear();
    _ctx.AppendReadbackPostCopyBarrier(readbackBarriers, bufferReadback.get());
    _ctx.AppendReadbackPostCopyBarrier(readbackBarriers, textureReadback.get());
    if (!readbackBarriers.empty()) {
        command->ResourceBarrier(readbackBarriers);
    }
    command->End();
    uploader.EndFlight(0);

    ASSERT_TRUE(_ctx.SubmitAndWait(command.get(), &reason)) << reason;
    uploader.CollectFlight(0);

    const auto bufferResult = _ctx.ReadHostVisibleBuffer(bufferReadback.get(), bufferData.size(), &reason);
    ASSERT_TRUE(bufferResult.has_value()) << reason;
    EXPECT_TRUE(std::equal(bufferData.begin(), bufferData.end(), bufferResult->begin()))
        << "expected " << DescribeBytes(bufferData) << ", actual " << DescribeBytes(*bufferResult);

    const auto textureResult = _ctx.ReadHostVisibleBuffer(textureReadback.get(), textureReadbackSize, &reason);
    ASSERT_TRUE(textureResult.has_value()) << reason;
    for (uint32_t row = 0; row < textureHeight; ++row) {
        const auto expected = std::span<const byte>{textureData}.subspan(row * textureRowBytes, textureRowBytes);
        const auto actual = std::span<const byte>{*textureResult}.subspan(row * textureRowPitch, textureRowBytes);
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), actual.begin()))
            << "row " << row << ": expected " << DescribeBytes(expected)
            << ", actual " << DescribeBytes(actual);
    }
    EXPECT_TRUE(_ctx.GetCapturedErrors().empty()) << _ctx.JoinCapturedErrors();
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    ResourceUploaderRuntimeTest,
    ::testing::ValuesIn(GetEnabledTestBackends()),
    [](const ::testing::TestParamInfo<TestBackend>& info) {
        return string{fmt::format("{}", info.param)};
    });

}  // namespace
}  // namespace radray::render::test
