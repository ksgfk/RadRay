#include <radray/runtime/material.h>

#include <radray/logger.h>

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

void MaterialRenderProxy::Update(const Material& material) {
    vector<MaterialParameterValue> parameters;
    parameters.reserve(material.GetParameters().size());
    for (const auto& parameter : material.GetParameters()) parameters.push_back(parameter.DefaultValue);
    _parameters = std::move(parameters);
    _resource = &material.GetResource();
}

void MaterialRenderProxy::Update(const MaterialInstance& instance) {
    // 步骤 M3：解析实例覆盖与默认值，复制为 Scene 拥有的渲染侧参数快照。
    // 为什么：用户侧实例可继续编辑；绘制读取显式同步后的值，不保留源对象引用。
    // UE5 对照：Engine/Private/Materials/MaterialInstance.cpp 的 GetParameterValue 和 SetMIParameterValue 渲染命令。
    // 本例简化：同步 CPU 快照，单层继承；不建立渲染线程、UniformExpressionCache 或 GPU 参数缓冲。
    const auto& material = instance.GetMaterial();
    vector<MaterialParameterValue> parameters;
    parameters.reserve(material.GetParameters().size());
    for (size_t index = 0; index < material.GetParameters().size(); ++index) {
        parameters.push_back(*instance.GetParameter(index));
    }
    _parameters = std::move(parameters);
    _resource = &material.GetResource();
}

Nullable<const MaterialResource*> MaterialRenderProxy::GetResource() const noexcept { return _resource; }

Nullable<const MaterialParameterValue*> MaterialRenderProxy::GetParameter(size_t index) const noexcept {
    return index < _parameters.size() ? &_parameters[index] : nullptr;
}

}  // namespace radray
