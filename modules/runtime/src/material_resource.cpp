#include <radray/runtime/material_resource.h>

namespace radray {

std::optional<size_t> MaterialResource::FindProgram(std::string_view name) const noexcept {
    for (size_t index = 0; index < Programs.size(); ++index) {
        if (Programs[index].Name == name) return index;
    }
    return std::nullopt;
}

Nullable<const MaterialShaderProgram*> MaterialResource::GetProgram(size_t index) const noexcept {
    return index < Programs.size() ? &Programs[index] : nullptr;
}

}  // namespace radray
