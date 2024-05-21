#include <radray/logger.h>
#include <radray/allocator.h>

using namespace radray;
using namespace std;

void a() {
    {
        BuddyAllocator buddy{2};
        auto d = buddy.Allocate(114514);
        RADRAY_ASSERT_IMPL(!d.has_value(), "err");

        auto a = buddy.Allocate(1);
        RADRAY_ASSERT_IMPL(a.has_value(), "err");
        RADRAY_ASSERT_IMPL(a.value() == 0, "err");

        auto b = buddy.Allocate(1);
        RADRAY_ASSERT_IMPL(b.has_value(), "err");
        RADRAY_ASSERT_IMPL(b.value() == 1, "err");

        auto c = buddy.Allocate(1);
        RADRAY_ASSERT_IMPL(!c.has_value(), "err");
    }
    {
        BuddyAllocator buddy{2};

        auto a = buddy.Allocate(2);
        RADRAY_ASSERT_IMPL(a.has_value(), "err");
        RADRAY_ASSERT_IMPL(a.value() == 0, "err");

        auto c = buddy.Allocate(1);
        RADRAY_ASSERT_IMPL(!c.has_value(), "err");
    }
    {
        BuddyAllocator buddy{8};
        auto a = buddy.Allocate(4);
        RADRAY_ASSERT_IMPL(a.has_value(), "err");
        RADRAY_ASSERT_IMPL(a.value() == 0, "err");

        auto b = buddy.Allocate(1);
        RADRAY_ASSERT_IMPL(b.has_value(), "err");
        RADRAY_ASSERT_IMPL(b.value() == 4, "err");

        auto c = buddy.Allocate(2);
        RADRAY_ASSERT_IMPL(c.has_value(), "err");
        RADRAY_ASSERT_IMPL(c.value() == 6, "err");
    }
    {
        BuddyAllocator buddy{1};
        auto a = buddy.Allocate(1);
        RADRAY_ASSERT_IMPL(a.has_value(), "err");
        RADRAY_ASSERT_IMPL(a.value() == 0, "err");

        auto c = buddy.Allocate(1);
        RADRAY_ASSERT_IMPL(!c.has_value(), "err");
    }
    {
        BuddyAllocator buddy{3};
        auto a = buddy.Allocate(3);
        RADRAY_ASSERT_IMPL(a.has_value(), "err");
        RADRAY_ASSERT_IMPL(a.value() == 0, "err");

        auto c = buddy.Allocate(1);
        RADRAY_ASSERT_IMPL(!c.has_value(), "err");
    }
    {
        BuddyAllocator buddy{5};
        auto a = buddy.Allocate(3);
        RADRAY_ASSERT_IMPL(a.has_value(), "err");
        RADRAY_ASSERT_IMPL(a.value() == 0, "err");

        auto b = buddy.Allocate(1);
        RADRAY_ASSERT_IMPL(b.has_value(), "err");
        RADRAY_ASSERT_IMPL(b.value() == 4, "err");

        auto c = buddy.Allocate(1);
        RADRAY_ASSERT_IMPL(!c.has_value(), "err");
    }
    {
        BuddyAllocator buddy{31};
        auto a = buddy.Allocate(17);
        RADRAY_ASSERT_IMPL(a.has_value(), "err");
        RADRAY_ASSERT_IMPL(a.value() == 0, "err");

        auto b = buddy.Allocate(14);
        RADRAY_ASSERT_IMPL(!b.has_value(), "err");
    }
}

void b() {
    {
        BuddyAllocator buddy{8};
        auto a = buddy.Allocate(4);
        RADRAY_ASSERT_IMPL(a.has_value(), "err");
        RADRAY_ASSERT_IMPL(a.value() == 0, "err");

        buddy.Destroy(a.value());

        auto b = buddy.Allocate(2);
        RADRAY_ASSERT_IMPL(b.has_value(), "err");
        RADRAY_ASSERT_IMPL(b.value() == 0, "err");

        auto c = buddy.Allocate(2);
        RADRAY_ASSERT_IMPL(c.has_value(), "err");
        RADRAY_ASSERT_IMPL(c.value() == 2, "err");

        auto d = buddy.Allocate(4);
        RADRAY_ASSERT_IMPL(d.has_value(), "err");
        RADRAY_ASSERT_IMPL(d.value() == 4, "err");

        buddy.Destroy(c.value());

        auto e = buddy.Allocate(1);
        RADRAY_ASSERT_IMPL(e.has_value(), "err");
        RADRAY_ASSERT_IMPL(e.value() == 2, "err");

        auto f = buddy.Allocate(1);
        RADRAY_ASSERT_IMPL(f.has_value(), "err");
        RADRAY_ASSERT_IMPL(f.value() == 3, "err");
    }
}

int main() {
    a();
    b();
    return 0;
}
