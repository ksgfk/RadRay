#pragma once

#include <radray/gpu/types.h>
#include <radray/shader/shader_layout.h>
#include <radray/runtime/asset_handle.h>

namespace radray {

struct MaterialParameter;
class MaterialRenderProxy;
class ShaderAsset;

struct MaterialParameterResourceTarget {
    size_t ParameterIndex{0};
    shader::ShaderBindingReference Reference{};
    gpu::BindingKind Kind{gpu::BindingKind::Texture};
    bool ActiveOnDevice{false};
};

struct MaterialParameterConstantTarget {
    size_t ParameterIndex{0};
    uint32_t AbsoluteOffset{0};
    uint32_t Size{0};
};

class MaterialParameterBindingMap {
public:
    std::span<const MaterialParameterResourceTarget> GetResourceTargets() const noexcept;
    std::span<const MaterialParameterConstantTarget> GetConstantTargets() const noexcept;
    // 先验证全部目标再写；仅修改映射字节，不初始化块内其他字节。
    bool WriteConstants(const MaterialRenderProxy& proxy, std::span<byte> destination) const noexcept;
    // parameterSet 必须属于 shader 描述的对应组且可写；Set 失败后调用方重置或丢弃参数集。
    // 本接口不隐式 Reset、Flush，也不填充未映射的超集绑定。
    bool BindResources(const MaterialRenderProxy& proxy, uint32_t groupSlot, gpu::ShaderParameterSet& parameterSet) const noexcept;

private:
    friend class MaterialAsset;
    MaterialParameterBindingMap() = default;
    static std::optional<MaterialParameterBindingMap> Create(std::span<const MaterialParameter> parameters, const ShaderAsset& shader,
                                                             std::span<const string> subset, std::string_view constantBlockName);
    AssetHandle<MaterialAsset> _material;
    vector<uint32_t> _activeGroups;
    uint32_t _constantBlockSize{0};
    vector<MaterialParameterResourceTarget> _resources;
    vector<MaterialParameterConstantTarget> _constants;
};

}  // namespace radray
