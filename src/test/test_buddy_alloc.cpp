#include <radray/logger.h>
#include <radray/allocator.h>

using namespace radray;
using namespace std;

void a() {
    {
        BuddyAllocator buddy{2};
        auto d = buddy.Allocate(114514);
        RADRAY_ASSERT(!d.has_value());

        auto a = buddy.Allocate(1);
        RADRAY_ASSERT(a.has_value());
        RADRAY_ASSERT(a.value() == 0);

        auto b = buddy.Allocate(1);
        RADRAY_ASSERT(b.has_value());
        RADRAY_ASSERT(b.value() == 1);

        auto c = buddy.Allocate(1);
        RADRAY_ASSERT(!c.has_value());
    }
    {
        BuddyAllocator buddy{2};

        auto a = buddy.Allocate(2);
        RADRAY_ASSERT(a.has_value());
        RADRAY_ASSERT(a.value() == 0);

        auto c = buddy.Allocate(1);
        RADRAY_ASSERT(!c.has_value());
    }
    {
        BuddyAllocator buddy{8};
        auto a = buddy.Allocate(4);
        RADRAY_ASSERT(a.has_value());
        RADRAY_ASSERT(a.value() == 0);

        auto b = buddy.Allocate(1);
        RADRAY_ASSERT(b.has_value());
        RADRAY_ASSERT(b.value() == 4);

        auto c = buddy.Allocate(2);
        RADRAY_ASSERT(c.has_value());
        RADRAY_ASSERT(c.value() == 6);
    }
    {
        BuddyAllocator buddy{1};
        auto a = buddy.Allocate(1);
        RADRAY_ASSERT(a.has_value());
        RADRAY_ASSERT(a.value() == 0);

        auto c = buddy.Allocate(1);
        RADRAY_ASSERT(!c.has_value());
    }
    {
        BuddyAllocator buddy{3};
        auto a = buddy.Allocate(3);
        RADRAY_ASSERT(a.has_value());
        RADRAY_ASSERT(a.value() == 0);

        auto c = buddy.Allocate(1);
        RADRAY_ASSERT(!c.has_value());
    }
    {
        BuddyAllocator buddy{5};
        auto a = buddy.Allocate(3);
        RADRAY_ASSERT(a.has_value());
        RADRAY_ASSERT(a.value() == 0);

        auto b = buddy.Allocate(1);
        RADRAY_ASSERT(b.has_value());
        RADRAY_ASSERT(b.value() == 4);

        auto c = buddy.Allocate(1);
        RADRAY_ASSERT(!c.has_value());
    }
    {
        BuddyAllocator buddy{31};
        auto a = buddy.Allocate(17);
        RADRAY_ASSERT(a.has_value());
        RADRAY_ASSERT(a.value() == 0);

        auto b = buddy.Allocate(14);
        RADRAY_ASSERT(!b.has_value());
    }
}

void b() {
    {
        BuddyAllocator buddy{8};
        auto a = buddy.Allocate(4);
        RADRAY_ASSERT(a.has_value());
        RADRAY_ASSERT(a.value() == 0);

        buddy.Destroy(a.value());

        auto b = buddy.Allocate(2);
        RADRAY_ASSERT(b.has_value());
        RADRAY_ASSERT(b.value() == 0);

        auto c = buddy.Allocate(2);
        RADRAY_ASSERT(c.has_value());
        RADRAY_ASSERT(c.value() == 2);

        auto d = buddy.Allocate(4);
        RADRAY_ASSERT(d.has_value());
        RADRAY_ASSERT(d.value() == 4);

        buddy.Destroy(c.value());

        auto e = buddy.Allocate(1);
        RADRAY_ASSERT(e.has_value());
        RADRAY_ASSERT(e.value() == 2);

        auto f = buddy.Allocate(1);
        RADRAY_ASSERT(f.has_value());
        RADRAY_ASSERT(f.value() == 3);
    }
}

int main() {
    a();
    b();
    return 0;
}
