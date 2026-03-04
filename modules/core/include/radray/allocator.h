#pragma once

#include <bit>
#include <optional>
#include <type_traits>

#include <radray/types.h>

namespace radray {

template <class TAllocator, class TAllocation>
concept is_allocator = requires(TAllocator alloc, size_t size, TAllocation allocation) {
    { alloc.Allocate(size) } -> std::same_as<std::optional<TAllocation>>;
    { alloc.Destroy(allocation) } -> std::same_as<void>;
};

class BuddyAllocator {
public:
    struct Allocation {
        size_t Offset = 0;
        size_t NodeIndex = 0;

        static constexpr Allocation Invalid() noexcept { return {}; }
    };

    explicit BuddyAllocator(size_t capacity) noexcept;

    ~BuddyAllocator() noexcept = default;

    std::optional<Allocation> Allocate(size_t size) noexcept;

    void Destroy(Allocation allocation) noexcept;

private:
    struct NodeInfo {
        size_t Longest = 0;
    };

    void UpdateAncestors(size_t index) noexcept;

    size_t _capacity;
    size_t _virtualCapacity;
    vector<NodeInfo> _nodes;
};

static_assert(is_allocator<BuddyAllocator, BuddyAllocator::Allocation>, "BuddyAllocator is not an allocator");

class FirstFitAllocator {
public:
    struct Allocation {
        size_t Start = 0;
        size_t Length = 0;

        static constexpr Allocation Invalid() noexcept { return {}; }
    };

    explicit FirstFitAllocator(size_t capacity) noexcept;

    std::optional<Allocation> Allocate(size_t size) noexcept;

    void Destroy(Allocation allocation) noexcept;

private:
    struct FreeRange {
        size_t Start = 0;
        size_t Length = 0;
    };

    size_t _capacity = 0;
    vector<FreeRange> _freeRanges;
};

static_assert(is_allocator<FirstFitAllocator, FirstFitAllocator::Allocation>, "FirstFitAllocator is not an allocator");

}  // namespace radray
