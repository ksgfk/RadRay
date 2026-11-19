#include <radray/gpu/external_resource.h>

#include <gtest/gtest.h>

#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#endif

#if defined(RADRAY_PLATFORM_WINDOWS)
namespace radray::gpu {
TEST(GpuContract, ExternalHandleTransfersOwnershipAndDuplicatesIndependently) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(event, nullptr);
    OwnedExternalHandle original{reinterpret_cast<uintptr_t>(event)};
    auto result = original.Duplicate();
    auto* duplicate = std::get_if<OwnedExternalHandle>(&result);
    ASSERT_NE(duplicate, nullptr);

    OwnedExternalHandle moved{std::move(original)};
    EXPECT_FALSE(original.IsValid());
    EXPECT_EQ(moved.Borrow().Value, reinterpret_cast<uintptr_t>(event));
    OwnedExternalHandle owner;
    owner = std::move(moved);
    EXPECT_FALSE(moved.IsValid());
    owner.Reset();
    EXPECT_FALSE(owner.IsValid());

    DWORD flags{};
    EXPECT_FALSE(GetHandleInformation(event, &flags));
    EXPECT_TRUE(GetHandleInformation(reinterpret_cast<HANDLE>(duplicate->Borrow().Value), &flags));
}
}  // namespace radray::gpu
#endif
