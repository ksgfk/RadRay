#pragma once

#include <optional>
#include <type_traits>

#include <radray/types.h>
#include <radray/utility.h>
#include <radray/logger.h>

namespace radray {

class BuddyAllocator {
private:
    enum class NodeState : uint8_t {
        Unused = 0,
        Used = 1,
        Split = 2,
        Full = 3
    };

public:
    BuddyAllocator(uint64_t capacity) noexcept;
    ~BuddyAllocator() noexcept = default;

    std::optional<uint64_t> Allocate(uint64_t size) noexcept;
    void Destroy(uint64_t offset) noexcept;

private:
    radray::vector<NodeState> _tree;
    uint64_t _capacity;
};

template <class TBlock, class TData, class TDerived>
class TightAllocator;
template <class TBlock, class TData, class TDerived>
class TightAllocatorChunk;
template <class TBlock, class TData, class TDerived>
class TightAllocation;

template <class TBlock, class TData, class TDerived>
class TightAllocatorChunk {
private:
    using ChunkType = TightAllocatorChunk<TBlock, TData, TDerived>;

    explicit TightAllocatorChunk(size_t chunkSize) noexcept
        : _skipfield(chunkSize, 0) {}

    size_t GetSize() const noexcept { return _skipfield.size(); }

    size_t FindEmpty(size_t size) const noexcept {
        return std::numeric_limits<size_t>::max();
    }

private:
    radray::vector<size_t> _skipfield;
    radray::unique_ptr<ChunkType> _next;
    ChunkType* _prev{nullptr};
    ChunkType* _fastJmp{nullptr};
    TBlock _data;

    friend class TightAllocator<TBlock, TData, TDerived>;
};

template <class TBlock, class TData, class TDerived>
class TightAllocation {
    using ChunkType = TightAllocatorChunk<TBlock, TData, TDerived>;

private:
    ChunkType* _chunk;
    size_t _offset;
    size_t _size;
};

template <class TBlock, class TData, class TDerived>
class TightAllocator {
private:
    using AllocationType = TightAllocation<TBlock, TData, TDerived>;
    using ChunkType = TightAllocatorChunk<TBlock, TData, TDerived>;

public:
    explicit TightAllocator(size_t chunkSize) noexcept
        : _chunkSize(chunkSize) {}

    virtual ~TightAllocator() noexcept {
    }

    std::optional<AllocationType> Allocate(size_t size) noexcept {
        if (_rootChunk == nullptr) {
            size_t allocSize = std::max(size, _chunkSize);
            auto chunk = radray::make_unique<ChunkType>(allocSize);
            std::optional<TBlock> block = AllocateNewBlock(allocSize);
            if (!block.has_value()) {
                RADRAY_ERR_LOG("TightAllocator failed to allocate new block");
                return std::nullopt;
            }
            chunk->_data = block.value();
            _rootChunk = std::move(chunk);
        }
        ChunkType* chunk = _rootChunk.get();
        while (chunk != nullptr) {
            size_t start = chunk->FindEmpty(size);
            if (start == std::numeric_limits<size_t>::max()) {
                chunk = chunk->_fastJmp;
                continue;
            }
        }
    }

private:
    std::optional<TBlock> AllocateNewBlock(size_t size) noexcept {
        return static_cast<TDerived*>(this)->AllocateNewBlockImpl(size);
    }

    void DestroyBlock(TBlock&& block) noexcept {
        static_cast<TDerived*>(this)->DestroyBlockImpl(std::move(block));
    }

private:
    radray::unique_ptr<ChunkType> _rootChunk;
    size_t _chunkSize;
};

}  // namespace radray
