#include <radray/render/msl.h>

#include <radray/enum_flags.h>
#include <radray/utility.h>

namespace radray::render {

std::string_view format_as(MslDataType v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(MslArgumentType v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(MslAccess v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(MslTextureType v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(MslStage v) noexcept {
    return EnumNameOr(v);
}

}  // namespace radray::render
