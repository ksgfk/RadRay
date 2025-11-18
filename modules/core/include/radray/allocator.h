#pragma once

#include <optional>
#include <type_traits>

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
    BlockAllocator(
        size_t basicSize,
        size_t destroyThreshold) noexcept
        : _basicSize(basicSize),
          _destroyThreshold(destroyThreshold) {}

    virtual ~BlockAllocator() noexcept = default;

    std::optional<BlockAllocation<THeap>> Allocate(size_t size) noexcept {
        if (size == 0) {
            return std::nullopt;
        }
        for (auto iter = _sizeQuery.lower_bound(size); iter != _sizeQuery.end(); iter++) {
            BlockAllocator::Block* block = iter->second;
            std::optional<size_t> start = block->_allocator.Allocate(size);
            if (start.has_value()) {
                _sizeQuery.erase(iter);
                block->_freeSize -= size;
                CheckBlockState(block);
                return std::make_optional(BlockAllocation<THeap>{block->_heap.get(), start.value(), size});
            }
        }
        {
            size_t needSize = std::max(size, _basicSize);
            unique_ptr<BlockAllocator::Block> newBlock = make_unique<BlockAllocator::Block>(
                static_cast<TDerived*>(this)->CreateHeap(needSize),
                static_cast<TDerived*>(this)->CreateSubAllocator(needSize),
                needSize);
            BlockAllocator::Block* blockPtr = newBlock.get();
            auto [newIter, isInsert] = _blocks.emplace(blockPtr->_heap.get(), std::move(newBlock));
            RADRAY_ASSERT(isInsert);
            size_t newStart = blockPtr->_allocator.Allocate(size).value();
            blockPtr->_freeSize -= size;
            CheckBlockState(blockPtr);
            return std::make_optional(BlockAllocation<THeap>{blockPtr->_heap.get(), newStart, size});
        }
    }

    void Destroy(BlockAllocation<THeap> allocation) noexcept {
        auto iter = _blocks.find(allocation.Heap);
        RADRAY_ASSERT(iter != _blocks.end());
        BlockAllocator::Block* block = iter->second.get();
        block->_allocator.Destroy(allocation.Start);
        auto [qBegin, qEnd] = _sizeQuery.equal_range(block->_freeSize);
        for (auto it = qBegin; it != qEnd; it++) {
            if (it->second == block) {
                _sizeQuery.erase(it);
                break;
            }
        }
        block->_freeSize += allocation.Length;
        CheckBlockState(block);
    }

private:
    class Block {
    public:
        Block(
            unique_ptr<THeap> heap,
            TSubAlloc&& allocator,
            size_t heapSize) noexcept
            : _freeSize(heapSize),
              _initSize(heapSize),
              _heap(std::move(heap)),
              _allocator(std::move(std::forward<TSubAlloc>(allocator))) {}

        size_t _freeSize;
        size_t _initSize;
        unique_ptr<THeap> _heap;
        TSubAlloc _allocator;
    };

    void CheckBlockState(BlockAllocator::Block* block) noexcept {
        bool isBlockDestroyed = false;
        if (block->_freeSize == block->_initSize) {
            _unused.emplace(block);
            while (_unused.size() > _destroyThreshold) {
                auto selectIter = _unused.begin();
                BlockAllocator::Block* selectBlock = *selectIter;
                if (selectBlock == block) {
                    isBlockDestroyed = true;
                }
                _unused.erase(selectIter);
                auto [qBegin, qEnd] = _sizeQuery.equal_range(selectBlock->_freeSize);
                for (auto it = qBegin; it != qEnd; it++) {
                    if (it->second == selectBlock) {
                        _sizeQuery.erase(it);
                        break;
                    }
                }
                _blocks.erase(selectBlock->_heap.get());
            }
        } else {
            _unused.erase(block);
        }
        if (block->_freeSize > 0 && !isBlockDestroyed) {
            _sizeQuery.emplace(block->_freeSize, block);
        }
    }

    unordered_map<THeap*, unique_ptr<BlockAllocator::Block>> _blocks;
    multimap<size_t, BlockAllocator::Block*> _sizeQuery;
    unordered_set<BlockAllocator::Block*> _unused;
    size_t _basicSize;
    size_t _destroyThreshold;
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

    class LinkNode {
    public:
        LinkNode(size_t start, size_t length) noexcept;

        size_t _start;
        size_t _length;
        FreeListAllocator::LinkNode* _prev;
        FreeListAllocator::LinkNode* _next;
        NodeState _state;
    };

    unordered_map<size_t, unique_ptr<FreeListAllocator::LinkNode>> _nodes;
    multimap<size_t, FreeListAllocator::LinkNode*> _sizeQuery;
    size_t _capacity;
};

static_assert(is_allocator<FreeListAllocator, size_t>, "FreeListAllocator is not an allocator");

}  // namespace radray
