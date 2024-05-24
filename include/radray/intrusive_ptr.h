#pragma once

namespace radray {

template <class T>
void IntrusivePtrAddRef(T* p) noexcept {
    p->AddRef();
}

template <class T>
void IntrusivePtrRelease(T* p) noexcept {
    p->Release();
}

template <class T>
class IntrusivePtr {
public:
    IntrusivePtr() noexcept : _ptr(nullptr) {}
    IntrusivePtr(T* p, bool isAddRef = true) noexcept : _ptr(p) {
        if (_ptr && isAddRef) {
            IntrusivePtrAddRef(_ptr);  // 不写明命名空间可以允许命名空间查找 模版还有这种trick...
        }
    }
    IntrusivePtr(const IntrusivePtr& ip) noexcept : _ptr(ip._ptr) {
        if (_ptr) {
            IntrusivePtrAddRef(_ptr);
        }
    }
    IntrusivePtr(IntrusivePtr&& ip) noexcept : _ptr(nullptr) {
        Swap(ip);
    }
    template <class U>
    IntrusivePtr(const IntrusivePtr<U>& ip) noexcept : _ptr(ip._ptr) {
        if (_ptr) {
            IntrusivePtrAddRef(_ptr);
        }
    }
    IntrusivePtr& operator=(const IntrusivePtr& ip) noexcept {
        return operator=(ip._ptr);
    }
    IntrusivePtr& operator=(IntrusivePtr&& ip) noexcept {
        Swap(ip);
        return *this;
    }
    template <class U>
    IntrusivePtr& operator=(const IntrusivePtr<U>& ip) noexcept {
        return operator=(ip._ptr);
    }
    IntrusivePtr& operator=(T* other) noexcept {
        if (other != _ptr) {
            const T* temp = _ptr;
            if (other) {
                IntrusivePtrAddRef(other);
            }
            _ptr = other;
            if (temp) {
                IntrusivePtrRelease(temp);
            }
        }
        return *this;
    }
    ~IntrusivePtr() noexcept {
        if (_ptr) {
            IntrusivePtrRelease(_ptr);
        }
    }

    T* Get() const noexcept { return _ptr; }

    void Reset() noexcept {
        const T* temp = _ptr;
        _ptr = nullptr;
        if (temp) {
            IntrusivePtrRelease(temp);
        }
    }

    void Swap(IntrusivePtr& ip) noexcept {
        const T* temp = _ptr;
        _ptr = ip._ptr;
        ip._ptr = temp;
    }

    void Attach(T other) noexcept {
        const T* temp = _ptr;
        _ptr = other;
        if (temp) {
            intrusive_ptr_release(temp);
        }
    }

    T* Detach() noexcept {
        const T* temp = _ptr;
        _ptr = nullptr;
        return temp;
    }

    T& operator*() const noexcept { return *_ptr; }

    T* operator->() const noexcept { return _ptr; }

private:
    T* _ptr;

    template <class U>
    friend class IntrusivePtr;
};

template <class T>
void swap(IntrusivePtr<T>& a, IntrusivePtr<T>& b) noexcept {
    a.Swap(b);
}

template <class T, class U>
bool operator==(IntrusivePtr<T> const& iPtr1, IntrusivePtr<U> const& iPtr2) {
    return (iPtr1.Get() == iPtr2.Get());
}

template <class T, class U>
bool operator!=(IntrusivePtr<T> const& iPtr1, IntrusivePtr<U> const& iPtr2) {
    return (iPtr1.Get() != iPtr2.Get());
}

template <class T>
bool operator==(IntrusivePtr<T> const& iPtr1, T* p) {
    return (iPtr1.Get() == p);
}

template <class T>
bool operator!=(IntrusivePtr<T> const& iPtr1, T* p) {
    return (iPtr1.Get() != p);
}

template <class T>
bool operator==(T* p, IntrusivePtr<T> const& iPtr2) {
    return (p == iPtr2.Get());
}

template <class T>
bool operator!=(T* p, IntrusivePtr<T> const& iPtr2) {
    return (p != iPtr2.Get());
}

}  // namespace radray
