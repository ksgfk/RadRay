#include <radray/logger.h>
#include <radray/allocator.h>

#include "test_helpers.h"

void a() {
    radray::FreeListAllocator alloc{2};
    auto a = alloc.Allocate(1);
    RADRAY_TEST_TRUE(a.has_value());
    RADRAY_TEST_TRUE(a.value() == 0);

    auto b = alloc.Allocate(1);
    RADRAY_TEST_TRUE(b.has_value());
    RADRAY_TEST_TRUE(b.value() == 1);

    auto c = alloc.Allocate(1);
    RADRAY_TEST_TRUE(!c.has_value());
}

void b() {
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
    RADRAY_TEST_TRUE(g.has_value());
    RADRAY_TEST_TRUE(g.value() == 1);
}

int main() {
    a();
    b();
    return 0;
}
