#include <gtest/gtest.h>

#include <radray/runtime/gpu_system.h>

using namespace radray;

TEST(PsoCacheHashTest, StencilStateParticipatesInGraphicsKeyHash) {
    PSOCache::GraphicsPsoKey lhs{};
    PSOCache::GraphicsPsoKey rhs{};
    lhs.DepthStencil.Stencil = render::StencilState::Default();
    rhs.DepthStencil.Stencil = render::StencilState::Default();
    rhs.DepthStencil.Stencil->ReadMask = 0x0F;

    ASSERT_NE(lhs, rhs);

    PSOCache::GraphicsPsoKeyHash hash{};
    EXPECT_NE(hash(lhs), hash(rhs));
}
