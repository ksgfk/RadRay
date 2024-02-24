#pragma once

#include <utility>
#include <vector>
#include <memory>
#include <algorithm>
#include <functional>

#include <radray/types.h>

namespace radray {

template <typename T>
class Delegate;
template <typename Return, typename... Args>
class Delegate<Return(Args...)> {
public:
    using FunctionType = Return(Args...);

    virtual ~Delegate() noexcept = default;

    virtual Return Invoke(Args... args) const = 0;

    Return operator()(Args... args) const { return this->Invoke(std::forward<Args>(args)...); }
};

template <typename T>
class FunctionDelegate {};
template <typename Return, typename... Args>
class FunctionDelegate<Return(Args...)> : public Delegate<Return(Args...)> {
public:
    explicit FunctionDelegate(std::function<Return(Args...)>&& f) : _f(std::move(f)) {}
    template <typename Functor>
    explicit FunctionDelegate(Functor&& functor) : _f(std::forward<Functor>(functor)) {}
    ~FunctionDelegate() noexcept override = default;

    Return Invoke(Args... args) const override { return _f(std::forward<Args>(args)...); }

private:
    std::function<Return(Args...)> _f;
};
template <typename Functor, size_t... Index>
auto MakeFuncDelegateHelper(Functor&& functor, std::index_sequence<Index...>) {
    using Trait = CallableTrait<Functor>;
    using Return = typename Trait::ReturnType;
    return std::make_shared<FunctionDelegate<Return(typename Trait::template Argument<Index>::type...)>>(std::forward<Functor>(functor));
}
template <typename Functor>
auto MakeFuncDelegate(Functor&& functor) {
    using Trait = CallableTrait<Functor>;
    return MakeFuncDelegateHelper(std::forward<Functor>(functor), std::make_index_sequence<Trait::ArgumentCount>());
}

template <typename T>
class MultiDelegate;
template <typename Return, typename... Args>
class MultiDelegate<Return(Args...)> {
    static_assert(std::is_void_v<Return>, "return type must be void");
};
template <typename... Args>
class MultiDelegate<void(Args...)> {
public:
    using DelegateType = Delegate<void(Args...)>;

    void Add(std::weak_ptr<DelegateType> delegate) {
        ClearExpired();
        _list.emplace_back(delegate);
    }

    void Remove(std::weak_ptr<DelegateType> delegate) {
        std::shared_ptr<DelegateType> l = delegate.lock();
        auto f = [&](const std::weak_ptr<DelegateType>& t) {
            if (t.expired()) {
                return true;
            }
            std::shared_ptr<DelegateType> r = t.lock();
            return l == r;
        };
        _list.erase(std::remove_if(_list.begin(), _list.end(), f), _list.end());
    }

    void ClearExpired() {
        _list.erase(std::remove_if(_list.begin(), _list.end(), [](auto&& t) { return t.expired(); }), _list.end());
    }

    void operator()(Args&&... args) {
        ClearExpired();
        for (const std::weak_ptr<DelegateType>& i : _list) {
            std::shared_ptr<DelegateType> t = i.lock();
            t->Invoke(std::forward<Args>(args)...);
        }
    }

private:
    std::vector<std::weak_ptr<DelegateType>> _list;
};

}  // namespace radray
