#pragma once

#include <utility>
#include <vector>
#include <memory>
#include <algorithm>
#include <functional>

#include <radray/types.h>

namespace radray {

template <typename T>
class MultiDelegate;
template <typename Return, typename... Args>
class MultiDelegate<Return(Args...)> {
    static_assert(std::is_void_v<Return>, "return type must be void");
};
template <typename T>
class DelegateHandle;
template <typename Return, typename... Args>
class DelegateHandle<Return(Args...)> {
    static_assert(std::is_void_v<Return>, "return type must be void");
};

template <typename... Args>
class DelegateHandle<void(Args...)> {
public:
    using CallableType = void(Args...);

    DelegateHandle() noexcept = default;
    DelegateHandle(
        std::function<CallableType>&& func,
        std::shared_ptr<MultiDelegate<CallableType>> md) noexcept
        : _func(std::make_shared<std::function<CallableType>>(std::move(func))),
          _md(std::weak_ptr<MultiDelegate<CallableType>>{md}) {
        md->Add(_func);
    }
    DelegateHandle(const DelegateHandle& other) noexcept = default;
    DelegateHandle(DelegateHandle&& other) noexcept {
        if (other.IsEmpty()) {
            Destroy();
        }
        _func = std::move(other._func);
        _md = std::move(other._md);
    }
    DelegateHandle& operator=(const DelegateHandle& other) noexcept = default;
    DelegateHandle& operator=(DelegateHandle&& other) noexcept {
        if (other.IsEmpty()) {
            Destroy();
        }
        _func = std::move(other._func);
        _md = std::move(other._md);
        return *this;
    }
    ~DelegateHandle() noexcept { Destroy(); }

    bool IsEmpty() const noexcept { return !_func && _md.expired(); }

    void Destroy() noexcept {
        if (!_md.expired() && _func.use_count() == 1) {
            auto md = _md.lock();
            md->Remove(_func);
        }
        _func.reset();
        _md.reset();
    }

private:
    std::shared_ptr<std::function<CallableType>> _func;
    std::weak_ptr<MultiDelegate<CallableType>> _md;
};

template <typename... Args>
class MultiDelegate<void(Args...)> {
public:
    using CallableType = void(Args...);
    using DataType = std::function<CallableType>;

    void Add(const std::shared_ptr<DataType>& data) noexcept {
        _list.emplace_back(std::weak_ptr<DataType>{data});
    }

    void Remove(const std::shared_ptr<DataType>& data) noexcept {
        _list.erase(std::remove_if(_list.begin(), _list.end(), [&](auto&& t) { return t.lock() == data; }), _list.end());
    }

    void Invoke(Args&&... args) {
        std::vector<std::weak_ptr<DataType>> temp{_list.begin(), _list.end()};
        for (auto&& i : temp) {
            std::shared_ptr<DataType> t = i.lock();
            t->operator()(std::forward<Args>(args)...);
        }
    }

    void operator()(Args&&... args) { return Invoke(std::forward<Args>(args)...); }

private:
    std::vector<std::weak_ptr<DataType>> _list;
};

}  // namespace radray
