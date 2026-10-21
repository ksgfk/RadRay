#pragma once

#include <optional>
#include <string_view>

#include <radray/types.h>

namespace radray::shader {

// A failed operation has no value and supplies diagnostics. Successful operations may
// also supply diagnostics, for example compiler warnings.
template <typename T>
struct DiagnosticResult {
    std::optional<T> Value;
    string Diagnostics;

    bool Succeeded() const noexcept { return Value.has_value(); }
};

void AppendDiagnostic(string& diagnostics, std::string_view message);

}  // namespace radray::shader
