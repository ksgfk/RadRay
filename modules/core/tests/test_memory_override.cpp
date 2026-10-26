#include <gtest/gtest.h>
#include <mimalloc.h>
#include <radray/memory.h>
#include <radray/types.h>

#include <cstdlib>
#include <cstring>
#include <malloc.h>

TEST(MimallocOverride, CrtAllocationAndReallocationPreserveData) {
    radray::unique_ptr<void, decltype(&std::free)> block{std::calloc(8, 32), &std::free};
    ASSERT_NE(block, nullptr);
    EXPECT_TRUE(mi_any_heap_contains(block.get()));
    const auto* bytes = static_cast<const unsigned char*>(block.get());
    for (size_t i = 0; i < 256; ++i) EXPECT_EQ(bytes[i], 0);
    std::memset(block.get(), 0x5a, 256);

    auto* original = block.release();
    auto* resized = std::realloc(original, 512);
    block.reset(resized != nullptr ? resized : original);
    ASSERT_NE(resized, nullptr);
    EXPECT_TRUE(mi_any_heap_contains(block.get()));
    bytes = static_cast<const unsigned char*>(block.get());
    for (size_t i = 0; i < 256; ++i) EXPECT_EQ(bytes[i], 0x5a);

    radray::unique_ptr<void, decltype(&std::free)> plain{std::malloc(128), &std::free};
    ASSERT_NE(plain, nullptr);
    EXPECT_TRUE(mi_any_heap_contains(plain.get()));
}

TEST(MimallocOverride, RadRayAndCrtAlignedAllocationsUseMimalloc) {
    radray::unique_ptr<void, decltype(&radray::Free)> plain{radray::Malloc(128), &radray::Free};
    ASSERT_NE(plain, nullptr);
    EXPECT_TRUE(mi_any_heap_contains(plain.get()));

    radray::unique_ptr<void, decltype(&radray::AlignedFree)> aligned{radray::AlignedAlloc(64, 128), &radray::AlignedFree};
    ASSERT_NE(aligned, nullptr);
    EXPECT_TRUE(mi_any_heap_contains(aligned.get()));
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned.get()) % 64, 0u);

    radray::unique_ptr<void, decltype(&_aligned_free)> crtAligned{_aligned_malloc(128, 64), &_aligned_free};
    ASSERT_NE(crtAligned, nullptr);
    EXPECT_TRUE(mi_any_heap_contains(crtAligned.get()));
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(crtAligned.get()) % 64, 0u);
}

TEST(MimallocOverride, CppObjectsContainersAndAlignedNewUseMimalloc) {
    auto object = radray::make_unique<int>(42);
    EXPECT_TRUE(mi_any_heap_contains(object.get()));
    EXPECT_EQ(*object, 42);

    auto array = radray::make_unique<int[]>(128);
    EXPECT_TRUE(mi_any_heap_contains(array.get()));
    radray::vector<int> values(128, 7);
    EXPECT_TRUE(mi_any_heap_contains(values.data()));
    EXPECT_EQ(values.back(), 7);

    struct alignas(64) AlignedValue {
        radray::array<radray::byte, 64> Bytes{};
    };
    auto aligned = radray::make_unique<AlignedValue>();
    EXPECT_TRUE(mi_any_heap_contains(aligned.get()));
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned.get()) % 64, 0u);
}
