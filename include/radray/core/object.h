#pragma once

#include <type_traits>
#include <utility>

#include <radray/types.h>

namespace radray {

class Object {
public:
    Object() = default;
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    virtual ~Object() noexcept = default;

    virtual uint64_t AddRef() = 0;

    virtual uint64_t RemoveRef() = 0;
};

template <class T>
class RC {
public:
    RC() noexcept = default;
    RC(std::nullptr_t) : _ptr{nullptr} {}
    explicit RC(T* ptr) : _ptr{ptr} {
        InternalAddRef();
    }
    RC(const RC& other) : _ptr{other._ptr} {
        InternalAddRef();
    }
    RC(RC&& other) noexcept : RC{other._ptr} {
        other._ptr = nullptr;
    }
    template <class U, typename std::enable_if_t<std::is_convertible_v<U*, T*>, int> = 0>
    RC(const RC<U>& other) : _ptr{static_cast<T*>(other._ptr)} {
        InternalAddRef();
    }
    template <class U, typename std::enable_if_t<std::is_convertible_v<U*, T*>, int> = 0>
    RC(RC<U>&& other) noexcept : _ptr{static_cast<T*>(other._ptr)} {
        other._ptr = nullptr;
    }
    RC& operator=(std::nullptr_t) {
        InternalRelease();
        return *this;
    }
    RC& operator=(T* other) {
        if (_ptr != other) {
            RC copy{other};
            std::swap(copy, *this);
        }
        return *this;
    }
    RC& operator=(const RC& other) {
        if (_ptr != other._ptr) {
            RC copy{other};
            std::swap(copy, *this);
        }
        return *this;
    }
    RC& operator=(RC&& other) {
        RC mv{std::move(other)};
        std::swap(mv, *this);
        return *this;
    }
    ~RC() {
        InternalRelease();
    }

    void Reset() {
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
    void InternalAddRef() {
        if (_ptr != nullptr) {
            _ptr->AddRef();
        }
    }

    void InternalRelease() {
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
