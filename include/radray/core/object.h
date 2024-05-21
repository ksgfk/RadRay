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

    virtual uint64_t Release() = 0;
};

template <class T>
requires std::is_base_of_v<Object, T>
class RC {
public:
    RC() noexcept = default;
    explicit RC(T* ptr) : _ptr{ptr} {
        InternalAddRef();
    }
    RC(const RC& other) : _ptr{other._ptr} {
        InternalAddRef();
    }
    RC(RC&& other) noexcept : RC{other._ptr} {
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

    friend void swap(RC& first, RC& second) noexcept {
        std::swap(first._ptr, second._ptr);
    }

    T** GetAddressOf() noexcept { return &_ptr; }  // dangerous

    T** ReleaseAndGetAddressOf() {
        InternalRelease();
        return &_ptr;
    }

    void Reset() {
        InternalRelease();
    }

    T* Get() const noexcept { return _ptr; }

    T* operator->() const noexcept { return _ptr; }

private:
    void InternalAddRef() {
        if (_ptr != nullptr) {
            _ptr->AddRef();
        }
    }
    void InternalRelease() {
        if (_ptr != nullptr) {
            _ptr->Release();
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
requires std::is_base_of_v<Object, T>
RC<T> MakeObject(Args... args) {
    RC<T> result{};
    *(result.GetAddressOf()) = new T{std::forward<Args>(args)...};
    return result;
}

}  // namespace radray
