#include <cstdlib>
#include <radray/logger.h>
#include <radray/allocator.h>

using namespace radray;
using namespace std;

#define RADRAY_TEST_TRUE(value) \
    do {                        \
        if (!(value)) {         \
            std::exit(-114514); \
        }                       \
    } while (0);

void a() {
    {
        BuddyAllocator buddy{2};
        auto d = buddy.Allocate(114514);
        RADRAY_TEST_TRUE(!d.has_value());

        auto a = buddy.Allocate(1);
        RADRAY_TEST_TRUE(a.has_value());
        RADRAY_TEST_TRUE(a.value() == 0);

        auto b = buddy.Allocate(1);
        RADRAY_TEST_TRUE(b.has_value());
        RADRAY_TEST_TRUE(b.value() == 1);

        auto c = buddy.Allocate(1);
        RADRAY_TEST_TRUE(!c.has_value());
    }
    {
        BuddyAllocator buddy{2};

        auto a = buddy.Allocate(2);
        RADRAY_TEST_TRUE(a.has_value());
        RADRAY_TEST_TRUE(a.value() == 0);

        auto c = buddy.Allocate(1);
        RADRAY_TEST_TRUE(!c.has_value());
    }
    {
        BuddyAllocator buddy{8};
        auto a = buddy.Allocate(4);
        RADRAY_TEST_TRUE(a.has_value());
        RADRAY_TEST_TRUE(a.value() == 0);

        auto b = buddy.Allocate(1);
        RADRAY_TEST_TRUE(b.has_value());
        RADRAY_TEST_TRUE(b.value() == 4);

        auto c = buddy.Allocate(2);
        RADRAY_TEST_TRUE(c.has_value());
        RADRAY_TEST_TRUE(c.value() == 6);
    }
    {
        BuddyAllocator buddy{1};
        auto a = buddy.Allocate(1);
        RADRAY_TEST_TRUE(a.has_value());
        RADRAY_TEST_TRUE(a.value() == 0);

        auto c = buddy.Allocate(1);
        RADRAY_TEST_TRUE(!c.has_value());
    }
    {
        BuddyAllocator buddy{3};
        auto a = buddy.Allocate(3);
        RADRAY_TEST_TRUE(a.has_value());
        RADRAY_TEST_TRUE(a.value() == 0);

        auto c = buddy.Allocate(1);
        RADRAY_TEST_TRUE(!c.has_value());
    }
    {
        BuddyAllocator buddy{5};
        auto a = buddy.Allocate(3);
        RADRAY_TEST_TRUE(a.has_value());
        RADRAY_TEST_TRUE(a.value() == 0);

        auto b = buddy.Allocate(1);
        RADRAY_TEST_TRUE(b.has_value());
        RADRAY_TEST_TRUE(b.value() == 4);

        auto c = buddy.Allocate(1);
        RADRAY_TEST_TRUE(!c.has_value());
    }
    {
        BuddyAllocator buddy{31};
        auto a = buddy.Allocate(17);
        RADRAY_TEST_TRUE(a.has_value());
        RADRAY_TEST_TRUE(a.value() == 0);

        auto b = buddy.Allocate(14);
        RADRAY_TEST_TRUE(!b.has_value());
    }
}

void b() {
    {
        BuddyAllocator buddy{8};
        auto a = buddy.Allocate(4);
        RADRAY_TEST_TRUE(a.has_value());
        RADRAY_TEST_TRUE(a.value() == 0);

        buddy.Destroy(a.value());

        auto b = buddy.Allocate(2);
        RADRAY_TEST_TRUE(b.has_value());
        RADRAY_TEST_TRUE(b.value() == 0);

        auto c = buddy.Allocate(2);
        RADRAY_TEST_TRUE(c.has_value());
        RADRAY_TEST_TRUE(c.value() == 2);

        auto d = buddy.Allocate(4);
        RADRAY_TEST_TRUE(d.has_value());
        RADRAY_TEST_TRUE(d.value() == 4);

        buddy.Destroy(c.value());

        auto e = buddy.Allocate(1);
        RADRAY_TEST_TRUE(e.has_value());
        RADRAY_TEST_TRUE(e.value() == 2);

        auto f = buddy.Allocate(1);
        RADRAY_TEST_TRUE(f.has_value());
        RADRAY_TEST_TRUE(f.value() == 3);
    }
}

int main() {
    a();
    b();
    return 0;
}
