#include "gpu_test_fixture.h"

#include <gtest/gtest.h>
#include <radray/utility.h>

namespace radray::render {
namespace {

TextureDescriptor ColorDesc() {
    return {.Dim = TextureDimension::Dim2D, .Width = 32, .Height = 32, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Memory = MemoryType::Device, .Usage = TextureUse::RenderTarget | TextureUse::Resource | TextureUse::CopySource};
}

TEST(TextureDescriptorValidation, RejectsIllegalCombinationsAndNativeLimits) {
    RenderDeviceCapabilities caps{};
    caps.Limits = {.MaxTexture1DDimension = 4096, .MaxTexture2DDimension = 4096, .MaxTexture3DDimension = 256, .MaxTextureArrayLayers = 256};
    TextureSupport support{.Supported = true, .SampleCounts = SampleCount::X1 | SampleCount::X4, .MaxWidth = 2048, .MaxHeight = 2048, .MaxDepth = 256, .MaxArrayLayers = 128, .MaxMipLevels = 12, .MaxResourceSize = uint64_t{1} << 30};
    const auto valid = ColorDesc();
    ASSERT_TRUE(ValidateTextureDescriptor(valid, caps, support).Supported);
    const auto reject = [&](TextureDescriptor desc) {
        const auto result = ValidateTextureDescriptor(desc, caps, support);
        EXPECT_FALSE(result.Supported);
        EXPECT_FALSE(result.Reason.empty());
    };
    auto desc = valid;
    desc.Width = 0;
    reject(desc);
    desc = valid;
    desc.Width = 8192;
    reject(desc);
    desc = valid;
    desc.Width = 3000;
    reject(desc);
    desc = valid;
    desc.SampleCount = 3;
    reject(desc);
    desc = valid;
    desc.SampleCount = 2;
    reject(desc);
    desc = valid;
    desc.SampleCount = 4;
    desc.MipLevels = 2;
    reject(desc);
    desc = valid;
    desc.SampleCount = 4;
    desc.Usage |= TextureUse::UnorderedAccess;
    reject(desc);
    desc = valid;
    desc.MipLevels = 7;
    reject(desc);
    desc = valid;
    desc.Format = TextureFormat::D32_FLOAT;
    reject(desc);
    desc = valid;
    desc.Usage = TextureUse::DepthStencilWrite;
    reject(desc);
    desc = valid;
    desc.Usage = TextureUses{256u};
    reject(desc);
    desc = valid;
    desc.Memory = MemoryType::Upload;
    reject(desc);
    desc = valid;
    desc.Hints = ResourceHint::External;
    reject(desc);
    desc = valid;
    desc.Dim = TextureDimension::Dim1D;
    reject(desc);
    desc = valid;
    desc.Dim = TextureDimension::Cube;
    desc.DepthOrArraySize = 5;
    reject(desc);
    desc = valid;
    desc.Dim = TextureDimension::Dim2DArray;
    desc.DepthOrArraySize = 129;
    reject(desc);
    support.MaxResourceSize = 32;
    reject(valid);
}

TEST(TextureDescriptorValidation, NormalizesAllSubresourcesAndRejectsOverflow) {
    auto desc = ColorDesc();
    desc.Dim = TextureDimension::Dim2DArray;
    desc.DepthOrArraySize = 4;
    desc.MipLevels = 5;
    EXPECT_EQ(NormalizeSubresourceRange(desc, SubresourceRange::AllSub()), (SubresourceRange{0, 4, 0, 5}));
    EXPECT_EQ(NormalizeSubresourceRange(desc, {2, SubresourceRange::All, 3, SubresourceRange::All}), (SubresourceRange{2, 2, 3, 2}));
    EXPECT_FALSE(NormalizeSubresourceRange(desc, {4, 1, 0, 1}));
    EXPECT_FALSE(NormalizeSubresourceRange(desc, {0, 1, 0, 0}));
    EXPECT_FALSE(NormalizeSubresourceRange(desc, {1, UINT32_MAX - 1, 0, 1}));
    desc.Dim = TextureDimension::Dim3D;
    EXPECT_EQ(NormalizeSubresourceRange(desc, SubresourceRange::AllSub()), (SubresourceRange{0, 1, 0, 5}));
}

class DeviceCapabilitiesTest : public testing::TestWithParam<RenderBackend> {
protected:
    test::DeviceContext Context;
    void SetUp() override {
        if (!test::TryCreateDevice(GetParam(), Context, true)) GTEST_SKIP() << "Backend unavailable";
    }
    void TearDown() override {
        if (Context.Device) {
            Context.Queue->Wait();
            Context.Device.reset();
        }
        EXPECT_EQ(Context.ValidationErrors.load(), 0u);
    }
};

TEST_P(DeviceCapabilitiesTest, LimitsAndCreatedQueuesAreConsistent) {
    const auto& caps = Context.Device->GetCapabilities();
    EXPECT_EQ(caps.Detail.CBufferAlignment, Context.Device->GetDetail().CBufferAlignment);
    EXPECT_EQ(caps.Limits.CBufferOffsetAlignment, caps.Detail.CBufferAlignment);
    EXPECT_GE(caps.Limits.StorageBufferOffsetAlignment, 1u);
    EXPECT_GT(caps.Limits.MaxBufferSize, 0u);
    EXPECT_GT(caps.Limits.MaxColorAttachments, 0u);
    EXPECT_GE(caps.Limits.MaxTexture2DDimension, 4096u);
    EXPECT_GT(caps.Queues[0].CreatedCount, 0u);
    for (size_t type = 0; type < caps.Queues.size(); ++type) {
        for (uint32_t slot = 0; slot < caps.Queues[type].CreatedCount; ++slot)
            EXPECT_TRUE(Context.Device->GetCommandQueue(static_cast<QueueType>(type), slot));
        EXPECT_FALSE(Context.Device->GetCommandQueue(static_cast<QueueType>(type), caps.Queues[type].CreatedCount));
    }
    EXPECT_TRUE(caps.Features.SubresourceBarriers);
    EXPECT_TRUE(caps.Features.UavMemoryBarrier);
    EXPECT_TRUE(caps.Features.UavWriteStages.HasFlag(ShaderStage::Compute));
    if (GetParam() == RenderBackend::D3D12) {
        EXPECT_TRUE(caps.Features.UavWriteStages.HasFlag(ShaderStage::Pixel));
    }
}

TEST_P(DeviceCapabilitiesTest, ReportedAttachmentFormatsAndSamplesCreate) {
    const TextureFormat formats[]{TextureFormat::RGBA8_UNORM, TextureFormat::BGRA8_UNORM, TextureFormat::RGBA16_FLOAT,
                                  TextureFormat::D32_FLOAT, TextureFormat::D24_UNORM_S8_UINT, TextureFormat::D16_UNORM};
    for (const auto format : formats) {
        const bool depth = IsDepthStencilFormat(format);
        auto desc = ColorDesc();
        desc.Format = format;
        desc.Usage = depth ? TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite : TextureUse::Resource | TextureUse::RenderTarget;
        const auto support = Context.Device->QueryTextureSupport({desc.Dim, format, desc.Usage});
        if (!support.Supported) continue;
        for (uint32_t samples = 1; samples <= 16; samples *= 2) {
            SCOPED_TRACE(fmt::format("format={} samples={}", format, samples));
            desc.SampleCount = samples;
            if (!support.SampleCounts.HasFlag(static_cast<SampleCount>(samples))) {
                EXPECT_FALSE(ValidateTextureDescriptor(desc, *Context.Device).Supported);
                continue;
            }
            auto texture = Context.Device->CreateTexture(desc);
            ASSERT_TRUE(texture);
            auto view = Context.Device->CreateTextureView({texture.Get(), desc.Dim, format, {0, 1, 0, 1}, depth ? TextureViewUsage::DepthWrite : TextureViewUsage::RenderTarget});
            ASSERT_TRUE(view);
            const RenderPassColorAttachmentDescriptor color{format, samples, LoadAction::Clear, StoreAction::Store};
            RenderPassDescriptor passDesc{};
            if (depth)
                passDesc.DepthStencilAttachment = {format, samples, LoadAction::Clear, StoreAction::Store, LoadAction::Clear, StoreAction::Store};
            else
                passDesc.ColorAttachments = std::span{&color, 1};
            auto pass = Context.Device->CreateRenderPass(passDesc);
            ASSERT_TRUE(pass);
            TextureView* colorView = view.Get();
            FramebufferDescriptor framebufferDesc{.Pass = pass.Get(), .Width = desc.Width, .Height = desc.Height};
            if (depth)
                framebufferDesc.DepthStencilAttachment = view.Get();
            else
                framebufferDesc.ColorAttachments = std::span{&colorView, 1};
            auto framebuffer = Context.Device->CreateFramebuffer(framebufferDesc);
            ASSERT_TRUE(framebuffer);
        }
    }
}

TEST_P(DeviceCapabilitiesTest, SubresourceRangeClearReadbackAndNestedLabels) {
    auto& device = *Context.Device;
    auto desc = ColorDesc();
    desc.Dim = TextureDimension::Dim2DArray;
    desc.DepthOrArraySize = 2;
    desc.MipLevels = 3;
    auto texture = device.CreateTexture(desc);
    ASSERT_TRUE(texture);
    const auto detail = device.GetCapabilities().Detail;
    const uint64_t pitch = Align(uint64_t{16 * 4}, detail.TextureDataPitchAlignment);
    auto readback = device.CreateBuffer({pitch * 16, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead, {}});
    ASSERT_TRUE(readback);
    auto view = device.CreateTextureView({texture.Get(), TextureDimension::Dim2DArray, desc.Format, {1, 1, 1, 1}, TextureViewUsage::RenderTarget});
    ASSERT_TRUE(view);
    const RenderPassColorAttachmentDescriptor color{desc.Format, 1, LoadAction::Clear, StoreAction::Store};
    auto pass = device.CreateRenderPass({std::span{&color, 1}, {}});
    ASSERT_TRUE(pass);
    TextureView* rawView = view.Get();
    auto framebuffer = device.CreateFramebuffer({pass.Get(), std::span{&rawView, 1}, nullptr, 16, 16, 1});
    ASSERT_TRUE(framebuffer);
    auto cmd = device.CreateCommandBuffer(Context.Queue);
    ASSERT_TRUE(cmd);
    cmd->Begin();
    cmd->PushDebugGroup("Capabilities");
    cmd->PushDebugGroup("Texture range");
    cmd->PushDebugGroup("Clear layer 1 mip 1");
    const ResourceBarrierDescriptor toTarget = BarrierTextureDescriptor{
        .Target = texture.Get(), .Before = TextureState::Undefined, .After = TextureState::RenderTarget, .IsSubresourceBarrier = true, .Range = {0, 2, 1, 2}};
    cmd->ResourceBarrier(std::span{&toTarget, 1});
    const ColorClearValue clear{{0.25f, 0.5f, 0.75f, 1.0f}};
    auto encoder = cmd->BeginRenderPass({pass.Get(), framebuffer.Get(), std::span{&clear, 1}, {}});
    ASSERT_TRUE(encoder);
    cmd->EndRenderPass(encoder.Release());
    const ResourceBarrierDescriptor toCopy = BarrierTextureDescriptor{
        .Target = texture.Get(), .Before = TextureState::RenderTarget, .After = TextureState::CopySource, .IsSubresourceBarrier = true, .Range = {0, 2, 1, 2}};
    cmd->ResourceBarrier(std::span{&toCopy, 1});
    cmd->CopyTextureToBuffer(readback.Get(), 0, texture.Get(), {1, 1, 1, 1});
    cmd->PopDebugGroup();
    cmd->PopDebugGroup();
    cmd->PopDebugGroup();
    cmd->End();
    CommandBuffer* rawCmd = cmd.Get();
    Context.Queue->Submit({.CmdBuffers = std::span{&rawCmd, 1}});
    Context.Queue->Wait();
    auto* mapped = static_cast<uint8_t*>(readback->Map(0, pitch * 16));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, pitch * 16});
    const auto* pixel = mapped + pitch * 7 + 7 * 4;
    EXPECT_NEAR(pixel[0], 64, 1);
    EXPECT_NEAR(pixel[1], 128, 1);
    EXPECT_NEAR(pixel[2], 191, 1);
    EXPECT_EQ(pixel[3], 255);
    readback->Unmap();
}

INSTANTIATE_TEST_SUITE_P(Backends, DeviceCapabilitiesTest, testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan));

}  // namespace
}  // namespace radray::render
