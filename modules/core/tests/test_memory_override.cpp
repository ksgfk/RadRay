#include <cstdlib>
#include <cstring>

#include <malloc.h>
#include <mimalloc.h>

#include <gtest/gtest.h>

#include <radray/types.h>

namespace radray {
namespace {

TEST(MimallocOverrideTest, CrtAllocationsUseMimalloc) {
    const unique_ptr<void, decltype(&std::free)> plain{std::malloc(128), &std::free};
    const unique_ptr<void, decltype(&_aligned_free)> aligned{_aligned_malloc(128, 64), &_aligned_free};

    EXPECT_TRUE(mi_any_heap_contains(plain.get()));
    EXPECT_TRUE(mi_any_heap_contains(aligned.get()));
}

TEST(MimallocOverrideTest, CrtReallocPreservesDataInMimalloc) {
    void* block = std::calloc(8, 32);
    ASSERT_NE(block, nullptr);
    std::memset(block, 0x5a, 256);

    void* resized = std::realloc(block, 512);
    ASSERT_NE(resized, nullptr);
    const unique_ptr<void, decltype(&std::free)> owner{resized, &std::free};

    EXPECT_TRUE(mi_any_heap_contains(resized));
    EXPECT_EQ(static_cast<const unsigned char*>(resized)[255], 0x5a);
}

TEST(MimallocOverrideTest, OperatorNewUsesMimalloc) {
    struct alignas(64) Aligned {
        array<byte, 64> Bytes{};
    };
    const auto object = make_unique<int>(42);
    const auto aligned = make_unique<Aligned>();
    const vector<int> values(128, 7);

    EXPECT_TRUE(mi_any_heap_contains(object.get()));
    EXPECT_TRUE(mi_any_heap_contains(aligned.get()));
    EXPECT_TRUE(mi_any_heap_contains(values.data()));
}

}  // namespace
}  // namespace radray
