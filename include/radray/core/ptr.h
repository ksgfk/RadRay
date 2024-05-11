#pragma once

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

template <class T>
class RC {
public:
    RC() noexcept : _ptr(nullptr) {}
    RC(std::nullptr_t) noexcept : _ptr(nullptr) {}
    RC(T* other) noexcept : _ptr(other) { InternalAddRef(); }
    RC(const RC& other) noexcept : _ptr(other._ptr) { InternalAddRef(); }
    RC(RC&& other) noexcept : _ptr(other._ptr) { other.ptr_ = nullptr; }
    RC& operator=(std::nullptr_t) noexcept {
        InternalRelease();
        return *this;
    }
    RC& operator=(T* other) noexcept {
        if (_ptr != other) {
            RC(other).Swap(*this);
        }
        return *this;
    }
    RC& operator=(const RC& other) noexcept {
        if (_ptr != other._ptr) {
            RC(other).Swap(*this);
        }
        return *this;
    }
    RC& operator=(RC&& other) noexcept {
        RC(std::move(other)).Swap(*this);
        return *this;
    }
    ~RC() noexcept { InternalRelease(); }

    void Swap(RC&& r) noexcept {
        T* tmp = _ptr;
        _ptr = r._ptr;
        r._ptr = tmp;
    }

    void Swap(RC& r) noexcept {
        T* tmp = _ptr;
        _ptr = r._ptr;
        r._ptr = tmp;
    }

    T* Get() const noexcept { return _ptr; }
    T* operator->() const noexcept { return _ptr; }
    T* const* GetAddressOf() const noexcept { return &_ptr; }
    T** GetAddressOf() noexcept { return &_ptr; }
    operator T*() const noexcept { return _ptr; }
    T** ReleaseAndGetAddressOf() noexcept {
        InternalRelease();
        return &_ptr;
    }

    T* Detach() noexcept {
        T* ptr = _ptr;
        _ptr = nullptr;
        return ptr;
    }
    void Attach(T* other) noexcept {
        if (_ptr != nullptr) {
            auto ref = _ptr->Release();
            RADRAY_ASSERT(ref != 0 || _ptr != other, "Attaching to the same object only works if duplicate references are being coalesced.");
        }
        _ptr = other;
    }

    uint64_t Reset() { return InternalRelease(); }

private:
    T* _ptr;

    void InternalAddRef() const noexcept {
        if (_ptr != nullptr) {
            _ptr->AddRef();
        }
    }

    uint64_t InternalRelease() noexcept {
        uint64_t ref = 0;
        T* temp = _ptr;
        if (temp != nullptr) {
            _ptr = nullptr;
            ref = temp->Release();
        }
        return ref;
    }
};

template <class T>
class Box {
public:
    Box() noexcept : _ptr(nullptr) {}
    Box(std::nullptr_t) noexcept : _ptr(nullptr) {}
    Box(T* other) noexcept : _ptr(other) { InternalAddRef(); }
    Box(Box&& other) noexcept : _ptr(other._ptr) { other.ptr_ = nullptr; }
    Box& operator=(std::nullptr_t) noexcept {
        InternalRelease();
        return *this;
    }
    Box& operator=(T* other) noexcept {
        if (_ptr != other) {
            Box(other).Swap(*this);
        }
        return *this;
    }
    Box& operator=(Box&& other) noexcept {
        Box(std::move(other)).Swap(*this);
        return *this;
    }
    ~Box() noexcept { InternalRelease(); }

    void Swap(Box&& r) noexcept {
        T* tmp = _ptr;
        _ptr = r._ptr;
        r._ptr = tmp;
    }

    T* Get() const noexcept { return _ptr; }
    T* operator->() const noexcept { return _ptr; }
    operator T*() const noexcept { return _ptr; }
    T** ReleaseAndGetAddressOf() noexcept {
        InternalRelease();
        return &_ptr;
    }

    T* Detach() noexcept {
        T* ptr = _ptr;
        _ptr = nullptr;
        return ptr;
    }
    void Attach(T* other) noexcept {
        if (_ptr != nullptr) {
            RADRAY_ASSERT(_ptr != other, "may leak");
            auto ref = _ptr->Release();
            RADRAY_ASSERT(ref == 0, "not unique");
        }
        _ptr = other;
    }

    uint64_t Reset() { return InternalRelease(); }

private:
    T* _ptr;

    void InternalAddRef() const noexcept {
        if (_ptr != nullptr) {
            _ptr->AddRef();
        }
    }

    uint64_t InternalRelease() noexcept {
        uint64_t ref = 0;
        T* temp = _ptr;
        if (temp != nullptr) {
            _ptr = nullptr;
            ref = temp->Release();
        }
        return ref;
    }
};

}  // namespace radray
