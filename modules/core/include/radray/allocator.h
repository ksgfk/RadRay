#pragma once

#include <bit>
#include <limits>
#include <list>
#include <optional>
#include <type_traits>

#include <radray/enum_flags.h>
#include <radray/types.h>
#include <radray/utility.h>
#include <radray/logger.h>

namespace radray {

template <class TAllocator, class TAllocation>
concept is_allocator = requires(TAllocator alloc, size_t size, TAllocation allocation) {
    { alloc.Allocate(size) } -> std::same_as<std::optional<TAllocation>>;
    { alloc.Destroy(allocation) } -> std::same_as<void>;
};

template <class TAllocator>
concept is_resetable_allocator = requires(TAllocator alloc) {
    { alloc.Reset() } -> std::same_as<void>;
};

class BuddyAllocator {
public:
    using Allocation = size_t;

    explicit BuddyAllocator(size_t capacity) noexcept;

    ~BuddyAllocator() noexcept = default;

    std::optional<size_t> Allocate(size_t size) noexcept;

    void Destroy(size_t offset) noexcept;

private:
    size_t _capacity;
    size_t _virtualCapacity;
    vector<size_t> _longest;
    vector<size_t> _nodeSize;
    vector<size_t> _nodeOffset;
    vector<size_t> _actualCapacity;
    vector<uint8_t> _allocated;

    void UpdateAncestors(size_t index) noexcept;
};

static_assert(is_allocator<BuddyAllocator, size_t>, "BuddyAllocator is not an allocator");

struct BlockAllocatorOwnerBase {};

struct BlockAllocatorPageBase {};

template <class THeap, class TSubAllocation>
struct BlockAllocation {
    THeap* Heap;
    BlockAllocatorPageBase* OwnerPage;
    BlockAllocatorOwnerBase* OwnerAllocator;
    size_t Start;
    size_t Length;
    TSubAllocation SubAllocation;
};

template <class TSubAlloc, class THeap, class TDerived>
requires is_allocator<TSubAlloc, typename TSubAlloc::Allocation>
class BlockAllocator : public BlockAllocatorOwnerBase {
public:
    using SubAllocation = typename TSubAlloc::Allocation;
    using Allocation = BlockAllocation<THeap, SubAllocation>;

    explicit BlockAllocator(size_t basicPageSize) noexcept
        : _basicPageSize(basicPageSize) {}

    virtual ~BlockAllocator() noexcept = default;

    std::optional<Allocation> Allocate(size_t size) noexcept {
        if (size == 0) {
            return std::nullopt;
        }
        const size_t needBucket = BucketForNeed(size);
        EnsureBucketCount(needBucket + 1);
        for (size_t bucket = needBucket; bucket < _bucketHeads.size(); ++bucket) {
            auto& bucketList = _bucketHeads[bucket];
            for (auto it = bucketList.begin(); it != bucketList.end();) {
                Page* page = *it;
                ++it;
                if (page->_freeBytes >= size) {
                    std::optional<SubAllocation> subAlloc = page->_allocator.Allocate(size);
                    if (subAlloc.has_value()) {
                        page->_freeBytes -= size;
                        page->_liveAllocs += 1;
                        UpdatePageBucket(page);
                        const size_t start = StartOf(subAlloc.value());
                        return Allocation{page->_heap.get(), page, this, start, size, subAlloc.value()};
                    }
                }
            }
        }
        Page* page = CreatePage(size);
        std::optional<SubAllocation> subAlloc = page->_allocator.Allocate(size);
        RADRAY_ASSERT(subAlloc.has_value());
        page->_freeBytes -= size;
        page->_liveAllocs += 1;
        UpdatePageBucket(page);
        const size_t start = StartOf(subAlloc.value());
        return Allocation{page->_heap.get(), page, this, start, size, subAlloc.value()};
    }

    void Destroy(Allocation allocation) noexcept {
        RADRAY_ASSERT(allocation.Heap != nullptr);
        RADRAY_ASSERT(allocation.OwnerAllocator == static_cast<BlockAllocatorOwnerBase*>(this));
        RADRAY_ASSERT(allocation.OwnerPage != nullptr);
        Page* page = static_cast<Page*>(allocation.OwnerPage);
        RADRAY_ASSERT(page->_heap.get() == allocation.Heap);
        page->_allocator.Destroy(allocation.SubAllocation);
        page->_freeBytes += allocation.Length;
        RADRAY_ASSERT(page->_liveAllocs > 0);
        page->_liveAllocs -= 1;
        if (page->_liveAllocs == 0) {
            ReleasePage(page);
            return;
        }
        UpdatePageBucket(page);
    }

    void Reset() noexcept {
        _bucketHeads.clear();
        for (auto& up : _pages) {
            Page* page = up.get();
            RADRAY_ASSERT(page);
            if constexpr (is_resetable_allocator<TSubAlloc>) {
                page->_allocator.Reset();
            } else {
                page->_allocator = static_cast<TDerived*>(this)->CreateSubAllocator(page->_capacity);
            }
            page->_freeBytes = page->_capacity;
            page->_liveAllocs = 0;

            const size_t bucket = BucketForFree(page->_freeBytes);
            EnsureBucketCount(bucket + 1);
            auto& dst = _bucketHeads[bucket];
            dst.push_front(page);
            page->_bucketIter = dst.begin();
            page->_bucketIndex = bucket;
        }
    }

private:
    static constexpr size_t StartOf(const SubAllocation& alloc) noexcept {
        if constexpr (std::is_same_v<SubAllocation, size_t>) {
            return alloc;
        } else {
            return alloc.Start;
        }
    }

    static constexpr size_t npos = static_cast<size_t>(-1);

    struct Page : public BlockAllocatorPageBase {
        explicit Page(
            unique_ptr<THeap> heap,
            TSubAlloc&& allocator,
            size_t capacity) noexcept
            : _freeBytes(capacity),
              _capacity(capacity),
              _heap(std::move(heap)),
              _allocator(std::move(std::forward<TSubAlloc>(allocator))) {}

        size_t _freeBytes = 0;
        size_t _capacity = 0;
        size_t _liveAllocs = 0;

        size_t _ownerIndex = 0;
        size_t _bucketIndex = npos;
        list<Page*>::iterator _bucketIter{};

        unique_ptr<THeap> _heap;
        TSubAlloc _allocator;
    };

    static constexpr size_t BucketForFree(size_t freeBytes) noexcept {
        if (freeBytes == 0) {
            return 0;
        }
        return std::bit_width(freeBytes) - 1;
    }

    static constexpr size_t BucketForNeed(size_t needBytes) noexcept {
        if (needBytes <= 1) {
            return 0;
        }
        return std::bit_width(needBytes - 1);
    }

    void EnsureBucketCount(size_t count) noexcept {
        if (_bucketHeads.size() >= count) {
            return;
        }
        _bucketHeads.resize(count);
    }

    void RemoveFromBucket(Page* page) noexcept {
        if (page->_bucketIndex == npos) {
            return;
        }
        _bucketHeads[page->_bucketIndex].erase(page->_bucketIter);
        page->_bucketIndex = npos;
    }

    void MoveToBucketFront(Page* page, size_t bucket) noexcept {
        EnsureBucketCount(bucket + 1);
        auto& dst = _bucketHeads[bucket];
        if (page->_bucketIndex == npos) {
            dst.push_front(page);
            page->_bucketIter = dst.begin();
            page->_bucketIndex = bucket;
            return;
        }
        auto& src = _bucketHeads[page->_bucketIndex];
        dst.splice(dst.begin(), src, page->_bucketIter);
        page->_bucketIndex = bucket;
        page->_bucketIter = dst.begin();
    }

    void UpdatePageBucket(Page* page) noexcept {
        if (page->_freeBytes == 0) {
            RemoveFromBucket(page);
            return;
        }
        const size_t newBucket = BucketForFree(page->_freeBytes);
        if (page->_bucketIndex == newBucket) {
            return;
        }
        MoveToBucketFront(page, newBucket);
    }

    Page* CreatePage(size_t requestSize) noexcept {
        const size_t capacity = std::max(requestSize, _basicPageSize);
        unique_ptr<Page> page = make_unique<Page>(
            static_cast<TDerived*>(this)->CreateHeap(capacity),
            static_cast<TDerived*>(this)->CreateSubAllocator(capacity),
            capacity);
        Page* pagePtr = page.get();
        pagePtr->_ownerIndex = _pages.size();
        _pages.emplace_back(std::move(page));
        UpdatePageBucket(pagePtr);
        return pagePtr;
    }

    void ReleasePage(Page* page) noexcept {
        RemoveFromBucket(page);
        const size_t idx = page->_ownerIndex;
        RADRAY_ASSERT(idx < _pages.size());
        if (idx != _pages.size() - 1) {
            std::swap(_pages[idx], _pages.back());
            _pages[idx]->_ownerIndex = idx;
        }
        _pages.pop_back();
    }

    vector<unique_ptr<Page>> _pages;
    vector<list<Page*>> _bucketHeads;
    size_t _basicPageSize;
};

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
    enum class NodeState {
        Free,
        Used
    };

    static constexpr uint32_t npos = std::numeric_limits<uint32_t>::max();

    struct Node {
        size_t start = 0;
        size_t length = 0;
        uint32_t prev = npos;
        uint32_t next = npos;
        uint32_t freePos = npos;
        uint32_t generation = 0;
        NodeState state = NodeState::Free;
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

class StackAllocator {
public:
    using Allocation = size_t;

    explicit StackAllocator(size_t capacity) noexcept;

    std::optional<size_t> Allocate(size_t size) noexcept;

    void Destroy(size_t) noexcept {}

    void Reset() noexcept { _offset = 0; }

private:
    size_t _capacity = 0;
    size_t _offset = 0;
};

static_assert(is_allocator<StackAllocator, size_t>, "StackAllocator is not an allocator");

}  // namespace radray
