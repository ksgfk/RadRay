#include <gtest/gtest.h>

#include <radray/render/rhi.h>

using namespace radray;

// 后端 -> 默认字节码类型的映射。这个映射属 render 层: shader 层的接口一律直接收
// ShaderBlobCategory, 不认识 RenderBackend。
TEST(RhiTypesTest, BackendMapsToBlobCategory) {
    EXPECT_EQ(
        render::GetShaderBlobCategoryForBackend(render::RenderBackend::D3D12),
        render::ShaderBlobCategory::DXIL);
    EXPECT_EQ(
        render::GetShaderBlobCategoryForBackend(render::RenderBackend::Vulkan),
        render::ShaderBlobCategory::SPIRV);
    EXPECT_EQ(
        render::GetShaderBlobCategoryForBackend(render::RenderBackend::Metal),
        render::ShaderBlobCategory::MSL);
}

// MAX_COUNT 是哨兵, 不应把成员名泄漏到日志里。
TEST(RhiTypesTest, BackendFormatHidesSentinel) {
    EXPECT_EQ(format_as(render::RenderBackend::D3D12), "D3D12");
    EXPECT_EQ(format_as(render::RenderBackend::MAX_COUNT), "UNKNOWN");
}
