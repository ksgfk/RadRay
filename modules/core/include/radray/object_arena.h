#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include <radray/inline_vector.h>
#include <radray/nullable.h>
#include <radray/scope_guard.h>

namespace radray {

struct ObjectArenaOptions {
    size_t BlockSize = 64 * 1024;
    size_t MaxRetainedBytes = 256 * 1024;
};

// See docs/architecture/core-facilities.md for storage and allocator lifetime contracts.
class ObjectArena final {
public:
    // BlockSize must be positive. Access must be serialized by the caller.
    explicit ObjectArena(ObjectArenaOptions options = {}) noexcept;
    ~ObjectArena() noexcept;

    ObjectArena(const ObjectArena&) = delete;
    ObjectArena& operator=(const ObjectArena&) = delete;
    ObjectArena(ObjectArena&&) = delete;
    ObjectArena& operator=(ObjectArena&&) = delete;

    // Raw storage only; zero bytes consumes one byte. Alignment must be a nonzero
    // power of two. Invalid input, overflow and allocation failure return empty.
    [[nodiscard]] Nullable<void*> TryAllocateBytes(size_t bytes, size_t alignment) noexcept;
    // Same allocation contract, but failure throws std::bad_alloc.
    [[nodiscard]] void* AllocateBytes(size_t bytes, size_t alignment);

    // Constructor arguments are forwarded unchanged. Successful objects are
    // destroyed in reverse completion order; the destructor must not throw.
    // A failed construction leaves consumed storage and completed nested objects
    // in the arena. Do not destroy registered objects manually.
    template <class T, class... Args>
    T* Emplace(Args&&... args) {
        static_assert(std::is_object_v<T> && !std::is_array_v<T>);
        static_assert(std::is_same_v<T, std::remove_cv_t<T>>);
        static_assert(std::is_nothrow_destructible_v<T>);
#ifdef RADRAY_IS_DEBUG
        RADRAY_ASSERT(!_reclaiming);
        ++_constructionDepth;
        auto construction = MakeScopeGuard([this]() noexcept { --_constructionDepth; });
#endif
        if constexpr (std::is_trivially_destructible_v<T>) {
            return std::construct_at(static_cast<T*>(AllocateBytes(sizeof(T), alignof(T))), std::forward<Args>(args)...);
        } else {
            ReserveCleanup();
            auto reservation = MakeScopeGuard([this]() noexcept { --_pendingCleanups; });
            T* object = std::construct_at(static_cast<T*>(AllocateBytes(sizeof(T), alignof(T))), std::forward<Args>(args)...);
            // ReserveCleanup accounts for all unfinished nested constructions,
            // so committing an object cannot allocate after ownership transfer.
            _cleanups.emplace_back(CleanupRecord{object, [](void* value) noexcept { std::destroy_at(static_cast<T*>(value)); }});
            return object;
        }
    }

    // Both operations invalidate all allocations and run registered destructors.
    // External containers must already be destroyed, not merely cleared.
    // Neither operation may run during construction; cleanup must not re-enter
    // this arena. Reset retains storage within MaxRetainedBytes; Release frees it.
    void Reset() noexcept;
    void Release() noexcept;

    // O(block count), including block headers/padding and heap cleanup capacity;
    // excludes this object's inline storage and system allocator overhead.
    size_t GetReservedBytes() const noexcept;

private:
    struct Block;
    struct CleanupRecord {
        void* Object;
        void (*Destroy)(void*) noexcept;
    };

    void ReserveCleanup();
    void DestroyObjects() noexcept;
    void FreeLargeBlocks() noexcept;
    size_t GetCleanupBytes() const noexcept;

    ObjectArenaOptions _options;
    Nullable<Block*> _blocks{nullptr};
    Nullable<Block*> _lastBlock{nullptr};
    Nullable<Block*> _currentBlock{nullptr};
    Nullable<Block*> _largeBlocks{nullptr};
    InlineVector<CleanupRecord, 16> _cleanups;
    size_t _pendingCleanups{0};
#ifdef RADRAY_IS_DEBUG
    size_t _constructionDepth{0};
    bool _reclaiming{false};
#endif
};

// Borrows an arena. Individual deallocation does not reclaim storage, and
// construct/destroy belong to the container rather than the arena cleanup list.
template <class T>
class ArenaAllocator {
public:
    static_assert(std::is_object_v<T> && std::is_same_v<T, std::remove_cv_t<T>>);
    using value_type = T;
    using size_type = size_t;
    using difference_type = std::ptrdiff_t;
    using is_always_equal = std::false_type;
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_swap = std::false_type;

    template <class U>
    struct rebind {
        using other = ArenaAllocator<U>;
    };

    ArenaAllocator() = delete;
    explicit ArenaAllocator(ObjectArena& arena) noexcept : _arena(&arena) {}
    ArenaAllocator(const ArenaAllocator&) noexcept = default;
    ArenaAllocator& operator=(const ArenaAllocator&) noexcept = default;

    template <class U>
    ArenaAllocator(const ArenaAllocator<U>& other) noexcept : _arena(&other.GetArena()) {}

    [[nodiscard]] T* allocate(size_t count) {
        if (count > max_size()) throw std::bad_array_new_length{};
        return static_cast<T*>(_arena->AllocateBytes(count * sizeof(T), alignof(T)));
    }

    void deallocate(T* pointer, size_t count) noexcept {
        (void)pointer;
        (void)count;
    }

    template <class U, class... Args>
    void construct(U* pointer, Args&&... args) {
        std::uninitialized_construct_using_allocator(pointer, *this, std::forward<Args>(args)...);
    }

    size_t max_size() const noexcept { return std::numeric_limits<size_t>::max() / sizeof(T); }
    ObjectArena& GetArena() const noexcept { return *_arena; }

    template <class U>
    bool operator==(const ArenaAllocator<U>& other) const noexcept {
        return _arena == &other.GetArena();
    }

private:
    ObjectArena* _arena;
};

}  // namespace radray
