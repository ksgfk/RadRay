#pragma once

#include <filesystem>
#include <radray/runtime/shader_layout.h>

namespace radray {

struct ShaderStageBytecode {
    uint32_t VariantIndex{0};
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};
    string Entry;
    vector<byte> Bytecode;
};

struct ShaderAssetDesc {
    ShaderFamilyLayoutDesc Family;
    vector<ShaderStageBytecode> Stages;
};

class ShaderAsset final {
public:
    static std::optional<ShaderAsset> Create(render::Device& device, const ShaderAssetDesc& desc);
    static std::optional<ShaderAsset> Load(render::Device& device, const std::filesystem::path& indexPath);
    ShaderAsset(ShaderAsset&&) noexcept = default;
    ShaderAsset& operator=(ShaderAsset&&) noexcept = default;
    ShaderAsset(const ShaderAsset&) = delete;
    ShaderAsset& operator=(const ShaderAsset&) = delete;

    const ShaderPipelineLayoutDesc& GetLayoutDesc() const noexcept;
    const ShaderFamilyLayoutDesc& GetFamilyDesc() const noexcept;
    render::RenderBackend GetBackend() const noexcept;
    Nullable<const ShaderTargetProgramInterfaceDesc*> GetVariantInterface(uint32_t variant) const noexcept;
    std::optional<render::ShaderEntry> GetStage(uint32_t variant, render::ShaderStage stage) const noexcept;
    std::optional<ShaderBindingLocation> FindBinding(std::string_view name) const noexcept;
    std::optional<ShaderPushConstantLocation> FindPushConstant(std::string_view name) const noexcept;
    std::optional<shader::ShaderResolvedMember> FindBufferMember(std::string_view block, std::string_view path) const;

private:
    ShaderAsset() = default;
    struct StageObject {
        uint32_t VariantIndex;
        render::ShaderStage Stage;
        string Entry;
        unique_ptr<render::Shader> Object;
    };
    ShaderFamilyLayoutDesc _family;
    render::RenderBackend _backend{render::RenderBackend::D3D12};
    vector<StageObject> _stages;
};

}  // namespace radray
