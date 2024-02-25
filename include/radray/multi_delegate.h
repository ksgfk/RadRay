#pragma once

#include <utility>
#include <vector>
#include <memory>
#include <algorithm>
#include <functional>

#include <radray/types.h>

namespace radray {

template <typename Functor, size_t... Index>
auto MakePureFunctionDelegateImpl(Functor&& functor, std::index_sequence<Index...>) {
    using Trait = CallableTrait<Functor>;
    using Return = typename Trait::ReturnType;
    return std::make_shared<std::function<Return(typename Trait::template Parameter<Index>::type...)>>(std::forward<Functor>(functor));
}
template <typename Functor>
auto MakeDelegate(Functor&& functor) {
    constexpr auto isFunction = std::is_function_v<remove_all_cvref_t<decltype(functor)>>;
    if constexpr (isFunction) {
        using Original = std::add_pointer_t<remove_all_cvref_t<decltype(functor)>>;
        using Trait = CallableTrait<Original>;
        return MakePureFunctionDelegateImpl<Original>(std::forward<Original>(functor), std::make_index_sequence<Trait::ParameterCount>());
    } else {
        using Trait = CallableTrait<Functor>;
        constexpr auto isMember = Trait::IsMember;
        constexpr auto isFunctor = Trait::IsFunctor;
        static_assert(!isMember || isFunctor, "only functor or pure function can match this overload");
        return MakePureFunctionDelegateImpl(std::forward<Functor>(functor), std::make_index_sequence<Trait::ParameterCount>());
    }
}

template <typename Function, typename Instance, typename Return, typename... Args>
auto GenerateMemberFunctionCallImpl(Function&& func, Instance* ins) {
    if constexpr (std::is_void_v<Return>) {
        return [=](Args... args) {
            (ins->*func)(std::forward<Args>(args)...);
        };
    } else {
        return [=](Args... args) {
            return (ins->*func)(std::forward<Args>(args)...);
        };
    }
}
template <typename Function, typename Instance, size_t... Index>
auto MakeMemberFunctionDelegateImpl(Function&& func, Instance* ins, std::index_sequence<Index...>) {
    using Trait = CallableTrait<Function>;
    using Return = typename Trait::ReturnType;
    auto impl = GenerateMemberFunctionCallImpl<Function, Instance, Return, typename Trait::template Parameter<Index>::type...>(std::forward<Function>(func), ins);
    return std::make_shared<std::function<Return(typename Trait::template Parameter<Index>::type...)>>(std::move(impl));
}
template <typename Function, typename Instance>
auto MakeDelegate(Function&& func, Instance* ins) {
    using Trait = CallableTrait<Function>;
    static_assert(Trait::IsMember && !Trait::IsFunctor, "only member function can match this overload");
    return MakeMemberFunctionDelegateImpl(std::forward<Function>(func), ins, std::make_index_sequence<Trait::ParameterCount>());
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
    using DelegateType = std::function<void(Args...)>;

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
            t->operator()(std::forward<Args>(args)...);
        }
    }

private:
    std::vector<std::weak_ptr<DelegateType>> _list;
};

}  // namespace radray
