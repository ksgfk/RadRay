#pragma once

#include <bit>
#include <limits>
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

class FreeListAllocator {
public:
    struct Allocation {
        size_t Start = 0;
        size_t Length = 0;
        uint32_t Node = 0;
        uint32_t Generation = 0;

        static constexpr Allocation Invalid() noexcept { return {}; }
    };

    explicit FreeListAllocator(size_t capacity) noexcept;

    std::optional<Allocation> Allocate(size_t size) noexcept;

    void Destroy(Allocation allocation) noexcept;

private:
    static constexpr uint32_t npos = std::numeric_limits<uint32_t>::max();

    enum class NodeState {
        Free,
        Used
    };

    struct Node {
        size_t Start = 0;
        size_t Length = 0;
        uint32_t Prev = npos;
        uint32_t Next = npos;
        uint32_t FreePos = npos;
        uint32_t Generation = 0;
        NodeState State = NodeState::Free;
    };

    uint32_t NewNode(size_t start, size_t length, NodeState state) noexcept;
    void DeleteNode(uint32_t idx) noexcept;
    void AddFree(uint32_t idx) noexcept;
    void RemoveFree(uint32_t idx) noexcept;

    size_t _capacity;
    vector<Node> _nodes;
    vector<uint32_t> _nodeFreePool;
    vector<uint32_t> _freeNodes;
    uint32_t _head = npos;
};

static_assert(is_allocator<FreeListAllocator, FreeListAllocator::Allocation>, "FreeListAllocator is not an allocator");

}  // namespace radray
