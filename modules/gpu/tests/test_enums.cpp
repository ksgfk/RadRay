#include <radray/gpu/types.h>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace radray::gpu {
TEST(GpuContract, BindingQueriesAndFormatting) {
    EXPECT_EQ(GetBindingRegisterClass(BindingKind::RWStructuredBuffer), BindingRegisterClass::UAV);
    EXPECT_TRUE(CanBeRootDescriptor(BindingKind::CBuffer));
    EXPECT_FALSE(CanBeRootDescriptor(BindingKind::Texture));
    EXPECT_EQ(fmt::format("{}", VertexFormat::FLOAT32X3), "float3");
    EXPECT_EQ(GetTextureFormatBytesPerPixel(TextureFormat::RGBA8_UNORM), 4u);
}
}  // namespace radray::gpu
