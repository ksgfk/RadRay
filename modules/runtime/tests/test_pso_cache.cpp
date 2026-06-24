#include <gtest/gtest.h>

#include <radray/runtime/gpu_system.h>

using namespace radray;

TEST(PsoCacheHashTest, StencilStateParticipatesInGraphicsKeyHash) {
    PSOCache::GraphicsPsoKey lhs{};
    PSOCache::GraphicsPsoKey rhs{};
    lhs.DepthStencil = render::DepthStencilState::Default();
    rhs.DepthStencil = render::DepthStencilState::Default();
    lhs.DepthStencil->Stencil = render::StencilState::Default();
    rhs.DepthStencil->Stencil = render::StencilState::Default();
    rhs.DepthStencil->Stencil->ReadMask = 0x0F;

    ASSERT_NE(lhs, rhs);

    PSOCache::GraphicsPsoKeyHash hash{};
    EXPECT_NE(hash(lhs), hash(rhs));
}

TEST(PsoCacheHashTest, ShaderIdentityParticipatesInGraphicsKeyHash) {
    PSOCache::GraphicsPsoKey lhs{};
    PSOCache::GraphicsPsoKey rhs{};
    lhs.VS.Name = "material_a";
    rhs.VS.Name = "material_b";
    lhs.VS.EntryPoint = "VSMain";
    rhs.VS.EntryPoint = "VSMain";
    lhs.VS.Stage = render::ShaderStage::Vertex;
    rhs.VS.Stage = render::ShaderStage::Vertex;

    ASSERT_NE(lhs, rhs);

    PSOCache::GraphicsPsoKeyHash hash{};
    EXPECT_NE(hash(lhs), hash(rhs));
}
