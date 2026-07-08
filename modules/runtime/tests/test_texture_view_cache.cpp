#include "render_test_framework.h"

#include <gtest/gtest.h>
#include <fmt/format.h>

#include <radray/render/gpu_resource.h>
#include <radray/runtime/texture_asset.h>

namespace radray::render::test {
namespace {

constexpr uint32_t kTexSize = 4;
constexpr uint32_t kMipLevels = 2;
constexpr TextureFormat kTexFormat = TextureFormat::RGBA8_UNORM;

// 直接构造一个 TextureAsset (device + texture + 默认全量 SRV), 验证 GetOrCreateSrv 的
// 去重语义: 默认描述返回 _srv; 非默认子 view 按 descriptor 去重, 同描述返同指针。
class TextureViewCacheTest : public ::testing::TestWithParam<TestBackend> {
protected:
    void SetUp() override {
        string reason;
        if (!_ctx.Initialize(this->GetParam(), &reason)) {
            GTEST_SKIP() << fmt::format("Init failed on {}: {}", format_as(this->GetParam()), reason);
        }
    }

    ComputeTestContext _ctx{};
};

TEST_P(TextureViewCacheTest, DedupAndDefault) {
    string reason;

    TextureDescriptor texDesc{};
    texDesc.Dim = TextureDimension::Dim2D;
    texDesc.Width = kTexSize;
    texDesc.Height = kTexSize;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = kMipLevels;
    texDesc.SampleCount = 1;
    texDesc.Format = kTexFormat;
    texDesc.Memory = MemoryType::Device;
    texDesc.Usage = TextureUse::Resource | TextureUse::CopyDestination;
    auto texOpt = _ctx.CreateTexture(texDesc, &reason);
    ASSERT_TRUE(texOpt.HasValue()) << reason;
    auto texture = texOpt.Release();

    TextureViewDescriptor srvDesc{};
    srvDesc.Target = texture.get();
    srvDesc.Dim = TextureDimension::Dim2D;
    srvDesc.Format = kTexFormat;
    srvDesc.Range = SubresourceRange::AllSub();
    srvDesc.Usage = TextureViewUsage::Resource;
    auto srvOpt = _ctx.CreateTextureView(srvDesc, &reason);
    ASSERT_TRUE(srvOpt.HasValue()) << reason;
    auto srv = srvOpt.Release();

    {
        TextureAsset asset{_ctx.GetDevicePtr(), "test_tex", std::move(texture), std::move(srv)};
        ASSERT_TRUE(asset.IsValid());

        // 默认描述 -> 直接返回 _srv, 不进缓存。
        TextureView* def0 = asset.GetOrCreateSrv(TextureSubViewDesc::Default());
        TextureView* def1 = asset.GetOrCreateSrv(TextureSubViewDesc{});
        EXPECT_EQ(def0, asset.GetSrv());
        EXPECT_EQ(def1, asset.GetSrv());

        // 非默认子 view (只取第 1 层 mip)。
        TextureSubViewDesc subMip1{};
        subMip1.Dim = TextureDimension::Dim2D;
        subMip1.Format = kTexFormat;
        subMip1.Range = SubresourceRange{0, 1, 1, 1};

        TextureView* v0 = asset.GetOrCreateSrv(subMip1);
        ASSERT_NE(v0, nullptr);
        EXPECT_NE(v0, asset.GetSrv());  // 非默认, 是新建的子 view。

        // 同描述再取 -> 命中缓存, 同指针 (去重)。
        TextureView* v1 = asset.GetOrCreateSrv(subMip1);
        EXPECT_EQ(v0, v1);

        // 不同描述 (取第 0 层 mip) -> 不同指针。
        TextureSubViewDesc subMip0{};
        subMip0.Dim = TextureDimension::Dim2D;
        subMip0.Format = kTexFormat;
        subMip0.Range = SubresourceRange{0, 1, 0, 1};
        TextureView* v2 = asset.GetOrCreateSrv(subMip0);
        ASSERT_NE(v2, nullptr);
        EXPECT_NE(v2, v0);
    }
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    TextureViewCacheTest,
    ::testing::ValuesIn(GetEnabledTestBackends()),
    [](const ::testing::TestParamInfo<TestBackend>& info) {
        return string{format_as(info.param)};
    });

}  // namespace
}  // namespace radray::render::test
