#include <radray/gpu/types.h>
#include <radray/gpu/external_resource.h>

#include <gtest/gtest.h>

#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#endif

namespace radray::gpu {
TEST(GpuContract, EmptyExternalHandleReportsDuplicationError) {
    OwnedExternalHandle handle;
    EXPECT_FALSE(handle.IsValid());
    auto duplicate = handle.Duplicate();
    const auto* error = std::get_if<ExternalError>(&duplicate);
    ASSERT_NE(error, nullptr);
#if defined(RADRAY_PLATFORM_WINDOWS)
    EXPECT_EQ(error->NativeApi, ExternalNativeApi::Win32);
    EXPECT_EQ(error->NativeCode, ERROR_INVALID_HANDLE);
#endif
}

TEST(GpuContract, FrameValueOperations) {
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
