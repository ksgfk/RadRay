#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <gtest/gtest.h>

// This standalone executable links the static runtime and replaces calling-thread C++ new,
// including array/aligned forms. It excludes other threads, DLL-private allocators, direct
// malloc and residency. Never combine this source with another allocation-probe translation unit.
namespace graph_allocations {
thread_local bool Enabled{false};
thread_local uint64_t Count{0}, Bytes{0};
void CountAllocation(size_t size) noexcept {
    if (Enabled) {
        ++Count;
        Bytes += size;
    }
}
void* Allocate(size_t size) {
    CountAllocation(size);
    if (auto* result = std::malloc(std::max(size, size_t{1}))) return result;
    std::abort();
}
void* AllocateAligned(size_t size, size_t alignment) {
    CountAllocation(size);
#ifdef _WIN32
    auto* result = _aligned_malloc(std::max(size, size_t{1}), alignment);
#else
    auto* result = std::aligned_alloc(alignment, ((std::max(size, size_t{1}) + alignment - 1) / alignment) * alignment);
#endif
    if (!result) std::abort();
    return result;
}
void FreeAligned(void* value) noexcept {
#ifdef _WIN32
    _aligned_free(value);
#else
    std::free(value);
#endif
}
struct Probe {
    Probe() {
        Count = Bytes = 0;
        Enabled = true;
    }
    ~Probe() { Enabled = false; }
};
struct Sample {
    uint64_t Count, Bytes;
};
template <class Callback>
Sample Measure(Callback&& callback) {
    {
        Probe probe;
        callback();
    }
    return {Count, Bytes};
}
}  // namespace graph_allocations

void* operator new(size_t size) { return graph_allocations::Allocate(size); }
void* operator new[](size_t size) { return graph_allocations::Allocate(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }
void* operator new(size_t size, std::align_val_t alignment) { return graph_allocations::AllocateAligned(size, size_t(alignment)); }
void* operator new[](size_t size, std::align_val_t alignment) { return graph_allocations::AllocateAligned(size, size_t(alignment)); }
void operator delete(void* value, std::align_val_t) noexcept { graph_allocations::FreeAligned(value); }
void operator delete[](void* value, std::align_val_t) noexcept { graph_allocations::FreeAligned(value); }
void operator delete(void* value, size_t, std::align_val_t) noexcept { graph_allocations::FreeAligned(value); }
void operator delete[](void* value, size_t, std::align_val_t) noexcept { graph_allocations::FreeAligned(value); }

namespace radray {

namespace {
TEST(GraphAllocationProbeTest, CountsOrdinaryAndAlignedNew) {
    const auto sample = graph_allocations::Measure([] {
        void* ordinary = ::operator new(37);
        void* aligned = ::operator new(65, std::align_val_t{64});
        ::operator delete(ordinary);
        ::operator delete(aligned, std::align_val_t{64});
    });
    EXPECT_EQ(sample.Count, 2u);
    EXPECT_EQ(sample.Bytes, 102u);
}

}  // namespace
}  // namespace radray
