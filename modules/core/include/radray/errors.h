#pragma once

#include <string_view>

namespace radray {

enum class Errors {
    InvalidArgument,
    InvalidOperation,
    IndexOutOfRange,
    ArgumentOutOfRange,
    UnsupportedPlatform,
    OutOfMemory,
    COMException,

    D3D12,
    VK,
    METAL,
    LIBPNG,
    RADRAYIMGUI,
    DXC,
    WIN,
};

std::string_view format_as(Errors v) noexcept;

}  // namespace radray
