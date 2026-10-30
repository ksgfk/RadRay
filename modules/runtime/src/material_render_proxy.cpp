#include <radray/runtime/material_render_proxy.h>

namespace radray {

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
