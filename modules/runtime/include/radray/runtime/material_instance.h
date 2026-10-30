#pragma once

#include <radray/runtime/material.h>

namespace radray {

// 借用一份地址稳定、内容不变的 Material；不支持父实例链或静态 shader 变体。
class MaterialInstance {
public:
    explicit MaterialInstance(const Material& material);
    const Material& GetMaterial() const noexcept;
    Nullable<const MaterialParameterValue*> GetParameter(size_t index) const noexcept;
    // 拒绝未知索引或类型改变；失败时保留现有覆盖。
    bool SetParameter(size_t index, MaterialParameterValue value);
    bool ResetParameter(size_t index) noexcept;

private:
    const Material* _material;
    vector<std::optional<MaterialParameterValue>> _overrides;
};

}  // namespace radray
