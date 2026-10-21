#pragma once

#include <radray/types.h>

namespace radray {

enum class RuntimeStartupStatus : uint8_t {
    NotAttempted,
    Started,
    InvalidDescriptor,
    BackendNotBuilt,
    NoAdapter,
    ValidationUnavailable,
    InitializationFailed,
};

struct RuntimeStartupResult {
    RuntimeStartupStatus Status{RuntimeStartupStatus::NotAttempted};
    string Reason;
};

}  // namespace radray
