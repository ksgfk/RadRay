#include "gpu_test_fixture.h"

#include <algorithm>
#include <limits>

#include <radray/gpu_impl/validation.h>
#ifdef RADRAY_ENABLE_VULKAN
#include <radray/gpu_impl/backend/vulkan_helper.h>
#endif

namespace radray::gpu {
namespace {

class TextureUploadTest : public test::BackendTest {};

TEST(TextureReadbackValidation, RejectsInvalidFormatGeometryAndBufferLayout) {
    TextureDescriptor texture{.Dim = TextureDimension::Dim3D, .Width = 12, .Height = 8, .DepthOrArraySize = 4,
                              .MipLevels = 2, .SampleCount = 1, .Format = TextureFormat::R8_UNORM, .Usage = TextureUse::CopySource};
    BufferDescriptor buffer{.Size = 512 + 4 * 256 + 3, .Usage = BufferUse::CopyDestination};
    DeviceLimits limits{};
    limits.TextureDataPlacementAlignment = 512;
    limits.TextureDataPitchAlignment = 256;
    const TextureBufferCopyRegion valid{{0, 0, 2, 1, 3, 2, 1, 2}, 512, 256, 3};
    auto check = [&](TextureBufferCopyRegion region) { return ValidateTextureReadbackRegion(texture, buffer, region, limits).first; };
    EXPECT_TRUE(check(valid));
    --buffer.Size;
    EXPECT_FALSE(check(valid));
    ++buffer.Size;
    auto region = valid;
    region.RowsPerImage = 1;
    EXPECT_FALSE(check(region));
    region = valid; region.RowPitch = 255;
    EXPECT_FALSE(check(region));
    region = valid; region.DestinationOffset = 1;
    EXPECT_FALSE(check(region));
    region = valid; region.Source.Width = 0;
    EXPECT_FALSE(check(region));
    region = valid; region.Source.X = UINT32_MAX;
    EXPECT_FALSE(check(region));
    region = valid; region.Source.Z = 3;
    EXPECT_FALSE(check(region));
    region = valid; region.Source.ArrayLayer = 1;
    EXPECT_FALSE(check(region));
    region = valid; region.Source.MipLevel = 2;
    EXPECT_FALSE(check(region));
    texture.Usage = TextureUse::CopyDestination;
    EXPECT_FALSE(check(valid));
    texture.Usage = TextureUse::CopySource;
    buffer.Usage = BufferUse::CopySource;
    EXPECT_FALSE(check(valid));
    buffer.Usage = BufferUse::CopyDestination;
    for (auto format : {TextureFormat::UNKNOWN, TextureFormat::D16_UNORM, TextureFormat::D24_UNORM_S8_UINT}) {
        texture.Format = format;
        EXPECT_FALSE(check(valid));
        EXPECT_FALSE(check({{0, 0, 0, 0, 12, 8, 0, 4}, 0, 256}));
    }
    texture.Format = TextureFormat::BC1_UNORM;
    limits.TextureDataPlacementAlignment = limits.TextureDataPitchAlignment = 1;
    region = {{1, 0, 4, 0, 2, 4}, 0, 8};
    EXPECT_TRUE(check(region));
    region.RowPitch = 9;
    EXPECT_FALSE(check(region));
    region.RowPitch = 8; region.RowsPerImage = UINT32_MAX;
    EXPECT_FALSE(check(region));
    region.RowsPerImage = 0; region.DestinationOffset = 1;
    EXPECT_FALSE(check(region));
    region.DestinationOffset = 0; region.Source.X = 1;
    EXPECT_FALSE(check(region));
}

#ifdef RADRAY_ENABLE_VULKAN
TEST(TextureReadbackValidation, QueueFamilyGranularityUsesCompressedBlockUnits) {
    TextureDescriptor texture{.Dim = TextureDimension::Dim2D, .Width = 18, .Height = 16, .DepthOrArraySize = 1,
                              .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::R8_UNORM};
    auto supported = [&](TextureRegion2D image, uint64_t offset, VkQueueFlags flags, VkExtent3D step) {
        return vulkan::IsBufferImageTransferSupported(texture, image, offset, flags, step);
    };
    EXPECT_TRUE(supported({0, 0, 1, 1, 2, 2}, 1, VK_QUEUE_GRAPHICS_BIT, {1, 1, 1}));
    EXPECT_FALSE(supported({0, 0, 1, 1, 2, 2}, 4, VK_QUEUE_TRANSFER_BIT, {}));
    EXPECT_FALSE(supported({0, 0, 0, 0, 18, 16}, 1, VK_QUEUE_TRANSFER_BIT, {}));
    EXPECT_TRUE(supported({0, 0, 0, 0, 18, 16}, 4, VK_QUEUE_TRANSFER_BIT, {}));
    texture.Format = TextureFormat::BC1_UNORM;
    EXPECT_FALSE(supported({0, 0, 4, 0, 8, 8}, 0, VK_QUEUE_TRANSFER_BIT, {2, 2, 1}));
    EXPECT_TRUE(supported({0, 0, 8, 0, 8, 8}, 0, VK_QUEUE_TRANSFER_BIT, {2, 2, 1}));
    EXPECT_TRUE(supported({0, 0, 16, 0, 2, 8}, 0, VK_QUEUE_TRANSFER_BIT, {2, 2, 1}));
}
#endif

TEST_P(TextureUploadTest, RegionTargetsSelectedCubeFace) {
    auto& device = GetDevice();
    auto texture = device.CreateTexture({.Dim = TextureDimension::Cube, .Width = 4, .Height = 4, .DepthOrArraySize = 6,
                                         .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::R8_UNORM,
                                         .Usage = TextureUse::CopyDestination | TextureUse::CopySource}).Unwrap();
    const uint32_t pitch = test::CopyRowPitch(device, 4, 1);
    vector<byte> pixels(4 * pitch, byte{0x6d});
    auto source = test::CreateUploadBuffer(device, pixels, BufferUse::CopySource).Unwrap();
    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    test::Barrier(commands, {BarrierBufferDescriptor{.Target = source.get(), .Before = BufferState::HostWrite, .After = BufferState::CopySource},
                             BarrierTextureDescriptor{.Target = texture.get(), .Before = TextureState::Undefined, .After = TextureState::CopyDestination}});
    const bool copied = commands.CopyBufferToTextureRegion({source.get(), texture.get(), {0, pitch, {0, 3, 0, 0, 4, 4}}});
    test::SubmitAndWait(device, recording);
    ASSERT_TRUE(copied);
    EXPECT_EQ(test::ReadTexture(device, *texture, TextureState::CopyDestination, 3), vector<byte>(16, byte{0x6d}));
}

TEST_P(TextureUploadTest, SmallestCompressedMipRoundTripsOneBlock) {
    auto& device = GetDevice();
    auto texture = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1,
                                         .MipLevels = 4, .SampleCount = 1, .Format = TextureFormat::BC1_UNORM,
                                         .Usage = TextureUse::CopyDestination | TextureUse::CopySource}).Unwrap();
    const uint32_t pitch = test::CopyRowPitch(device, 1, 8);
    const vector<byte> block{byte{0x1f}, byte{0x7c}, byte{0xe0}, byte{0x03}, byte{1}, byte{2}, byte{3}, byte{4}};
    auto upload = test::CreateUploadBuffer(device, block, BufferUse::CopySource).Unwrap();
    auto readback = test::CreateReadbackBuffer(device, pitch).Unwrap();
    auto regionResult = test::CreateReadbackBuffer(device, block.size()).Unwrap();
    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    const SubresourceRange mip{0, 1, 3, 1};
    test::Barrier(commands, {BarrierBufferDescriptor{.Target = upload.get(), .Before = BufferState::HostWrite, .After = BufferState::CopySource},
                             BarrierTextureDescriptor{.Target = texture.get(), .Before = TextureState::Undefined, .After = TextureState::CopyDestination, .Range = mip}});
    EXPECT_TRUE(commands.CopyBufferToTextureRegion({upload.get(), texture.get(), {0, pitch, {3, 0, 0, 0, 1, 1}}}));
    test::Barrier(commands, {BarrierTextureDescriptor{.Target = texture.get(), .Before = TextureState::CopyDestination, .After = TextureState::CopySource, .Range = mip}});
    commands.CopyTextureToBuffer(readback.get(), 0, texture.get(), mip);
    EXPECT_TRUE(commands.CopyTextureToBufferRegion({texture.get(), regionResult.get(), {{3, 0, 0, 0, 1, 1}, 0, pitch}}));
    test::Barrier(commands, {BarrierBufferDescriptor{.Target = regionResult.get(), .Before = BufferState::CopyDestination, .After = BufferState::HostRead}});
    test::Barrier(commands, {BarrierBufferDescriptor{.Target = readback.get(), .Before = BufferState::CopyDestination, .After = BufferState::HostRead}});
    test::SubmitAndWait(device, recording);
    const auto bytes = test::MapAndRead(*readback);
    ASSERT_GE(bytes.size(), block.size());
    EXPECT_TRUE(std::equal(block.begin(), block.end(), bytes.begin()));
    EXPECT_EQ(test::MapAndRead(*regionResult), block);
}

TEST_P(TextureUploadTest, BlockFormatsCannotBindAsTexelBuffers) {
    auto& device = GetDevice();
    const BindingDescriptor binding{.BindingId = 0, .Kind = BindingKind::TypedBuffer, .Count = 1, .Stages = ShaderStage::Compute,
                                    .D3D12 = D3D12BindingDescriptor{BindingRegisterClass::SRV, 0, 0}, .Vulkan = VulkanBindingDescriptor{0, 0}};
    auto group = device.CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
    auto buffer = device.CreateBuffer({.Size = 256, .Memory = MemoryType::DeviceLocal, .Usage = BufferUse::Resource}).Unwrap();
    auto set = device.CreateShaderParameterSet({group.get()}).Unwrap();
    EXPECT_FALSE(set->Set(0, 0, ShaderTexelBufferBinding{buffer.get(), {0, 16}, TextureFormat::BC7_UNORM}));
    EXPECT_EQ(TakeRhiValidationErrors(), 1u);
}

TEST_P(TextureUploadTest, RegionReadbackPreservesPaddingAndNonzeroDestinationOffset) {
    auto& device = GetDevice();
    for (auto format : {TextureFormat::RGBA8_UNORM, TextureFormat::BC1_UNORM}) {
        auto texture = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 32, .Height = 32, .DepthOrArraySize = 1,
            .MipLevels = 2, .SampleCount = 1, .Format = format, .Usage = TextureUse::CopyDestination | TextureUse::CopySource}).Unwrap();
        const auto block = GetTextureFormatBlockInfo(format);
        const uint32_t pitch = test::CopyRowPitch(device, block.Columns(16), block.BytesPerBlock);
        const uint32_t offset = std::max(512u, static_cast<uint32_t>(device.GetCapabilities().Limits.TextureDataPlacementAlignment));
        vector<byte> pattern(pitch * block.Rows(16));
        for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = byte((17 * i + i / pitch) & 255);
        vector<byte> expected(offset + pitch * block.Rows(8) + 16, byte{0xad});
        auto source = test::CreateUploadBuffer(device, pattern, BufferUse::CopySource).Unwrap();
        auto sentinel = test::CreateUploadBuffer(device, expected, BufferUse::CopySource).Unwrap();
        auto output = test::CreateReadbackBuffer(device, expected.size()).Unwrap();
        auto recording = test::BeginRecording(device);
        auto& commands = *recording.Commands;
        test::Barrier(commands, {BarrierBufferDescriptor{source.get(), BufferState::HostWrite, BufferState::CopySource},
                                 BarrierBufferDescriptor{sentinel.get(), BufferState::HostWrite, BufferState::CopySource},
                                 BarrierTextureDescriptor{.Target = texture.get(), .Before = TextureState::Undefined, .After = TextureState::CopyDestination}});
        commands.CopyBufferToBuffer(output.get(), 0, sentinel.get(), 0, expected.size());
        test::Barrier(commands, {BarrierBufferDescriptor{output.get(), BufferState::CopyDestination, BufferState::CopyDestination}});
        ASSERT_TRUE(commands.CopyBufferToTextureRegion({source.get(), texture.get(), {0, pitch, {1, 0, 0, 0, 16, 16}}}));
        test::Barrier(commands, {BarrierTextureDescriptor{.Target = texture.get(), .Before = TextureState::CopyDestination, .After = TextureState::CopySource}});
        ASSERT_TRUE(commands.CopyTextureToBufferRegion({texture.get(), output.get(), {{1, 0, 4, 4, 4, 8}, offset, pitch}}));
        test::Barrier(commands, {BarrierBufferDescriptor{output.get(), BufferState::CopyDestination, BufferState::HostRead}});
        test::SubmitAndWait(device, recording);
        for (uint32_t row = 0; row < block.Rows(8); ++row)
            std::copy_n(pattern.begin() + (4 / block.BlockHeight + row) * pitch + 4 / block.BlockWidth * block.BytesPerBlock,
                        block.Columns(4) * block.BytesPerBlock, expected.begin() + offset + row * pitch);
        EXPECT_EQ(test::MapAndRead(*output), expected);
    }
}

TEST_P(TextureUploadTest, VolumeUploadHonorsRowStrideBetweenSlicesAndDestinationZ) {
    auto& device = GetDevice();
    auto texture = device.CreateTexture({.Dim = TextureDimension::Dim3D, .Width = 4, .Height = 2, .DepthOrArraySize = 3,
                                         .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::R8_UNORM,
                                         .Usage = TextureUse::CopyDestination | TextureUse::CopySource}).Unwrap();
    const uint32_t pitch = test::CopyRowPitch(device, 4, 1);
    const uint32_t rowsPerImage = 3;
    vector<byte> pixels((rowsPerImage + 1) * pitch + 4, byte{0xcd});
    for (uint32_t slice = 0; slice < 2; ++slice) {
        for (uint32_t y = 0; y < 2; ++y) {
            std::fill_n(pixels.begin() + (slice * rowsPerImage + y) * pitch, 4, byte(10 * slice + y));
        }
    }
    auto upload = test::CreateUploadBuffer(device, pixels, BufferUse::CopySource).Unwrap();
    auto readback = test::CreateReadbackBuffer(device, 6 * pitch).Unwrap();
    auto tightTail = test::CreateReadbackBuffer(device, uint64_t{rowsPerImage + 1} * pitch + 2).Unwrap();
    vector<byte> expected(2 * rowsPerImage * pitch, byte{0x91});
    auto padded = test::CreateReadbackBuffer(device, expected.size()).Unwrap();
    auto sentinel = test::CreateUploadBuffer(device, expected, BufferUse::CopySource).Unwrap();
    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    test::Barrier(commands, {BarrierBufferDescriptor{.Target = upload.get(), .Before = BufferState::HostWrite, .After = BufferState::CopySource},
                             BarrierTextureDescriptor{.Target = texture.get(), .Before = TextureState::Undefined, .After = TextureState::CopyDestination}});
    EXPECT_TRUE(commands.CopyBufferToTextureRegion({upload.get(), texture.get(), {0, pitch, {0, 0, 0, 0, 4, 2, 1, 2}, rowsPerImage}}));
    test::Barrier(commands, {BarrierTextureDescriptor{.Target = texture.get(), .Before = TextureState::CopyDestination, .After = TextureState::CopySource}});
    commands.CopyTextureToBuffer(readback.get(), 0, texture.get(), {0, 1, 0, 1});
    const TextureBufferCopyRegion sliceRegion{{0, 0, 1, 0, 2, 2, 1, 2}, 0, pitch, rowsPerImage};
    test::Barrier(commands, {BarrierBufferDescriptor{sentinel.get(), BufferState::HostWrite, BufferState::CopySource}});
    commands.CopyBufferToBuffer(padded.get(), 0, sentinel.get(), 0, expected.size());
    commands.CopyBufferToBuffer(tightTail.get(), 0, sentinel.get(), 0, tightTail->GetDesc().Size);
    test::Barrier(commands, {BarrierBufferDescriptor{padded.get(), BufferState::CopyDestination, BufferState::CopyDestination},
                             BarrierBufferDescriptor{tightTail.get(), BufferState::CopyDestination, BufferState::CopyDestination}});
    EXPECT_TRUE(commands.CopyTextureToBufferRegion({texture.get(), padded.get(), sliceRegion}));
    test::Barrier(commands, {BarrierBufferDescriptor{padded.get(), BufferState::CopyDestination, BufferState::HostRead}});
    auto outside = sliceRegion;
    outside.Source.Z = 3;
    EXPECT_FALSE(commands.CopyTextureToBufferRegion({texture.get(), tightTail.get(), outside}));
    EXPECT_EQ(TakeRhiValidationErrors(), 1u);
    EXPECT_TRUE(commands.CopyTextureToBufferRegion({texture.get(), tightTail.get(), sliceRegion}));
    test::Barrier(commands, {BarrierBufferDescriptor{.Target = tightTail.get(), .Before = BufferState::CopyDestination, .After = BufferState::HostRead}});
    test::Barrier(commands, {BarrierBufferDescriptor{.Target = readback.get(), .Before = BufferState::CopyDestination, .After = BufferState::HostRead}});
    test::SubmitAndWait(device, recording);
    const auto bytes = test::MapAndRead(*readback);
    ASSERT_EQ(bytes.size(), 6 * pitch);
    const auto regionBytes = test::MapAndRead(*tightTail);
    for (uint32_t z = 1; z <= 2; ++z) {
        for (uint32_t y = 0; y < 2; ++y) {
            for (uint32_t x = 0; x < 2; ++x)
                expected[((z - 1) * rowsPerImage + y) * pitch + x] = byte(10 * (z - 1) + y);
            for (uint32_t x = 0; x < 4; ++x)
                EXPECT_EQ(bytes[(z * 2 + y) * pitch + x], byte(10 * (z - 1) + y));
        }
    }
    EXPECT_EQ(test::MapAndRead(*padded), expected);
    expected.resize(tightTail->GetDesc().Size);
    EXPECT_EQ(regionBytes, expected);
}

INSTANTIATE_TEST_SUITE_P(Backends, TextureUploadTest, testing::ValuesIn(test::AllBackends), test::BackendParamName);

}  // namespace
}  // namespace radray::gpu
