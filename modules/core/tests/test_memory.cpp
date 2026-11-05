#include <radray/memory.h>

#include <algorithm>
#include <cstring>

#include <gtest/gtest.h>

#include <radray/types.h>

#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

namespace radray {
namespace {

bool IsFilledWith(const void* data, size_t size, unsigned char value) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    return std::all_of(bytes, bytes + size, [value](unsigned char b) { return b == value; });
}

TEST(MemoryTest, MallocReturnsWritableBlock) {
    const unique_ptr<void, decltype(&Free)> block{Malloc(4096), &Free};
    ASSERT_NE(block, nullptr);

    std::memset(block.get(), 0x5a, 4096);

    EXPECT_TRUE(IsFilledWith(block.get(), 4096, 0x5a));
}

TEST(MemoryTest, AlignedAllocHonoursAlignment) {
    for (size_t alignment : {size_t{8}, size_t{64}, size_t{4096}}) {
        const unique_ptr<void, decltype(&AlignedFree)> block{AlignedAlloc(alignment, 3 * alignment), &AlignedFree};
        ASSERT_NE(block, nullptr);

        std::memset(block.get(), 0xa5, 3 * alignment);

        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(block.get()) % alignment, 0u);
        EXPECT_TRUE(IsFilledWith(block.get(), 3 * alignment, 0xa5));
    }
}

#ifdef RADRAY_ENABLE_MIMALLOC
TEST(MemoryTest, AllocationsComeFromMimallocWhenEnabled) {
    const unique_ptr<void, decltype(&Free)> plain{Malloc(128), &Free};
    const unique_ptr<void, decltype(&AlignedFree)> aligned{AlignedAlloc(256, 256), &AlignedFree};

    EXPECT_TRUE(mi_any_heap_contains(plain.get()));
    EXPECT_TRUE(mi_any_heap_contains(aligned.get()));
}
#endif

TEST(MemoryTest, FreeAcceptsNull) {
    Free(nullptr);
    AlignedFree(nullptr);
}

}  // namespace
}  // namespace radray
