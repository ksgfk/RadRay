#include <radray/errors.h>

#include <radray/utility.h>

namespace radray {

std::string_view format_as(Errors v) noexcept {
    switch (v) {
        case Errors::InvalidArgument: return "InvalidArgument";
        case Errors::InvalidOperation: return "InvalidOperation";
        case Errors::IndexOutOfRange: return "IndexOutOfRange";
        case Errors::ArgumentOutOfRange: return "ArgumentOutOfRange";
        case Errors::UnsupportedPlatform: return "UnsupportedPlatform";
        case Errors::OutOfMemory: return "OutOfMemory";
        case Errors::COMException: return "COMException";

        case Errors::D3D12: return "D3D12";
        case Errors::VK: return "VK";
        case Errors::METAL: return "METAL";
        case Errors::LIBPNG: return "libpng";
        case Errors::RADRAYIMGUI: return "radrayimgui";
        case Errors::DXC: return "dxc";
        case Errors::WIN: return "WINDOWS API";
    }
    Unreachable();
}

}  // namespace radray
