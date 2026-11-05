#include "gpu_test_fixture.h"

namespace radray::render {
namespace {

constexpr uint32_t Size = 4;

class TextureViewTest : public test::BackendTest {
protected:
    unique_ptr<Texture> CreateTexture(TextureFormat format, TextureUses usage, TextureDimension dim = TextureDimension::Dim2D, uint32_t layers = 1) {
        return GetDevice().CreateTexture({.Dim = dim, .Width = Size, .Height = Size, .DepthOrArraySize = layers, .MipLevels = 1, .SampleCount = 1,
                                          .Format = format, .Usage = usage})
            .Unwrap();
    }

    void ExpectRejected(const TextureViewDescriptor& desc) {
        EXPECT_FALSE(GetDevice().CreateTextureView(desc));
        EXPECT_EQ(TakeRhiValidationErrors(), 1u);
    }

    void Clear(TextureView& view, std::array<float, 4> color) {
        auto recording = test::BeginRecording(GetDevice());
        auto& commands = *recording.Commands;
        const RenderPassColorAttachment attachment{&view, LoadAction::Clear, StoreAction::Store, {color}};
        auto encoder = commands.BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = Size, .Height = Size}).Unwrap();
        commands.EndRenderPass(std::move(encoder));
        test::SubmitAndWait(GetDevice(), recording);
    }

    void ToRenderTarget(Texture& texture) {
        auto recording = test::BeginRecording(GetDevice());
        test::Barrier(*recording.Commands, {BarrierTextureDescriptor{.Target = &texture, .Before = TextureState::Undefined, .After = TextureState::RenderTarget}});
        test::SubmitAndWait(GetDevice(), recording);
    }
};

TEST_P(TextureViewTest, FormatOfDifferentSizeIsRejected) {
    auto texture = CreateTexture(TextureFormat::RGBA8_UNORM, TextureUse::Resource);

    ExpectRejected({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA16_FLOAT, {0, 1, 0, 1}, TextureViewUsage::Resource});
}

TEST_P(TextureViewTest, UsageMissingFromTextureIsRejected) {
    auto texture = CreateTexture(TextureFormat::RGBA8_UNORM, TextureUse::Resource);

    ExpectRejected({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget});
}

TEST_P(TextureViewTest, MipOutsideTextureIsRejected) {
    auto texture = CreateTexture(TextureFormat::RGBA8_UNORM, TextureUse::Resource);

    ExpectRejected({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 1, 1}, TextureViewUsage::Resource});
}

TEST_P(TextureViewTest, LayersOutsideTextureAreRejected) {
    auto texture = CreateTexture(TextureFormat::RGBA8_UNORM, TextureUse::Resource, TextureDimension::Dim2DArray, 2);

    ExpectRejected({texture.get(), TextureDimension::Dim2DArray, TextureFormat::RGBA8_UNORM, {1, 2, 0, 1}, TextureViewUsage::Resource});
}

TEST_P(TextureViewTest, MixedDepthStencilPermissionRequiresStencilAspect) {
    auto texture = CreateTexture(TextureFormat::D32_FLOAT, TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite);

    ExpectRejected({texture.get(), TextureDimension::Dim2D, TextureFormat::D32_FLOAT, {0, 1, 0, 1}, TextureViewUsage::DepthReadStencilWrite});
}

TEST_P(TextureViewTest, SrgbViewEncodesIntoUnormTexture) {
    auto texture = CreateTexture(TextureFormat::RGBA8_UNORM, TextureUse::RenderTarget | TextureUse::CopySource);
    auto view = GetDevice().CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM_SRGB, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();

    ToRenderTarget(*texture);
    Clear(*view, {0.5f, 0.5f, 0.5f, 1.0f});

    // Linear 0.5 encodes to sRGB 188.
    const auto texels = test::ReadTexture(GetDevice(), *texture, TextureState::RenderTarget);
    ASSERT_EQ(texels.size(), Size * Size * 4);
    for (size_t i = 0; i < texels.size(); i += 4) {
        for (size_t c = 0; c < 3; ++c) EXPECT_NEAR(static_cast<int>(texels[i + c]), 188, 1) << "byte " << i + c;
        EXPECT_EQ(static_cast<int>(texels[i + 3]), 255);
    }
}

TEST_P(TextureViewTest, LayerViewWritesOnlyItsLayer) {
    auto& device = GetDevice();
    auto texture = CreateTexture(TextureFormat::RGBA8_UNORM, TextureUse::RenderTarget | TextureUse::CopySource, TextureDimension::Dim2DArray, 2);
    auto first = device.CreateTextureView({texture.get(), TextureDimension::Dim2DArray, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    auto second = device.CreateTextureView({texture.get(), TextureDimension::Dim2DArray, TextureFormat::RGBA8_UNORM, {1, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();

    ToRenderTarget(*texture);
    Clear(*first, {1.0f, 0.0f, 0.0f, 1.0f});
    Clear(*second, {0.0f, 0.0f, 1.0f, 1.0f});

    const auto red = vector<uint32_t>(Size * Size, 0xff0000ff);
    const auto blue = vector<uint32_t>(Size * Size, 0xffff0000);
    EXPECT_EQ(test::FromBytes<uint32_t>(test::ReadTexture(device, *texture, TextureState::RenderTarget, 0)), red);
    EXPECT_EQ(test::FromBytes<uint32_t>(test::ReadTexture(device, *texture, TextureState::RenderTarget, 1)), blue);
}

INSTANTIATE_TEST_SUITE_P(Backends, TextureViewTest, testing::ValuesIn(test::AllBackends), test::BackendParamName);

}  // namespace
}  // namespace radray::render
