#include <radray/logger.h>
#include <radray/allocator.h>

#include "test_helpers.h"

using namespace radray;
using namespace std;

class HeapInternal {
public:
    HeapInternal(int* counter, size_t size) noexcept : Size(size), Counter(counter) {
        RADRAY_INFO_LOG("HeapInternal created with size: {}", size);
        (*Counter)++;
    }

    ~HeapInternal() noexcept {
        RADRAY_INFO_LOG("HeapInternal destroyed with size: {}", Size);
        (*Counter)--;
    }

    HeapInternal(const HeapInternal&) = delete;
    HeapInternal& operator=(const HeapInternal&) = delete;

    size_t Size;
    int* Counter;
};

class Heap {
public:
    Heap(int* counter, size_t size) noexcept : Size(size), Internal{make_unique<HeapInternal>(counter, size)} {}

    size_t Size;
    radray::unique_ptr<HeapInternal> Internal;
};

class TestAllocator : public BlockAllocator<BuddyAllocator, Heap, TestAllocator> {
public:
    TestAllocator(
        int* counter,
        size_t basicSize,
        size_t destroyThreshold) noexcept
        : BlockAllocator(basicSize, destroyThreshold),
          Counter(counter) {}

    ~TestAllocator() noexcept override = default;

    radray::unique_ptr<Heap> CreateHeap(size_t size) noexcept {
        return radray::make_unique<Heap>(Counter, size);
    }

    BuddyAllocator CreateSubAllocator(size_t size) noexcept { return BuddyAllocator{size}; }

    int* Counter;
};

void a() {
    radray::unique_ptr<int> counter = radray::make_unique<int>(0);

    TestAllocator alloc{counter.get(), 2, 0};
    auto a = alloc.Allocate(1);
    RADRAY_TEST_TRUE(a.has_value());
    RADRAY_TEST_TRUE(a.value().Length == 1);
    RADRAY_TEST_TRUE(a.value().Start == 0);

    RADRAY_TEST_TRUE(*counter == 1);

    auto b = alloc.Allocate(1);
    RADRAY_TEST_TRUE(b.has_value());
    RADRAY_TEST_TRUE(b.value().Length == 1);
    RADRAY_TEST_TRUE(b.value().Start == 1);

    RADRAY_TEST_TRUE(a.value().Heap == b.value().Heap);

    RADRAY_TEST_TRUE(*counter == 1);

    auto c = alloc.Allocate(1);
    RADRAY_TEST_TRUE(c.has_value());
    RADRAY_TEST_TRUE(c.value().Length == 1);
    RADRAY_TEST_TRUE(c.value().Start == 0);

    RADRAY_TEST_TRUE(a.value().Heap != c.value().Heap);

    RADRAY_TEST_TRUE(*counter == 2);

    auto d = alloc.Allocate(2);
    RADRAY_TEST_TRUE(d.has_value());
    RADRAY_TEST_TRUE(d.value().Length == 2);
    RADRAY_TEST_TRUE(d.value().Start == 0);

    RADRAY_TEST_TRUE(*counter == 3);

    alloc.Destroy(c.value());

    RADRAY_TEST_TRUE(*counter == 2);

    alloc.Destroy(d.value());

    RADRAY_TEST_TRUE(*counter == 1);
}

int main() {
    a();
    return 0;
}
