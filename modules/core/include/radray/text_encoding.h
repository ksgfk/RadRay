#pragma once

#include <optional>
#include <string_view>

#include <radray/types.h>

namespace radray {

namespace text_encoding {

std::optional<wstring> ToWideChar(std::string_view str) noexcept;

std::optional<string> ToMultiByte(std::wstring_view str) noexcept;

}  // namespace text_encoding

}  // namespace radray
