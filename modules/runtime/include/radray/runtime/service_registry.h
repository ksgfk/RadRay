#pragma once

#include <tuple>
#include <type_traits>

#include <radray/runtime_type.h>
#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

// ════════════════════════════════════════════════════════════════
//  非侵入式、无单例、trait 驱动的分阶段服务装配。
//
//  - 不入侵类内:依赖声明放在类外的 ServiceTraits<T> 特化里,业务类零改动,
//    仅复用其已有的 public setter。
//  - 无单例:ServiceRegistry 是调用方持有的局部对象,显式传递,无 thread_local。
//  - 解环:三阶段(实例化 → 装配 → 初始化)。装配发生时全部实例已存在,
//    互相持有引用(如 WindowManager <-> GpuSystem)天然可解。
//
//  典型用法:
//      // 类外声明"我要谁"(用类已有的 public setter):
//      template <> struct ServiceTraits<GpuSystem> {
//          static constexpr auto Inject = std::tuple{&GpuSystem::SetWindowManager};
//      };
//      template <> struct ServiceTraits<AssetManager> {
//          static constexpr auto Inject = std::tuple{&AssetManager::SetGpuSystem};
//      };
//      // setter 形参是基类、要 resolve 的是派生具体类型时,用 As<Source>。
//
//      ServiceRegistry reg;          // 局部对象,无单例
//      reg.Add(wm); reg.Add(gpu); reg.Add(asset);  // phase 1 实例已建,登记
//      reg.Wire();                                  // phase 2 装配交叉引用
//      reg.Initialize();                            // phase 3 初始化数据(可选 OnInitialize 钩子)
// ════════════════════════════════════════════════════════════════

/// 类外特化点。默认无依赖。使用方按 `template <> struct ServiceTraits<T> { ... }` 声明。
template <class T>
struct ServiceTraits {
    static constexpr auto Inject = std::tuple{};
};

/// setter 形参类型 != 要 resolve 的具体类型时(派生 -> 基类上行转换),
/// 用 As<Source> 显式指定 resolve 的来源具体类型。
template <class Source, class M>
struct ServiceInjectAs {
    M Setter;
};

template <class Source, class M>
constexpr ServiceInjectAs<Source, M> As(M setter) noexcept {
    return ServiceInjectAs<Source, M>{setter};
}

namespace detail {

// 从 setter 成员函数指针里抽出形参类型(同时覆盖 noexcept 与非 noexcept 签名)。
template <class C, class A>
A ServiceSetterArg(void (C::*)(A));
template <class C, class A>
A ServiceSetterArg(void (C::*)(A) noexcept);

// 使用项目统一的运行时类型 id 做服务 key。所有进入 registry 的类型都必须有固定 id。
template <class T>
constexpr RuntimeTypeId ServiceTypeKey() noexcept {
    return runtime_type_id_v<T>;
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
    template <class T>
    void Add(T* service) {
        if (service == nullptr) {
            RADRAY_ABORT("ServiceRegistry::Add received null service pointer");
            return;
        }
        Entry e{};
        e.Key = detail::ServiceTypeKey<T>();
        e.Ptr = service;
        e.WireFn = &ServiceRegistry::WireService<T>;
        if constexpr (requires(T* p) { p->OnInitialize(); }) {
            e.InitFn = [](void* p) { static_cast<T*>(p)->OnInitialize(); };
        } else {
            e.InitFn = nullptr;
        }
        _entries.emplace_back(e);
    }

    /// 按具体类型取已登记服务。未登记返回 nullptr。
    template <class T>
    T* Resolve() const noexcept {
        const RuntimeTypeId key = detail::ServiceTypeKey<T>();
        for (const Entry& e : _entries) {
            if (e.Key == key) {
                return static_cast<T*>(e.Ptr);
            }
        }
        return nullptr;
    }

    /// phase 2:按各服务的 ServiceTraits<T>::Inject 装配交叉引用。
    /// 此刻全部实例已登记,互相持有引用(环)均可解析。
    void Wire() {
        for (Entry& e : _entries) {
            e.WireFn(*this, e.Ptr);
        }
    }

    /// phase 3:按登记(拓扑)序调用各服务可选的 OnInitialize() 钩子。
    void Initialize() {
        for (Entry& e : _entries) {
            if (e.InitFn != nullptr) {
                e.InitFn(e.Ptr);
            }
        }
    }

private:
    struct Entry {
        RuntimeTypeId Key{};
        void* Ptr{nullptr};
        void (*WireFn)(ServiceRegistry&, void*){nullptr};
        void (*InitFn)(void*){nullptr};
    };

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
        using Service = std::remove_pointer_t<Arg>;
        Service* dep = reg.Resolve<Service>();
        if (dep == nullptr) {
            RADRAY_ABORT("ServiceRegistry::Wire: required dependency not registered (check ServiceTraits Inject and Add order)");
            return;
        }
        (self.*setter)(dep);
    }

    // As<Source> 绑定:resolve Source,隐式上行转换喂给 setter。
    template <class T, class Source, class M>
    static void ApplyBinding(ServiceRegistry& reg, T& self, ServiceInjectAs<Source, M> binding) {
        Source* dep = reg.Resolve<Source>();
        if (dep == nullptr) {
            RADRAY_ABORT("ServiceRegistry::Wire: required source service not registered (check ServiceTraits As<Source> and Add order)");
            return;
        }
        (self.*(binding.Setter))(dep);
    }

    vector<Entry> _entries;
};

}  // namespace radray
