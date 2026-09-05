#pragma once

#include <concepts>
#include <tuple>
#include <type_traits>

#include <radray/runtime/service_traits.h>
#include <radray/scope_guard.h>

namespace radray {

// docs/architecture/render-framework.md

namespace detail {

template <class T>
struct ServiceSlot {
    using Type = T;
    using Argument = T&;
    using Storage = T*;
    static constexpr bool kOptional = false;

    static Storage Store(Argument value) noexcept { return &value; }
};

template <class T>
struct ServiceSlot<OptionalService<T>> {
    using Type = T;
    using Argument = Nullable<T*>;
    using Storage = Nullable<T*>;
    static constexpr bool kOptional = true;

    static Storage Store(Argument value) noexcept { return value; }
};

template <class T, class = void>
struct ServiceProvides {
    using Type = TypeList<>;
};

template <class T>
struct ServiceProvides<T, std::void_t<typename ServiceTraits<T>::Provides>> {
    using Type = typename ServiceTraits<T>::Provides;
};

template <class T, class = void>
struct ServiceDependencies {
    using Type = TypeList<>;
};

template <class T>
struct ServiceDependencies<T, std::void_t<typename ServiceTraits<T>::Dependencies>> {
    using Type = typename ServiceTraits<T>::Dependencies;
};

template <class T>
inline constexpr bool kIsServiceTypeList = false;

template <class... T>
inline constexpr bool kIsServiceTypeList<TypeList<T...>> = true;

template <class T, class... U>
constexpr size_t CountServiceType(TypeList<U...>) noexcept {
    return (size_t{0} + ... + size_t{std::is_same_v<T, U>});
}

template <class T, bool IsOptional, bool IsOrdered>
struct ServiceDependency {
    using Type = T;
    using Argument = std::conditional_t<IsOptional, Nullable<T*>, std::add_lvalue_reference_t<T>>;
    static constexpr bool kOptional = IsOptional;
    static constexpr bool kOrdered = IsOrdered;
    static constexpr bool kValid = std::is_class_v<T> && !std::is_volatile_v<T>;
};

template <class T>
struct ServiceDependencyInfo {
    static constexpr bool kValid = false;
};

template <class T>
struct ServiceDependencyInfo<Required<T>> : ServiceDependency<T, false, true> {};

template <class T>
struct ServiceDependencyInfo<Optional<T>> : ServiceDependency<T, true, true> {};

template <class T>
struct ServiceDependencyInfo<Link<T>> : ServiceDependency<T, false, false> {};

template <class T>
struct ServiceDependencyInfo<OptionalLink<T>> : ServiceDependency<T, true, false> {};

template <class T>
inline constexpr bool kHasServiceInitialize = requires(T& self) { ServiceTraits<T>::Initialize(self); } || requires { &ServiceTraits<T>::Initialize; };

template <class T>
inline constexpr bool kHasServiceShutdown = requires(T& self) { ServiceTraits<T>::Shutdown(self); } || requires { &ServiceTraits<T>::Shutdown; };

template <class T>
inline constexpr bool kHasServiceUnwire = requires(T& self) { ServiceTraits<T>::Unwire(self); } || requires { &ServiceTraits<T>::Unwire; };

template <class... Entries>
struct StaticServicePlan {
    static constexpr size_t kCount = sizeof...(Entries);
    using Slots = std::tuple<ServiceSlot<Entries>...>;

    template <size_t I>
    using Slot = std::tuple_element_t<I, Slots>;

    template <size_t I>
    using Type = typename Slot<I>::Type;

    template <class Key, class T>
    static consteval bool Provides() {
        using Exports = typename ServiceProvides<T>::Type;
        if constexpr (kIsServiceTypeList<Exports>) {
            return std::is_same_v<std::remove_const_t<Key>, T> || CountServiceType<std::remove_const_t<Key>>(Exports{}) != 0;
        } else {
            return false;
        }
    }

    template <class Key>
    static consteval size_t ProviderCount() {
        return (size_t{0} + ... + size_t{Provides<Key, typename ServiceSlot<Entries>::Type>()});
    }

    template <class Key>
    static consteval size_t ProviderIndex() {
        constexpr array<bool, kCount> matches{Provides<Key, typename ServiceSlot<Entries>::Type>()...};
        for (size_t i = 0; i < kCount; ++i) {
            if (matches[i]) return i;
        }
        return kCount;
    }

    template <class T, class... Interfaces>
    static consteval bool CheckExports(TypeList<Interfaces...>) {
        return (std::is_class_v<Interfaces> && ...) &&
               ((!std::is_const_v<Interfaces> && !std::is_volatile_v<Interfaces>) && ...) &&
               (std::is_convertible_v<T*, Interfaces*> && ...) &&
               ((!std::is_same_v<T, Interfaces>) && ...) &&
               ((ProviderCount<Interfaces>() == 1) && ...);
    }

    template <class... Interfaces>
    static consteval bool UniqueExports(TypeList<Interfaces...>) {
        return ((CountServiceType<Interfaces>(TypeList<Interfaces...>{}) == 1) && ...);
    }

    template <class T>
    static consteval bool CheckBindings() {
        using Exports = typename ServiceProvides<T>::Type;
        if constexpr (!std::is_class_v<T> || std::is_const_v<T> || std::is_volatile_v<T> || !kIsServiceTypeList<Exports>) {
            return false;
        } else {
            return ProviderCount<T>() == 1 && CheckExports<T>(Exports{}) && UniqueExports(Exports{});
        }
    }

    template <class Dependency>
    static consteval bool CheckDependency() {
        using Info = ServiceDependencyInfo<Dependency>;
        if constexpr (!Info::kValid) {
            return false;
        } else {
            constexpr size_t count = ProviderCount<typename Info::Type>();
            if constexpr (count > 1)
                return false;
            else if constexpr (count == 0)
                return Info::kOptional;
            else
                return Info::kOptional || !Slot<ProviderIndex<typename Info::Type>()>::kOptional;
        }
    }

    template <class T, class... Dependencies>
    static consteval bool CheckInjection(TypeList<Dependencies...>) {
        if constexpr (!(ServiceDependencyInfo<Dependencies>::kValid && ...)) {
            return false;
        } else if constexpr (sizeof...(Dependencies) == 0 && !requires(T& self) { ServiceTraits<T>::Inject(self); } && !requires { &ServiceTraits<T>::Inject; }) {
            return true;
        } else {
            return requires {
                static_cast<void (*)(T&, typename ServiceDependencyInfo<Dependencies>::Argument...) noexcept>(&ServiceTraits<T>::Inject);
            };
        }
    }

    template <class... Dependencies>
    static consteval bool CheckDependencies(TypeList<Dependencies...>) {
        return (CheckDependency<Dependencies>() && ...);
    }

    template <class T>
    static consteval bool CheckService() {
        using Dependencies = typename ServiceDependencies<T>::Type;
        if constexpr (!kIsServiceTypeList<Dependencies>) {
            return false;
        } else {
            bool valid = CheckDependencies(Dependencies{}) && CheckInjection<T>(Dependencies{});
            if constexpr (kHasServiceInitialize<T>) {
                valid = valid && kHasServiceShutdown<T> && requires {
                    static_cast<ServiceStatus (*)(T&)>(&ServiceTraits<T>::Initialize);
                };
            }
            if constexpr (kHasServiceShutdown<T>) {
                valid = valid && requires {
                    static_cast<void (*)(T&) noexcept>(&ServiceTraits<T>::Shutdown);
                };
            }
            if constexpr (kHasServiceUnwire<T>) {
                valid = valid && requires {
                    static_cast<void (*)(T&) noexcept>(&ServiceTraits<T>::Unwire);
                };
            }
            return valid;
        }
    }

    static constexpr bool kBindingsValid = (CheckBindings<typename ServiceSlot<Entries>::Type>() && ...);
    static constexpr bool kServicesValid = (CheckService<typename ServiceSlot<Entries>::Type>() && ...);

    struct Schedule {
        array<size_t, kCount> Order{};
        bool Acyclic{true};
    };

    template <size_t Consumer, class Dependency>
    static constexpr void AddEdge(array<array<bool, kCount>, kCount>& edges) {
        using Info = ServiceDependencyInfo<Dependency>;
        constexpr size_t provider = ProviderIndex<typename Info::Type>();
        if constexpr (Info::kOrdered && provider < kCount) edges[Consumer][provider] = true;
    }

    template <size_t Consumer, class... Dependencies>
    static constexpr void AddEdges(array<array<bool, kCount>, kCount>& edges, TypeList<Dependencies...>) {
        (AddEdge<Consumer, Dependencies>(edges), ...);
    }

    static consteval Schedule MakeSchedule() {
        Schedule result{};
        if constexpr (kBindingsValid && kServicesValid) {
            array<array<bool, kCount>, kCount> edges{};
            [&]<size_t... I>(std::index_sequence<I...>) {
                (AddEdges<I>(edges, typename ServiceDependencies<Type<I>>::Type{}), ...);
            }(std::make_index_sequence<kCount>{});
            array<bool, kCount> emitted{};
            for (size_t rank = 0; rank < kCount; ++rank) {
                size_t next = kCount;
                for (size_t candidate = 0; candidate < kCount; ++candidate) {
                    if (emitted[candidate]) continue;
                    bool ready = true;
                    for (size_t dependency = 0; dependency < kCount; ++dependency) {
                        if (edges[candidate][dependency] && !emitted[dependency]) ready = false;
                    }
                    if (ready) {
                        next = candidate;
                        break;
                    }
                }
                if (next == kCount) {
                    result.Acyclic = false;
                    return result;
                }
                result.Order[rank] = next;
                emitted[next] = true;
            }
        }
        return result;
    }

    static constexpr Schedule kSchedule = MakeSchedule();
    static constexpr bool kValid = kBindingsValid && kServicesValid && kSchedule.Acyclic;
};

}  // namespace detail

enum class ServiceRegistryState {
    Ready,
    Initializing,
    Running,
    ShuttingDown,
    Stopped,
    Failed,
};

/// Allows a composition to be checked without instantiating an invalid registry.
template <class... Entries>
inline constexpr bool kValidServiceRegistry = detail::StaticServicePlan<Entries...>::kValid;

/// A non-owning, statically compiled service composition. All bound objects must remain
/// alive through Initialize/Shutdown. Lifecycle calls are externally serialized.
/// Destruction does not call hooks; the owner chooses when to call Shutdown.
template <class... Entries>
class ServiceRegistry {
    using Plan = detail::StaticServicePlan<Entries...>;
    static_assert(Plan::kBindingsValid, "ServiceRegistry: duplicate/invalid service or Provides binding.");
    static_assert(Plan::kServicesValid, "ServiceRegistry: missing/optional required provider or invalid ServiceTraits hook signature (Initialize requires Shutdown).");
    static_assert(Plan::kSchedule.Acyclic, "ServiceRegistry: lifecycle dependency cycle; Link expresses reference-only dependencies.");

public:
    static constexpr size_t kServiceCount = sizeof...(Entries);
    static constexpr auto kInitializationOrder = Plan::kSchedule.Order;

    explicit ServiceRegistry(typename detail::ServiceSlot<Entries>::Argument... instances) noexcept
        : _instances(detail::ServiceSlot<Entries>::Store(instances)...) {}

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry(ServiceRegistry&&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(ServiceRegistry&&) = delete;
    ~ServiceRegistry() noexcept = default;

    /// Exact static lookup; absent keys and empty optional slots return an empty Nullable.
    template <class T>
    Nullable<T*> Resolve() const noexcept {
        static_assert(std::is_class_v<T> && !std::is_volatile_v<T>);
        constexpr size_t index = Plan::template ProviderIndex<T>();
        if constexpr (index == kServiceCount) {
            return nullptr;
        } else if constexpr (Plan::template Slot<index>::kOptional) {
            return static_cast<T*>(std::get<index>(_instances).Get());
        } else {
            return static_cast<T*>(std::get<index>(_instances));
        }
    }

    /// A required lookup. Its provider must be present and non-optional at compile time.
    template <class T>
    T& Get() const noexcept {
        constexpr size_t index = Plan::template ProviderIndex<T>();
        static_assert(index < kServiceCount, "ServiceRegistry::Get: service is not provided.");
        if constexpr (index < kServiceCount) {
            static_assert(!Plan::template Slot<index>::kOptional, "ServiceRegistry::Get: optional slots require Resolve.");
            return static_cast<T&>(*std::get<index>(_instances));
        }
    }

    /// Wires every present instance, then directly calls traits in the compiled order.
    /// Failure cleans the current partial initialization, preceding services and wiring.
    [[nodiscard]] ServiceStatus Initialize() {
        if (_state != ServiceRegistryState::Ready) {
            return {ServiceError::InvalidState, "service registry can only initialize once", {}};
        }
        _state = ServiceRegistryState::Initializing;
        auto rollback = MakeScopeGuard([this]() noexcept {
            Cleanup();
            _state = ServiceRegistryState::Failed;
        });
        [&]<size_t... I>(std::index_sequence<I...>) { (InjectOne<I>(), ...); }(std::make_index_sequence<kServiceCount>{});
        _wired = true;
        ServiceStatus result{};
        [&]<size_t... Rank>(std::index_sequence<Rank...>) {
            (void)(InitializeOne<Rank>(result) && ...);
        }(std::make_index_sequence<kServiceCount>{});
        if (!result) return result;
        _state = ServiceRegistryState::Running;
        rollback.Dismiss();
        return result;
    }

    /// Idempotent; false means a hook attempted to reenter an active lifecycle call.
    bool Shutdown() noexcept {
        if (_state == ServiceRegistryState::Initializing || _state == ServiceRegistryState::ShuttingDown) return false;
        if (_state == ServiceRegistryState::Stopped || _state == ServiceRegistryState::Failed) return true;
        _state = ServiceRegistryState::ShuttingDown;
        Cleanup();
        _state = ServiceRegistryState::Stopped;
        return true;
    }

    ServiceRegistryState GetState() const noexcept { return _state; }

private:
    template <class Dependency>
    decltype(auto) DependencyArgument() const noexcept {
        using Info = detail::ServiceDependencyInfo<Dependency>;
        if constexpr (Info::kOptional)
            return Resolve<typename Info::Type>();
        else
            return Get<typename Info::Type>();
    }

    template <class T, class... Dependencies>
    void Inject(T& self, TypeList<Dependencies...>) noexcept {
        if constexpr (requires { ServiceTraits<T>::Inject(self, DependencyArgument<Dependencies>()...); }) {
            ServiceTraits<T>::Inject(self, DependencyArgument<Dependencies>()...);
        }
    }

    template <size_t Index>
    void InjectOne() noexcept {
        using T = typename Plan::template Type<Index>;
        auto self = Resolve<T>();
        if constexpr (Plan::template Slot<Index>::kOptional) {
            if (!self) return;
        }
        Inject(*self.Get(), typename detail::ServiceDependencies<T>::Type{});
    }

    template <size_t Rank>
    bool InitializeOne(ServiceStatus& result) {
        using T = typename Plan::template Type<kInitializationOrder[Rank]>;
        _progress = Rank + 1;
        if constexpr (detail::kHasServiceInitialize<T>) {
            auto self = Resolve<T>();
            if constexpr (Plan::template Slot<kInitializationOrder[Rank]>::kOptional) {
                if (!self) return true;
            }
            result = ServiceTraits<T>::Initialize(*self.Get());
            if (!result) {
                if constexpr (requires { std::string_view{ServiceTraits<T>::Name}; }) result.Service = ServiceTraits<T>::Name;
                return false;
            }
        }
        return true;
    }

    template <size_t Rank>
    void ShutdownOne() noexcept {
        using T = typename Plan::template Type<kInitializationOrder[Rank]>;
        if constexpr (detail::kHasServiceShutdown<T>) {
            if (_progress > Rank) {
                auto self = Resolve<T>();
                if constexpr (Plan::template Slot<kInitializationOrder[Rank]>::kOptional) {
                    if (!self) return;
                }
                ServiceTraits<T>::Shutdown(*self.Get());
            }
        }
    }

    template <size_t Index>
    void UnwireOne() noexcept {
        using T = typename Plan::template Type<Index>;
        if constexpr (detail::kHasServiceUnwire<T>) {
            auto self = Resolve<T>();
            if constexpr (Plan::template Slot<Index>::kOptional) {
                if (!self) return;
            }
            ServiceTraits<T>::Unwire(*self.Get());
        }
    }

    void Cleanup() noexcept {
        [&]<size_t... I>(std::index_sequence<I...>) { (ShutdownOne<kServiceCount - 1 - I>(), ...); }(std::make_index_sequence<kServiceCount>{});
        _progress = 0;
        if (_wired) {
            [&]<size_t... I>(std::index_sequence<I...>) { (UnwireOne<kServiceCount - 1 - I>(), ...); }(std::make_index_sequence<kServiceCount>{});
            _wired = false;
        }
    }

    std::tuple<typename detail::ServiceSlot<Entries>::Storage...> _instances;
    size_t _progress{0};
    ServiceRegistryState _state{ServiceRegistryState::Ready};
    bool _wired{false};
};

template <class... T>
ServiceRegistry(T&...) -> ServiceRegistry<T...>;

}  // namespace radray
