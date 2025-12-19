#pragma once

#include <bit>
#include <limits>
#include <optional>
#include <type_traits>
#include <unordered_map>

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

class BuddyAllocator {
public:
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
    static constexpr size_t LeftChild(size_t index) noexcept { return index * 2 + 1; }
    static constexpr size_t RightChild(size_t index) noexcept { return index * 2 + 2; }
    static constexpr size_t Parent(size_t index) noexcept { return (index - 1) / 2; }
};

static_assert(is_allocator<BuddyAllocator, size_t>, "BuddyAllocator is not an allocator");

template <class THeap>
struct BlockAllocation {
    THeap* Heap;
    size_t Start;
    size_t Length;
};

template <class TSubAlloc, class THeap, class TDerived>
requires is_allocator<TSubAlloc, size_t>
class BlockAllocator {
public:
    explicit BlockAllocator(size_t basicPageSize) noexcept
        : _basicPageSize(basicPageSize) {}

    virtual ~BlockAllocator() noexcept = default;

    std::optional<BlockAllocation<THeap>> Allocate(size_t size) noexcept {
        if (size == 0) {
            return std::nullopt;
        }
        const size_t needBucket = BucketForNeed(size);
        EnsureBucketCount(needBucket + 1);
        for (size_t bucket = needBucket; bucket < _bucketHeads.size(); ++bucket) {
            Page* page = _bucketHeads[bucket];
            while (page != nullptr) {
                Page* next = page->_next;
                if (page->_freeBytes >= size) {
                    std::optional<size_t> start = page->_allocator.Allocate(size);
                    if (start.has_value()) {
                        page->_freeBytes -= size;
                        page->_liveAllocs += 1;
                        UpdatePageBucket(page);
                        return BlockAllocation<THeap>{page->_heap.get(), start.value(), size};
                    }
                }
                page = next;
            }
        }
        Page* page = CreatePage(size);
        std::optional<size_t> start = page->_allocator.Allocate(size);
        RADRAY_ASSERT(start.has_value());
        page->_freeBytes -= size;
        page->_liveAllocs += 1;
        UpdatePageBucket(page);
        return BlockAllocation<THeap>{page->_heap.get(), start.value(), size};
    }

    void Destroy(BlockAllocation<THeap> allocation) noexcept {
        RADRAY_ASSERT(allocation.Heap != nullptr);
        auto it = _heapToPage.find(allocation.Heap);
        RADRAY_ASSERT(it != _heapToPage.end());

        Page* page = it->second;
        page->_allocator.Destroy(allocation.Start);
        page->_freeBytes += allocation.Length;
        RADRAY_ASSERT(page->_liveAllocs > 0);
        page->_liveAllocs -= 1;

        if (page->_liveAllocs == 0) {
            ReleasePage(page);
            return;
        }
        UpdatePageBucket(page);
    }

private:
    static constexpr size_t npos = static_cast<size_t>(-1);

    struct Page {
        explicit Page(
            unique_ptr<THeap> heap,
            TSubAlloc&& allocator,
            size_t capacity) noexcept
            : _capacity(capacity),
              _freeBytes(capacity),
              _heap(std::move(heap)),
              _allocator(std::move(std::forward<TSubAlloc>(allocator))) {}

        size_t _capacity = 0;
        size_t _freeBytes = 0;
        size_t _liveAllocs = 0;

        size_t _ownerIndex = 0;
        size_t _bucket = npos;
        Page* _prev = nullptr;
        Page* _next = nullptr;

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
        _bucketHeads.resize(count, nullptr);
    }

    void ListRemove(Page* page) noexcept {
        if (page->_bucket == npos) {
            return;
        }
        Page*& head = _bucketHeads[page->_bucket];
        if (page->_prev != nullptr) {
            page->_prev->_next = page->_next;
        } else {
            RADRAY_ASSERT(head == page);
            head = page->_next;
        }
        if (page->_next != nullptr) {
            page->_next->_prev = page->_prev;
        }
        page->_prev = nullptr;
        page->_next = nullptr;
        page->_bucket = npos;
    }

    void ListInsert(Page* page, size_t bucket) noexcept {
        EnsureBucketCount(bucket + 1);
        Page*& head = _bucketHeads[bucket];
        page->_bucket = bucket;
        page->_prev = nullptr;
        page->_next = head;
        if (head != nullptr) {
            head->_prev = page;
        }
        head = page;
    }

    void UpdatePageBucket(Page* page) noexcept {
        if (page->_freeBytes == 0) {
            ListRemove(page);
            return;
        }
        const size_t newBucket = BucketForFree(page->_freeBytes);
        if (page->_bucket != npos && page->_bucket == newBucket) {
            return;
        }
        ListRemove(page);
        ListInsert(page, newBucket);
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
        _heapToPage.emplace(pagePtr->_heap.get(), pagePtr);
        UpdatePageBucket(pagePtr);
        return pagePtr;
    }

    void ReleasePage(Page* page) noexcept {
        ListRemove(page);
        _heapToPage.erase(page->_heap.get());
        const size_t idx = page->_ownerIndex;
        RADRAY_ASSERT(idx < _pages.size());
        if (idx != _pages.size() - 1) {
            std::swap(_pages[idx], _pages.back());
            _pages[idx]->_ownerIndex = idx;
        }
        _pages.pop_back();
    }

    vector<unique_ptr<Page>> _pages;
    std::unordered_map<THeap*, Page*> _heapToPage;
    vector<Page*> _bucketHeads;
    size_t _basicPageSize;
};

class FreeListAllocator {
public:
    explicit FreeListAllocator(size_t capacity) noexcept;

    std::optional<size_t> Allocate(size_t size) noexcept;

    void Destroy(size_t offset) noexcept;

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
    vector<int32_t> _startToNode;
    uint32_t _head = npos;
};

static_assert(is_allocator<FreeListAllocator, size_t>, "FreeListAllocator is not an allocator");

}  // namespace radray
