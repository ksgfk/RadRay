#include <radray/runtime/material.h>

#include <radray/logger.h>

namespace radray {

std::optional<Material> Material::Create(const MaterialResource& resource, std::span<const MaterialParameter> parameters) {
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (parameters[index].Name.empty()) {
            RADRAY_ERR_LOG("Material parameter name is empty");
            return std::nullopt;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (parameters[previous].Name == parameters[index].Name) {
                RADRAY_ERR_LOG("Duplicate material parameter: {}", parameters[index].Name);
                return std::nullopt;
            }
        }
    }
    return Material{resource, parameters};
}

Material::Material(const MaterialResource& resource, std::span<const MaterialParameter> parameters)
    : _resource(&resource), _parameters(parameters.begin(), parameters.end()) {}

const MaterialResource& Material::GetResource() const noexcept { return *_resource; }

std::span<const MaterialParameter> Material::GetParameters() const noexcept { return _parameters; }

std::optional<size_t> Material::FindParameter(std::string_view name) const noexcept {
    for (size_t index = 0; index < _parameters.size(); ++index) {
        if (_parameters[index].Name == name) return index;
    }
    return std::nullopt;
}

Nullable<const MaterialParameterValue*> Material::GetDefaultParameter(size_t index) const noexcept {
    return index < _parameters.size() ? &_parameters[index].DefaultValue : nullptr;
}

}  // namespace radray
