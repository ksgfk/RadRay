#pragma once

#include <optional>
#include <string_view>

#include <radray/types.h>

namespace radray {

std::optional<wstring> ToWideChar(std::string_view str) noexcept;

std::optional<string> ToMultiByte(std::wstring_view str) noexcept;

}  // namespace radray
