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
private:
    enum class NodeState : uint8_t {
        Unused = 0,
        Used = 1,
        Split = 2,
        Full = 3
    };

public:
    BuddyAllocator(size_t capacity) noexcept;
    ~BuddyAllocator() noexcept = default;

    std::optional<size_t> Allocate(size_t size) noexcept;
    void Destroy(size_t offset) noexcept;

private:
    radray::vector<NodeState> _tree;
    size_t _capacity;
};

template <class THeap>
struct BlockAllocation {
    THeap* Heap;
    size_t Start;
    size_t Length;
};

template <class TSubAlloc, class TSubAllocCreate, class THeap, class THeapCreate>
requires is_allocator<TSubAlloc, size_t> &&
         std::is_nothrow_invocable_r_v<TSubAlloc, TSubAllocCreate, size_t> &&
         std::is_nothrow_invocable_r_v<THeap, THeapCreate, size_t>
class BlockAllocator {
public:
    BlockAllocator(
        TSubAllocCreate sac,
        THeapCreate hc,
        size_t basicSize,
        size_t destroyThreshold) noexcept
        : _subAllocCtor(std::move(sac)),
          _heapCtor(std::move(hc)),
          _basicSize(basicSize),
          _destroyThreshold(destroyThreshold) {}

    std::optional<BlockAllocation<THeap>> Allocate(size_t size) noexcept {
        if (size == 0) {
            return std::nullopt;
        }
        for (auto iter = _sizeQuery.lower_bound(size); iter != _sizeQuery.end(); iter++) {
            Block* block = iter->second;
            std::optional<size_t> start = block->_allocator.Allocate(size);
            if (start.has_value()) {
                _sizeQuery.erase(iter);
                block->_freeSize -= size;
                CheckBlockState(block);
                return std::make_optional(BlockAllocation<THeap>{block->_heap.get(), start.value(), size});
            }
        }
        {
            radray::unique_ptr<Block> newBlock = radray::make_unique<Block>(_subAllocCtor, _heapCtor, std::max(size, _basicSize));
            Block* blockPtr = newBlock.get();
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
        Block* block = iter->second.get();
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
            const TSubAllocCreate& subAllocCtor,
            const THeapCreate& heapCtor,
            size_t heapSize) noexcept
            : _heap(radray::make_unique<THeap>(heapCtor(heapSize))),
              _allocator(subAllocCtor(heapSize)),
              _freeSize(heapSize),
              _initSize(heapSize) {}

        radray::unique_ptr<THeap> _heap;
        TSubAlloc _allocator;
        size_t _freeSize;
        size_t _initSize;
    };

    void CheckBlockState(Block* block) noexcept {
        if (block->_freeSize > 0) {
            _sizeQuery.emplace(block->_freeSize, block);
        }
        if (block->_freeSize == block->_initSize) {
            _unused.emplace(block);
            while (_unused.size() > _destroyThreshold) {
                auto selectIter = _unused.begin();
                Block* selectBlock = *selectIter;
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
    }

    radray::unordered_map<THeap*, radray::unique_ptr<Block>> _blocks;
    radray::multimap<size_t, Block*> _sizeQuery;
    radray::unordered_set<Block*> _unused;
    TSubAllocCreate _subAllocCtor;
    THeapCreate _heapCtor;
    size_t _basicSize;
    size_t _destroyThreshold;
};

}  // namespace radray
