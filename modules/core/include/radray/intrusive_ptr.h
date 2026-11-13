#pragma once

// Reference ownership and threading contracts: docs/architecture/core-facilities.md.

#include <atomic>
#include <compare>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>

#include <radray/logger.h>
#include <radray/nullable.h>
#include <radray/types.h>

namespace radray {

template <class T>
class RefCountedBase {
public:
    RefCountedBase(const RefCountedBase&) = delete;
    RefCountedBase& operator=(const RefCountedBase&) = delete;
    RefCountedBase(RefCountedBase&&) = delete;
    RefCountedBase& operator=(RefCountedBase&&) = delete;

    void Retain() const noexcept {
        if (_refCount == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("RefCountedBase: reference overflow");
        ++_refCount;
    }

    void Release() const noexcept {
        if (_refCount == 0) RADRAY_ABORT("RefCountedBase: reference underflow");
        if (--_refCount == 0) delete static_cast<const T*>(this);
    }

    uint32_t UseCount() const noexcept { return _refCount; }

protected:
    RefCountedBase() noexcept = default;
    virtual ~RefCountedBase() noexcept {
        if (_refCount != 0) RADRAY_ABORT("RefCountedBase: destruction requires zero references");
    }

private:
    mutable uint32_t _refCount{0};
};

template <class T>
class ThreadSafeRefCountedBase {
public:
    ThreadSafeRefCountedBase(const ThreadSafeRefCountedBase&) = delete;
    ThreadSafeRefCountedBase& operator=(const ThreadSafeRefCountedBase&) = delete;
    ThreadSafeRefCountedBase(ThreadSafeRefCountedBase&&) = delete;
    ThreadSafeRefCountedBase& operator=(ThreadSafeRefCountedBase&&) = delete;

    void Retain() const noexcept {
        if (_refCount.fetch_add(1, std::memory_order_relaxed) == std::numeric_limits<uint32_t>::max()) {
            RADRAY_ABORT("ThreadSafeRefCountedBase: reference overflow");
        }
    }

    void Release() const noexcept {
        const uint32_t previous = _refCount.fetch_sub(1, std::memory_order_acq_rel);
        if (previous == 0) RADRAY_ABORT("ThreadSafeRefCountedBase: reference underflow");
        if (previous == 1) delete static_cast<const T*>(this);
    }

    // A snapshot only; this does not synchronize access to the object or its owners.
    uint32_t UseCount() const noexcept { return _refCount.load(std::memory_order_relaxed); }

protected:
    ThreadSafeRefCountedBase() noexcept = default;
    virtual ~ThreadSafeRefCountedBase() noexcept {
        if (UseCount() != 0) RADRAY_ABORT("ThreadSafeRefCountedBase: destruction requires zero references");
    }

private:
    mutable std::atomic<uint32_t> _refCount{0};
};

template <class T>
struct IntrusivePtrTraits {
    static void Retain(T* object) noexcept { object->Retain(); }
    static void Release(T* object) noexcept { object->Release(); }
};

template <class T>
class IntrusivePtr {
public:
    using element_type = T;

    constexpr IntrusivePtr() noexcept = default;
    constexpr IntrusivePtr(std::nullptr_t) noexcept {}

    explicit IntrusivePtr(Nullable<T*> ptr) noexcept : _ptr(ptr.Get()) {
        if (_ptr != nullptr) {
            IntrusivePtrTraits<T>::Retain(_ptr);
        }
    }

    IntrusivePtr(const IntrusivePtr& other) noexcept : _ptr(other._ptr) {
        if (_ptr != nullptr) {
            IntrusivePtrTraits<T>::Retain(_ptr);
        }
    }

    IntrusivePtr(IntrusivePtr&& other) noexcept : _ptr(other._ptr) {
        other._ptr = nullptr;
    }

    template <class U>
    requires(!std::is_same_v<U, T>) && std::is_convertible_v<U*, T*>
    IntrusivePtr(const IntrusivePtr<U>& other) noexcept : _ptr(other.Get()) {
        if (_ptr != nullptr) {
            IntrusivePtrTraits<T>::Retain(_ptr);
        }
    }

    template <class U>
    requires(!std::is_same_v<U, T>) && std::is_convertible_v<U*, T*>
    IntrusivePtr(IntrusivePtr<U>&& other) noexcept : _ptr(std::exchange(other._ptr, nullptr)) {}

    IntrusivePtr& operator=(const IntrusivePtr& other) noexcept {
        IntrusivePtr(other).Swap(*this);
        return *this;
    }

    IntrusivePtr& operator=(IntrusivePtr&& other) noexcept {
        IntrusivePtr(std::move(other)).Swap(*this);
        return *this;
    }

    IntrusivePtr& operator=(std::nullptr_t) noexcept {
        Reset();
        return *this;
    }

    ~IntrusivePtr() noexcept { Reset(); }

    T* Get() const noexcept { return _ptr; }
    T* operator->() const noexcept { return _ptr; }
    T& operator*() const noexcept { return *_ptr; }

    bool HasValue() const noexcept { return _ptr != nullptr; }
    explicit operator bool() const noexcept { return HasValue(); }

    void Reset() noexcept {
        if (T* old = std::exchange(_ptr, nullptr); old != nullptr) {
            IntrusivePtrTraits<T>::Release(old);
        }
    }

    void Swap(IntrusivePtr& other) noexcept { std::swap(_ptr, other._ptr); }

    friend void swap(IntrusivePtr& lhs, IntrusivePtr& rhs) noexcept { lhs.Swap(rhs); }

    friend bool operator==(const IntrusivePtr& lhs, const IntrusivePtr& rhs) noexcept {
        return lhs._ptr == rhs._ptr;
    }

    friend std::strong_ordering operator<=>(const IntrusivePtr& lhs, const IntrusivePtr& rhs) noexcept {
        return std::compare_three_way{}(lhs._ptr, rhs._ptr);
    }

    friend bool operator==(const IntrusivePtr& lhs, std::nullptr_t) noexcept {
        return lhs._ptr == nullptr;
    }

private:
    template <class U>
    friend class IntrusivePtr;

    T* _ptr{nullptr};
};

}  // namespace radray

namespace std {

template <class T>
struct hash<radray::IntrusivePtr<T>> {
    size_t operator()(const radray::IntrusivePtr<T>& ptr) const noexcept {
        return hash<T*>{}(ptr.Get());
    }
};

}  // namespace std
