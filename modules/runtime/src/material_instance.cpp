#include <radray/runtime/material_instance.h>

namespace radray {

MaterialInstance::MaterialInstance(const Material& material)
    : _material(&material), _overrides(material.GetParameters().size()) {}

const Material& MaterialInstance::GetMaterial() const noexcept { return *_material; }

Nullable<const MaterialParameterValue*> MaterialInstance::GetParameter(size_t index) const noexcept {
    if (index >= _overrides.size()) return nullptr;
    if (_overrides[index]) return &*_overrides[index];
    return _material->GetDefaultParameter(index);
}

bool MaterialInstance::SetParameter(size_t index, MaterialParameterValue value) {
    if (index >= _overrides.size()) return false;
    const auto declared = _material->GetDefaultParameter(index);
    if (!declared || declared->index() != value.index()) return false;
    _overrides[index] = std::move(value);
    return true;
}

bool MaterialInstance::ResetParameter(size_t index) noexcept {
    if (index >= _overrides.size()) return false;
    _overrides[index].reset();
    return true;
}

}  // namespace radray
