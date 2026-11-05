#include <radray/object_arena.h>

#include <algorithm>
#include <cstdint>

#include <radray/memory.h>

namespace radray {

namespace {

bool RoundUp(size_t value, size_t alignment, size_t& result) noexcept {
    const size_t padding = (alignment - (value & (alignment - 1))) & (alignment - 1);
    if (value > std::numeric_limits<size_t>::max() - padding) return false;
    result = value + padding;
    return true;
}

}  // namespace

struct ObjectArena::Block {
    Nullable<Block*> Next;
    byte* Data;
    size_t Capacity;
    size_t Used;
    size_t AllocatedBytes;

    static Nullable<Block*> Create(size_t capacity, size_t alignment) noexcept {
        alignment = std::max(alignment, std::max(alignof(Block), alignof(std::max_align_t)));
        size_t offset;
        if (!RoundUp(sizeof(Block), alignment, offset)) return nullptr;
        if (capacity > std::numeric_limits<size_t>::max() - offset) return nullptr;
        size_t total;
        if (!RoundUp(offset + capacity, alignment, total)) return nullptr;
        if (total > static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max())) return nullptr;
        Nullable<void*> storage = AlignedAlloc(alignment, total);
        if (!storage) return nullptr;
        return std::construct_at(static_cast<Block*>(storage.Get()), Block{nullptr, static_cast<byte*>(storage.Get()) + offset, capacity, 0, total});
    }

    Nullable<void*> Allocate(size_t bytes, size_t alignment) noexcept {
        void* cursor = Data + Used;
        size_t available = Capacity - Used;
        if (!std::align(alignment, bytes, cursor, available)) return nullptr;
        Used = static_cast<size_t>(static_cast<byte*>(cursor) - Data) + bytes;
        return cursor;
    }

    void Free() noexcept {
        std::destroy_at(this);
        AlignedFree(this);
    }
};

ObjectArena::ObjectArena(ObjectArenaOptions options) noexcept : _options(options) {
    if (_options.BlockSize == 0) RADRAY_ABORT("ObjectArena BlockSize must be positive");
}

ObjectArena::~ObjectArena() noexcept { Release(); }

Nullable<void*> ObjectArena::TryAllocateBytes(size_t bytes, size_t alignment) noexcept {
#ifdef RADRAY_IS_DEBUG
    RADRAY_ASSERT(!_reclaiming);
#endif
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    bytes = std::max(bytes, size_t{1});
    const size_t padding = alignment <= alignof(std::max_align_t) ? 0 : alignment - 1;
    if (bytes > _options.BlockSize || padding > _options.BlockSize - bytes) {
        auto block = Block::Create(bytes, alignment);
        if (!block) return nullptr;
        block->Next = _largeBlocks;
        _largeBlocks = block;
        return block->Allocate(bytes, alignment);
    }

    for (auto block = _currentBlock; block; block = block->Next) {
        if (auto storage = block->Allocate(bytes, alignment)) {
            _currentBlock = block;
            return storage;
        }
    }
    auto block = Block::Create(_options.BlockSize, alignof(std::max_align_t));
    if (!block) return nullptr;
    if (_lastBlock)
        _lastBlock->Next = block;
    else
        _blocks = block;
    _lastBlock = block;
    _currentBlock = block;
    return block->Allocate(bytes, alignment);
}

void* ObjectArena::AllocateBytes(size_t bytes, size_t alignment) {
    auto storage = TryAllocateBytes(bytes, alignment);
    if (!storage) throw std::bad_alloc{};
    return storage.Get();
}

void ObjectArena::ReserveCleanup() {
    const size_t limit = _cleanups.max_size();
    if (_pendingCleanups >= limit - _cleanups.size()) throw std::bad_alloc{};
    const size_t required = _cleanups.size() + _pendingCleanups + 1;
    if (required > _cleanups.capacity()) {
        const size_t growth = _cleanups.capacity() > limit / 2 ? limit : _cleanups.capacity() * 2;
        _cleanups.reserve(std::max(required, growth));
    }
    ++_pendingCleanups;
}

void ObjectArena::DestroyObjects() noexcept {
    RADRAY_ASSERT(_pendingCleanups == 0);
    for (size_t i = _cleanups.size(); i != 0; --i) {
        const auto& cleanup = _cleanups[i - 1];
        cleanup.Destroy(cleanup.Object);
    }
    _cleanups.clear();
}

void ObjectArena::FreeLargeBlocks() noexcept {
    while (_largeBlocks) {
        auto block = _largeBlocks;
        _largeBlocks = block->Next;
        block->Free();
    }
}

size_t ObjectArena::GetCleanupBytes() const noexcept {
    return _cleanups.capacity() == _cleanups.inline_capacity ? 0 : _cleanups.capacity() * sizeof(CleanupRecord);
}

void ObjectArena::Reset() noexcept {
#ifdef RADRAY_IS_DEBUG
    RADRAY_ASSERT(!_reclaiming && _constructionDepth == 0);
    _reclaiming = true;
    auto reclaiming = MakeScopeGuard([this]() noexcept { _reclaiming = false; });
#endif
    DestroyObjects();
    FreeLargeBlocks();
    size_t budget = _options.MaxRetainedBytes;
    if (GetCleanupBytes() > budget) _cleanups = InlineVector<CleanupRecord, 16>{};
    budget -= GetCleanupBytes();

    auto block = _blocks;
    _blocks = nullptr;
    _lastBlock = nullptr;
    while (block) {
        auto next = block->Next;
        if (block->AllocatedBytes <= budget) {
            budget -= block->AllocatedBytes;
            block->Used = 0;
            block->Next = nullptr;
            if (_lastBlock)
                _lastBlock->Next = block;
            else
                _blocks = block;
            _lastBlock = block;
        } else {
            block->Free();
        }
        block = next;
    }
    _currentBlock = _blocks;
}

void ObjectArena::Release() noexcept {
#ifdef RADRAY_IS_DEBUG
    RADRAY_ASSERT(!_reclaiming && _constructionDepth == 0);
    _reclaiming = true;
    auto reclaiming = MakeScopeGuard([this]() noexcept { _reclaiming = false; });
#endif
    DestroyObjects();
    FreeLargeBlocks();
    while (_blocks) {
        auto block = _blocks;
        _blocks = block->Next;
        block->Free();
    }
    _lastBlock = nullptr;
    _currentBlock = nullptr;
    _cleanups = InlineVector<CleanupRecord, 16>{};
}

size_t ObjectArena::GetReservedBytes() const noexcept {
#ifdef RADRAY_IS_DEBUG
    RADRAY_ASSERT(!_reclaiming);
#endif
    size_t bytes = GetCleanupBytes();
    for (auto block = _blocks; block; block = block->Next) bytes += block->AllocatedBytes;
    for (auto block = _largeBlocks; block; block = block->Next) bytes += block->AllocatedBytes;
    return bytes;
}

}  // namespace radray
