#pragma once

#include <type_traits>

#include <Metal/Metal.hpp>

namespace radray::rhi::metal {

template <class T, class Base = class NS::Object>
requires std::is_base_of_v<NS::Referencing<T, Base>, T>
class NSRef {
public:
    NSRef() noexcept : _ptr(nullptr) {}

    explicit NSRef(T* ptr) noexcept : _ptr(ptr) {}

    NSRef(const NSRef& other) noexcept : _ptr(other._ptr) {
        if (_ptr != nullptr) {
            _ptr->retain();
        }
    }

    NSRef(NSRef&& other) noexcept : _ptr(other._ptr) {
        other._ptr = nullptr;
    }

    NSRef& operator=(const NSRef& other) noexcept {
        NSRef{other}.Swap(*this);
        return *this;
    }

    NSRef& operator=(NSRef&& other) noexcept {
        NSRef{std::move(other)}.Swap(*this);
        return *this;
    }

    ~NSRef() noexcept {
        if (_ptr != nullptr) {
            _ptr->release();
            _ptr = nullptr;
        }
    }

    T* Get() const noexcept { return _ptr; }

    T* operator->() const noexcept { return _ptr; }

    void Swap(NSRef& other) noexcept {
        T* temp = _ptr;
        _ptr = other._ptr;
        other._ptr = temp;
    }

private:
    T* _ptr;
};

}  // namespace radray::rhi::metal
