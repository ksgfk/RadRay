#pragma once

#include <tuple>
#include <typeindex>
#include <type_traits>

#include <radray/logger.h>
#include <radray/nullable.h>
#include <radray/types.h>

namespace radray {

// 非侵入式、无单例、trait 驱动的分阶段服务装配。设计与当前装配关系见
// docs/architecture/render-framework.md
//
//     template <> struct ServiceTraits<GpuSystem> {
//         static constexpr auto Inject = std::tuple{&GpuSystem::SetWindowManager};
//     };
//
//     ServiceRegistry reg;                        // 局部对象, 无单例
//     reg.Add(wm); reg.Add(gpu); reg.Add(asset);  // phase 1 登记
//     reg.Wire();                                 // phase 2 装配交叉引用
//     reg.Initialize();                           // phase 3 可选 OnInitialize 钩子
//
// Add<Interfaces...>(service) 只登记 service 的静态类型及显式列出的接口。接口指针在
// Add 的 typed 上下文完成调整；别名只参与 Resolve，不重复参与 Wire/Initialize。

/// 类外特化点。默认无依赖。使用方按 `template <> struct ServiceTraits<T> { ... }` 声明。
template <class T>
struct ServiceTraits {
    static constexpr auto Inject = std::tuple{};
};

namespace detail {

// 从 setter 成员函数指针里抽出形参类型(同时覆盖 noexcept 与非 noexcept 签名)。
template <class C, class A>
A ServiceSetterArg(void (C::*)(A));
template <class C, class A>
A ServiceSetterArg(void (C::*)(A) noexcept);

template <class T>
std::type_index ServiceTypeKey() noexcept {
    return std::type_index{typeid(std::remove_cv_t<T>)};
}

}  // namespace detail

class ServiceRegistry {
public:
    ServiceRegistry() noexcept = default;
    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry(ServiceRegistry&&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(ServiceRegistry&&) = delete;
    ~ServiceRegistry() noexcept = default;

    /// phase 1:登记一个已实例化的服务(非拥有)。所有权仍在调用方。
    ///
    /// 静态类型 T 自动登记；Interfaces 仅在这里显式列出时登记。
    /// T* 必须能公开、无歧义地转换为每个 Interfaces*。
    template <class... Interfaces, class T>
    void Add(T* service) {
        static_assert(std::is_class_v<T>, "ServiceRegistry services must be class types.");
        static_assert(!std::is_const_v<T>, "ServiceRegistry services must be mutable objects.");
        static_assert((std::is_class_v<Interfaces> && ...), "ServiceRegistry interfaces must be class types.");
        static_assert((!std::is_const_v<Interfaces> && ...), "ServiceRegistry interface registrations must be mutable types.");
        static_assert(
            (std::is_convertible_v<T*, Interfaces*> && ...),
            "ServiceRegistry interfaces must be public, unambiguous bases of the service type.");
        if (service == nullptr) {
            RADRAY_ABORT("ServiceRegistry::Add received null service pointer");
            return;
        }

        RegisterBinding<T>(service);
        (RegisterBinding<Interfaces>(static_cast<Interfaces*>(service)), ...);

        ServiceEntry e{};
        e.Ptr = service;
        e.WireFn = &ServiceRegistry::WireService<T>;
        if constexpr (requires(T* p) { p->OnInitialize(); }) {
            e.InitFn = [](void* p) { static_cast<T*>(p)->OnInitialize(); };
        } else {
            e.InitFn = nullptr;
        }
        _services.emplace_back(e);
    }

    /// 只按显式登记的类型键查询；未登记返回空 Nullable。
    template <class T>
    Nullable<T*> Resolve() const noexcept {
        static_assert(std::is_class_v<T>, "ServiceRegistry resolve targets must be class types.");
        const auto it = _bindings.find(detail::ServiceTypeKey<T>());
        if (it == _bindings.end()) {
            return nullptr;
        }
        return static_cast<T*>(it->second);
    }

    /// phase 2:按各服务的 ServiceTraits<T>::Inject 装配交叉引用。
    /// 此刻全部实例已登记,互相持有引用(环)均可解析。
    void Wire() {
        for (ServiceEntry& e : _services) {
            e.WireFn(*this, e.Ptr);
        }
    }

    /// phase 3:按登记(拓扑)序调用各服务可选的 OnInitialize() 钩子。
    void Initialize() {
        for (ServiceEntry& e : _services) {
            if (e.InitFn != nullptr) {
                e.InitFn(e.Ptr);
            }
        }
    }

private:
    struct ServiceEntry {
        void* Ptr{nullptr};
        void (*WireFn)(ServiceRegistry&, void*){nullptr};
        void (*InitFn)(void*){nullptr};
    };

    template <class T>
    void RegisterBinding(T* service) {
        const bool inserted = _bindings.emplace(detail::ServiceTypeKey<T>(), static_cast<void*>(service)).second;
        if (!inserted) {
            RADRAY_ABORT("ServiceRegistry::Add duplicate service type registration");
        }
    }

    template <class T>
    static void WireService(ServiceRegistry& reg, void* self) {
        std::apply(
            [&](auto... binding) { (ApplyBinding(reg, *static_cast<T*>(self), binding), ...); },
            ServiceTraits<T>::Inject);
    }

    // 普通 setter:按形参指针的 pointee 类型 resolve。
    template <class T, class M>
    requires std::is_member_function_pointer_v<M>
    static void ApplyBinding(ServiceRegistry& reg, T& self, M setter) {
        using Arg = decltype(detail::ServiceSetterArg(setter));
        static_assert(std::is_pointer_v<Arg>, "ServiceRegistry setters must accept one service pointer.");
        using Service = std::remove_pointer_t<Arg>;
        Nullable<Service*> dep = reg.Resolve<Service>();
        if (!dep) {
            RADRAY_ABORT("ServiceRegistry::Wire: required dependency not registered (check ServiceTraits Inject and Add order)");
            return;
        }
        (self.*setter)(dep.Get());
    }

    vector<ServiceEntry> _services;
    unordered_map<std::type_index, void*> _bindings;
};

}  // namespace radray
