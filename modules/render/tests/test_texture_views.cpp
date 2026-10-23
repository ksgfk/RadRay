#include "gpu_test_fixture.h"

namespace radray::render {
namespace {

class TextureViewContractTest : public testing::TestWithParam<RenderBackend> {
protected:
    test::DeviceContext Context;
    void SetUp() override {
        if (!test::TryCreateDevice(GetParam(), Context, true, true)) {
            if (test::SetupMustFail(Context.Status, test::RequiredBackend(GetParam()))) FAIL() << Context.Reason;
            GTEST_SKIP() << Context.Reason;
        }
    }
    void TearDown() override { Context.Reset(); }

    unique_ptr<Texture> MakeTexture(TextureFormat format, TextureUses usage) {
        return Context.Device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = format, .Usage = usage}).Unwrap();
    }

    TextureViewDescriptor View(Texture* texture, TextureFormat format, TextureViewUsage usage) {
        return {texture, TextureDimension::Dim2D, format, {0, 1, 0, 1}, usage};
    }

    void Reject(const TextureViewDescriptor& desc) {
        const auto before = Context.RhiValidationErrors.load();
        EXPECT_FALSE(Context.Device->CreateTextureView(desc));
        EXPECT_EQ(Context.RhiValidationErrors.load(), before + 1);
    }
};

TEST_P(TextureViewContractTest, CompatibleViewsHaveIndividualUsages) {
    auto texture = MakeTexture(TextureFormat::RGBA8_UNORM, TextureUse::Resource | TextureUse::RenderTarget | TextureUse::UnorderedAccess);
    auto sampled = Context.Device->CreateTextureView(View(texture.get(), TextureFormat::RGBA8_UNORM_SRGB, TextureViewUsage::Resource));
    auto target = Context.Device->CreateTextureView(View(texture.get(), TextureFormat::RGBA8_UNORM_SRGB, TextureViewUsage::RenderTarget));
    auto storage = Context.Device->CreateTextureView(View(texture.get(), TextureFormat::RGBA8_UNORM, TextureViewUsage::UnorderedAccess));
    auto original = Context.Device->CreateTextureView(View(texture.get(), TextureFormat::RGBA8_UNORM, TextureViewUsage::Resource));
    ASSERT_TRUE(sampled);
    ASSERT_TRUE(target);
    ASSERT_TRUE(storage);
    ASSERT_TRUE(original);
    Reject(View(texture.get(), TextureFormat::RGBA8_UNORM_SRGB, TextureViewUsage::UnorderedAccess));
    Reject(View(texture.get(), TextureFormat::RGBA16_FLOAT, TextureViewUsage::Resource));
    Reject(View(texture.get(), TextureFormat::D32_FLOAT, TextureViewUsage::RenderTarget));
}

TEST_P(TextureViewContractTest, CompatibilityIsBackendSpecific) {
    auto texture = MakeTexture(TextureFormat::RGBA8_UNORM, TextureUse::Resource);
    auto desc = View(texture.get(), TextureFormat::R32_UINT, TextureViewUsage::Resource);
    if (GetParam() == RenderBackend::Vulkan) {
        auto view = Context.Device->CreateTextureView(desc);
        ASSERT_TRUE(view);
        EXPECT_EQ(view.Get()->GetDesc().Format, TextureFormat::R32_UINT);
    } else {
        Reject(desc);
    }
    Reject(View(texture.get(), TextureFormat::RGBA8_UNORM, TextureViewUsage::UnorderedAccess));
}

TEST_P(TextureViewContractTest, BgraUnormSrgbPair) {
    auto texture = MakeTexture(TextureFormat::BGRA8_UNORM, TextureUse::Resource | TextureUse::RenderTarget);
    ASSERT_TRUE(Context.Device->CreateTextureView(View(texture.get(), TextureFormat::BGRA8_UNORM_SRGB, TextureViewUsage::Resource)));
    ASSERT_TRUE(Context.Device->CreateTextureView(View(texture.get(), TextureFormat::BGRA8_UNORM_SRGB, TextureViewUsage::RenderTarget)));
}

TEST_P(TextureViewContractTest, DepthAndStencilViewsKeepTheirPlanes) {
    auto texture = MakeTexture(TextureFormat::D32_FLOAT, TextureUse::Resource | TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite);
    ASSERT_TRUE(Context.Device->CreateTextureView(View(texture.get(), TextureFormat::D32_FLOAT, TextureViewUsage::Resource)));
    ASSERT_TRUE(Context.Device->CreateTextureView(View(texture.get(), TextureFormat::D32_FLOAT, TextureViewUsage::DepthWrite)));
    const auto color = View(texture.get(), TextureFormat::R32_FLOAT, TextureViewUsage::Resource);
    if (GetParam() == RenderBackend::D3D12)
        ASSERT_TRUE(Context.Device->CreateTextureView(color));
    else
        Reject(color);

    const auto format = TextureFormat::D32_FLOAT_S8_UINT;
    const auto usage = TextureUse::Resource | TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite;
    if (!Context.Device->QueryTextureSupport({TextureDimension::Dim2D, format, usage}).Supported)
        GTEST_SKIP() << "D32_FLOAT_S8_UINT sampled depth/stencil is unavailable";
    auto combined = MakeTexture(format, usage);
    auto desc = View(combined.get(), format, TextureViewUsage::Resource);
    desc.Range.Aspects = TextureAspect::Depth;
    ASSERT_TRUE(Context.Device->CreateTextureView(desc));
    desc.Range.Aspects = TextureAspect::Stencil;
    ASSERT_TRUE(Context.Device->CreateTextureView(desc));
    desc.Range.Aspects = TextureAspect::Depth | TextureAspect::Stencil;
    Reject(desc);
}

TEST_P(TextureViewContractTest, SrgbAttachmentClearPreservesLinearStorageInterpretation) {
    auto texture = MakeTexture(TextureFormat::RGBA8_UNORM, TextureUse::RenderTarget | TextureUse::CopySource);
    auto view = Context.Device->CreateTextureView(View(texture.get(), TextureFormat::RGBA8_UNORM_SRGB, TextureViewUsage::RenderTarget)).Unwrap();
    const auto alignment = std::max<uint64_t>(1, Context.Device->GetCapabilities().Detail.TextureDataPitchAlignment);
    const auto rowPitch = (32 + alignment - 1) / alignment * alignment;
    auto readback = Context.Device->CreateBuffer({.Size = rowPitch * 8, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    auto allocator = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = Context.Device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor toRender = BarrierTextureDescriptor{texture.get(), TextureState::Undefined, TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&toRender, 1});
    const RenderPassColorAttachment attachment{view.get(), LoadAction::Clear, StoreAction::Store, {{0.5f, 0.5f, 0.5f, 1.0f}}};
    auto encoder = command->BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = 8, .Height = 8}).Unwrap();
    command->EndRenderPass(std::move(encoder));
    const ResourceBarrierDescriptor toCopy = BarrierTextureDescriptor{texture.get(), TextureState::RenderTarget, TextureState::CopySource};
    command->ResourceBarrier(std::span{&toCopy, 1});
    command->CopyTextureToBuffer(readback.get(), 0, texture.get(), {0, 1, 0, 1});
    command->End();
    CommandBuffer* commands[]{command.get()};
    test::GpuWorkGuard gpuWork{Context};
    Context.Queue->Submit({.CmdBuffers = commands});
    Context.Queue->Wait();
    const auto* data = static_cast<const uint8_t*>(readback->Map(0, rowPitch * 8));
    ASSERT_NE(data, nullptr);
    readback->InvalidateMappedRange({0, rowPitch * 8});
    for (uint32_t c = 0; c < 3; ++c) EXPECT_NEAR(data[c], 188, 1);
    EXPECT_EQ(data[3], 255);
    readback->Unmap();
    allocator->Reset();
    EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
}

TEST_P(TextureViewContractTest, CommandRetainsViewAndTextureUntilNativeReset) {
    auto texture = MakeTexture(TextureFormat::RGBA8_UNORM, TextureUse::RenderTarget);
    auto view = Context.Device->CreateTextureView(View(texture.get(), TextureFormat::RGBA8_UNORM, TextureViewUsage::RenderTarget)).Unwrap();
    auto allocator = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = Context.Device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor toRender = BarrierTextureDescriptor{texture.get(), TextureState::Undefined, TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&toRender, 1});
    const RenderPassColorAttachment attachment{view.get(), LoadAction::Clear, StoreAction::Store, {}};
    auto encoder = command->BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = 8, .Height = 8}).Unwrap();
    command->EndRenderPass(std::move(encoder));
    command->End();
    view->Destroy();
    EXPECT_TRUE(view->IsValid());
    texture->Destroy();
    EXPECT_TRUE(texture->IsValid());
    EXPECT_EQ(Context.RhiValidationErrors.load(), 2u);
    CommandBuffer* commands[]{command.get()};
    test::GpuWorkGuard gpuWork{Context};
    Context.Queue->Submit({.CmdBuffers = commands});
    Context.Queue->Wait();
    view->Destroy();
    EXPECT_TRUE(view->IsValid());
    EXPECT_EQ(Context.RhiValidationErrors.load(), 3u);
    allocator->Reset();
    view->Destroy();
    EXPECT_FALSE(view->IsValid());
    texture->Destroy();
    EXPECT_FALSE(texture->IsValid());
    EXPECT_EQ(Context.RhiValidationErrors.load(), 3u);
}

INSTANTIATE_TEST_SUITE_P(Backends, TextureViewContractTest, testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan));

}  // namespace
}  // namespace radray::render
