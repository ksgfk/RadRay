#pragma once

#include <string_view>

#include <radray/nullable.h>
#include <radray/types.h>

namespace radray {

// docs/architecture/render-framework.md

template <class... T>
struct TypeList {};

/// Required/Optional dependencies initialize before their consumer and shut down after it.
template <class T>
struct Required {};

template <class T>
struct Optional {};

/// A reference-only dependency. The object must exist, but need not be initialized yet.
template <class T>
struct Link {};

template <class T>
struct OptionalLink {};

/// A fixed, nullable instance slot. Its presence cannot change during a registry's lifetime.
template <class T>
struct OptionalService {};

enum class ServiceError {
    None,
    InvalidState,
    InitializationFailed,
};

struct ServiceStatus {
    ServiceError Code{ServiceError::None};
    string Message{};
    std::string_view Service{};

    explicit operator bool() const noexcept { return Code == ServiceError::None; }

    static ServiceStatus Failure(string message) {
        return {ServiceError::InitializationFailed, std::move(message), {}};
    }
};

/// Specializations may declare Provides, Dependencies and a static Name with static storage.
/// Inject(T&, dependency arguments...) and Unwire(T&) must return void and be noexcept.
/// Initialize(T&) returns ServiceStatus; its paired Shutdown(T&) must be noexcept and
/// accept partial initialization. Hooks are explicit; service member names are not inspected.
template <class T>
struct ServiceTraits {
    using Provides = TypeList<>;
    using Dependencies = TypeList<>;
};

}  // namespace radray
