#pragma once

#include <type_traits>
#include <utility>
#include <atomic>

#include <radray/types.h>

namespace radray {

class Object {
public:
    Object() noexcept = default;
    virtual ~Object() noexcept = default;

    uint64_t AddRef() noexcept;

    uint64_t RemoveRef() noexcept;

    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

private:
    std::atomic_uint64_t _refCount{0};
};

template <class T>
class RC {
public:
    RC() noexcept = default;
    RC(std::nullptr_t) noexcept : _ptr{nullptr} {}
    explicit RC(T* ptr) noexcept : _ptr{ptr} {
        InternalAddRef();
    }
    RC(const RC& other) noexcept : _ptr{other._ptr} {
        InternalAddRef();
    }
    RC(RC&& other) noexcept : _ptr{other._ptr} {
        other._ptr = nullptr;
    }
    template <class U, typename std::enable_if_t<std::is_convertible_v<U*, T*>, int> = 0>
    RC(const RC<U>& other) noexcept : _ptr{static_cast<T*>(other._ptr)} {
        InternalAddRef();
    }
    template <class U, typename std::enable_if_t<std::is_convertible_v<U*, T*>, int> = 0>
    RC(RC<U>&& other) noexcept : _ptr{static_cast<T*>(other._ptr)} {
        other._ptr = nullptr;
    }
    RC& operator=(std::nullptr_t) noexcept {
        InternalRelease();
        return *this;
    }
    RC& operator=(T* other) noexcept {
        if (_ptr != other) {
            RC copy{other};
            std::swap(copy, *this);
        }
        return *this;
    }
    RC& operator=(const RC& other) noexcept {
        if (_ptr != other._ptr) {
            RC copy{other};
            std::swap(copy, *this);
        }
        return *this;
    }
    RC& operator=(RC&& other) noexcept {
        RC mv{std::move(other)};
        std::swap(mv, *this);
        return *this;
    }
    ~RC() noexcept {
        InternalRelease();
    }

    void Reset() noexcept {
        InternalRelease();
    }

    T* Get() const noexcept { return _ptr; }

    T* operator->() const noexcept { return _ptr; }

    friend void swap(RC& first, RC& second) noexcept {
        std::swap(first._ptr, second._ptr);
    }

    template <class U>
    friend class RC;

private:
    void InternalAddRef() noexcept {
        if (_ptr != nullptr) {
            _ptr->AddRef();
        }
    }

    void InternalRelease() noexcept {
        if (_ptr != nullptr) {
            uint64_t refCount = _ptr->RemoveRef();
            if (refCount == 0) {
                delete _ptr;
            }
            _ptr = nullptr;
        }
    }

    T* _ptr{nullptr};
};

template <class T>
requires std::is_base_of_v<Object, T>
bool operator==(const RC<T>& l, const RC<T>& r) noexcept {
    return l.Get() == r.Get();
}

template <class T>
requires std::is_base_of_v<Object, T>
bool operator!=(const RC<T>& l, const RC<T>& r) noexcept {
    return l.Get() != r.Get();
}

template <class T, class... Args>
requires std::is_base_of_v<Object, T> && std::is_constructible_v<T, Args...>
RC<T> MakeObject(Args&&... args) {
    return RC<T>{new T{std::forward<Args>(args)...}};
}

}  // namespace radray
