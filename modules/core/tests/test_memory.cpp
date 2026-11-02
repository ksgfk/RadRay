#include <radray/memory.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>
#include <radray/types.h>

#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

TEST(MemoryTest, OrdinaryAllocationPreservesDataAndUsesConfiguredAllocator) {
    for (size_t size : {size_t{1}, size_t{128}, size_t{4096}}) {
        radray::unique_ptr<void, decltype(&radray::Free)> block{radray::Malloc(size), &radray::Free};
        ASSERT_NE(block, nullptr);
#ifdef RADRAY_ENABLE_MIMALLOC
        EXPECT_TRUE(mi_any_heap_contains(block.get()));
#endif
        std::memset(block.get(), 0x5a, size);
        const auto* bytes = static_cast<const unsigned char*>(block.get());
        EXPECT_TRUE(std::all_of(bytes, bytes + size, [](auto value) { return value == 0x5a; }));
    }
}

TEST(MemoryTest, AlignedAllocationPreservesDataAndUsesConfiguredAllocator) {
    for (size_t alignment : {size_t{8}, size_t{64}, size_t{256}, size_t{4096}}) {
        const size_t size = 3 * alignment;
        radray::unique_ptr<void, decltype(&radray::AlignedFree)> block{radray::AlignedAlloc(alignment, size), &radray::AlignedFree};
        ASSERT_NE(block, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(block.get()) % alignment, 0u);
#ifdef RADRAY_ENABLE_MIMALLOC
        EXPECT_TRUE(mi_any_heap_contains(block.get()));
#endif
        std::memset(block.get(), 0xa5, size);
        const auto* bytes = static_cast<const unsigned char*>(block.get());
        EXPECT_TRUE(std::all_of(bytes, bytes + size, [](auto value) { return value == 0xa5; }));
    }
}

TEST(MemoryTest, FreeAcceptsNull) {
    radray::Free(nullptr);
    radray::AlignedFree(nullptr);
}
