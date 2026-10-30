#pragma once

#include <span>
#include <variant>

#include <radray/runtime/material_resource.h>

namespace radray {

// 资源参数借用 RHI 对象；空指针表示尚未赋值。
using MaterialParameterValue = std::variant<float, array<float, 4>, Nullable<render::TextureView*>, Nullable<render::Sampler*>>;

struct MaterialParameter {
    string Name;
    MaterialParameterValue DefaultValue;
};

// 参数名称、类型、顺序与默认值在创建后保持不变。
// 借用 Resource；它必须晚于本定义及由其生成的所有 Proxy 销毁。
// 有 MaterialInstance 借用本定义时，不得搬移、替换或销毁它。
class Material {
public:
    // 拒绝空名称和重名；不编译 shader，不校验调用方的 shader 参数 ABI。
    static std::optional<Material> Create(const MaterialResource& resource, std::span<const MaterialParameter> parameters);
    const MaterialResource& GetResource() const noexcept;
    std::span<const MaterialParameter> GetParameters() const noexcept;
    // 索引属于本定义的声明顺序，不能跨 Material 假设其含义一致。
    std::optional<size_t> FindParameter(std::string_view name) const noexcept;
    Nullable<const MaterialParameterValue*> GetDefaultParameter(size_t index) const noexcept;

private:
    Material(const MaterialResource& resource, std::span<const MaterialParameter> parameters);
    const MaterialResource* _resource;
    vector<MaterialParameter> _parameters;
};

}  // namespace radray
