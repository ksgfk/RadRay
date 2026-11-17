#include <radray/gpu/types.h>

#include <gtest/gtest.h>

namespace radray::gpu {
TEST(GpuContract, FrameValueOperationsLinkWithoutImplementation) {
    SwapChainFrame original;
    SwapChainFrame moved{std::move(original)};
    original = std::move(moved);
    EXPECT_FALSE(original.IsValid());
    EXPECT_EQ(original.GetBackBuffer(), nullptr);
    EXPECT_EQ(original.GetWaitToDraw(), nullptr);
    EXPECT_EQ(original.GetReadyToPresent(), nullptr);
    EXPECT_EQ(original.GetBackBufferIndex(), std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(GetBindingRegisterClass(BindingKind::Sampler), BindingRegisterClass::Sampler);
}
}  // namespace radray::gpu
