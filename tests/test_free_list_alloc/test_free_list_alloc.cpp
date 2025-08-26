#include <gtest/gtest.h>

#include <radray/logger.h>
#include <radray/allocator.h>

TEST(Core_Allocator_FreeList, Simple) {
    radray::FreeListAllocator alloc{2};
    auto a = alloc.Allocate(1);
    ASSERT_TRUE(a.has_value());
    ASSERT_EQ(a.value(), 0);

    auto b = alloc.Allocate(1);
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(b.value(), 1);

    auto c = alloc.Allocate(1);
    ASSERT_FALSE(c.has_value());
}

TEST(Core_Allocator_FreeList, Simple2) {
    radray::FreeListAllocator alloc{16};
    alloc.Allocate(1);
    auto b = alloc.Allocate(1).value();
    auto c = alloc.Allocate(1).value();
    auto d = alloc.Allocate(1).value();
    auto e = alloc.Allocate(1).value();
    alloc.Allocate(1);

    alloc.Destroy(b);
    alloc.Destroy(c);
    alloc.Destroy(d);
    alloc.Destroy(e);

    auto g = alloc.Allocate(4);
    ASSERT_TRUE(g.has_value());
    ASSERT_EQ(g.value(), 1);
}

TEST(Core_Allocator_FreeList, AllocateAndDestroy) {
    radray::FreeListAllocator alloc{8};

    auto a = alloc.Allocate(2);
    ASSERT_TRUE(a.has_value());
    ASSERT_EQ(a.value(), 0);

    auto b = alloc.Allocate(3);
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(b.value(), 2);

    alloc.Destroy(a.value());
    auto c = alloc.Allocate(1);
    ASSERT_TRUE(c.has_value());
    ASSERT_EQ(c.value(), 0);

    alloc.Destroy(b.value());
    auto d = alloc.Allocate(5);
    ASSERT_TRUE(d.has_value());
    ASSERT_EQ(d.value(), 1);
}

TEST(Core_Allocator_FreeList, OverAllocate) {
    radray::FreeListAllocator alloc{4};

    auto a = alloc.Allocate(2);
    ASSERT_TRUE(a.has_value());
    ASSERT_EQ(a.value(), 0);

    auto b = alloc.Allocate(3);
    ASSERT_FALSE(b.has_value());

    alloc.Destroy(a.value());
    auto c = alloc.Allocate(4);
    ASSERT_TRUE(c.has_value());
    ASSERT_EQ(c.value(), 0);
}

TEST(Core_Allocator_FreeList, Fragmentation) {
    radray::FreeListAllocator alloc{10};

    // xxx ooooooo
    auto a = alloc.Allocate(3);
    ASSERT_TRUE(a.has_value());
    ASSERT_EQ(a.value(), 0);

    // xxx xx ooooo
    auto b = alloc.Allocate(2);
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(b.value(), 3);

    // xxx xx xxxx o
    auto c = alloc.Allocate(4);
    ASSERT_TRUE(c.has_value());
    ASSERT_EQ(c.value(), 5);

    // xxx oo xxxx o
    alloc.Destroy(b.value());

    // xxx xx xxxx o
    auto d = alloc.Allocate(2);
    ASSERT_TRUE(d.has_value());
    ASSERT_EQ(d.value(), 3);

    // xxx xx xxxx x
    auto e = alloc.Allocate(1);
    ASSERT_TRUE(d.has_value());
    ASSERT_EQ(e.value(), 9);
}

TEST(Core_Allocator_FreeList, EdgeCases) {
    radray::FreeListAllocator alloc{1};

    auto a = alloc.Allocate(1);
    ASSERT_TRUE(a.has_value());
    ASSERT_EQ(a.value(), 0);

    auto b = alloc.Allocate(1);
    ASSERT_FALSE(b.has_value());

    alloc.Destroy(a.value());
    auto c = alloc.Allocate(1);
    ASSERT_TRUE(c.has_value());
    ASSERT_EQ(c.value(), 0);
}
