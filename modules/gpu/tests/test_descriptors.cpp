#include <radray/gpu/types.h>

#include <gtest/gtest.h>

namespace radray::gpu {
TEST(GpuContract, DescriptorsResolveRanges) {
    TextureDescriptor texture{};
    texture.Dim = TextureDimension::Dim2DArray;
    texture.Format = TextureFormat::RGBA8_UNORM;
    texture.DepthOrArraySize = 3;
    texture.MipLevels = 4;
    const auto range = NormalizeSubresourceRange(texture, SubresourceRange::AllSub());
    ASSERT_TRUE(range);
    EXPECT_EQ(range->ArrayLayerCount, 3u);
    EXPECT_EQ(range->MipLevelCount, 4u);
    EXPECT_EQ(GetBindingRegisterClass(BindingKind::CBuffer), BindingRegisterClass::CBV);
    auto invalid = *range;
    invalid.BaseMipLevel = 4;
    EXPECT_FALSE(NormalizeSubresourceRange(texture, invalid));
}
}  // namespace radray::gpu
