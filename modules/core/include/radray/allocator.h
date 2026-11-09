#pragma once

#include <optional>

#include <radray/types.h>

namespace radray {

template <class TAllocator, class TAllocation, class TRequest = size_t>
concept is_allocator = requires(TAllocator alloc, TRequest request, TAllocation allocation) {
    { alloc.Allocate(request) } -> std::same_as<std::optional<TAllocation>>;
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

// Fixed-capacity integer ranges. See docs/architecture/core-facilities.md.
class BestFitAllocator final {
public:
    struct Allocation {
        uint32_t Offset{0};
        uint32_t Count{0};

        static constexpr Allocation Invalid() noexcept { return {}; }
        friend bool operator==(Allocation, Allocation) = default;
    };

    explicit BestFitAllocator(uint32_t capacity);
    BestFitAllocator(const BestFitAllocator&) = delete;
    BestFitAllocator& operator=(const BestFitAllocator&) = delete;

    // Zero count or insufficient contiguous space returns null.
    std::optional<Allocation> Allocate(uint32_t count);
    // Return an exact live allocation once, after all users have finished.
    void Destroy(Allocation allocation) noexcept;
    uint32_t GetCapacity() const noexcept;
    uint32_t GetFreeCount() const noexcept;

private:
    void InsertFree(uint32_t offset, uint32_t count);
    void EraseFree(map<uint32_t, uint32_t>::iterator range) noexcept;

    set<std::pair<uint32_t, uint32_t>> _freeByCount;
    map<uint32_t, uint32_t> _freeByOffset;
    uint32_t _capacity;
    uint32_t _freeCount;
};

static_assert(is_allocator<BestFitAllocator, BestFitAllocator::Allocation, uint32_t>, "BestFitAllocator is not an allocator");

}  // namespace radray
