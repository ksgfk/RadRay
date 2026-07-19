#pragma once

#include <optional>
#include <span>
#include <string_view>

#include <radray/basic_math.h>
#include <radray/render/common.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material_layout.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/texture_asset.h>
#include <radray/types.h>

namespace radray {

struct MaterialConstantBinding {
    string Name;
    uint32_t Binding{0};
    vector<byte> Data;
};

struct MaterialTextureBinding {
    string Name;
    uint32_t Binding{0};
    StreamingAssetRef<TextureAsset> Texture;
    TextureSubViewDesc View{};
    bool IsSet{false};
};

struct MaterialSamplerBinding {
    string Name;
    uint32_t Binding{0};
    std::optional<render::SamplerDescriptor> Sampler;
};

class MaterialParameterStorage {
public:
    bool Reset(const MaterialLayout& layout) noexcept;

    const MaterialLayout& GetLayout() const noexcept { return _layout; }
    uint64_t GetRevision() const noexcept { return _revision; }

    bool SetFloat(std::string_view name, float value) noexcept;
    bool SetVector(std::string_view name, const Eigen::Vector4f& value) noexcept;
    bool SetConstantBuffer(std::string_view name, std::span<const byte> data) noexcept;
    bool SetTexture(
        std::string_view name,
        StreamingAssetRef<TextureAsset> texture,
        const TextureSubViewDesc& view = {}) noexcept;
    bool ClearTexture(std::string_view name) noexcept;
    bool SetSampler(std::string_view name, const render::SamplerDescriptor& sampler) noexcept;
    bool ClearSampler(std::string_view name) noexcept;

    std::span<const MaterialConstantBinding> ConstantBindings() const noexcept { return _constants; }
    std::span<const MaterialTextureBinding> TextureBindings() const noexcept { return _textures; }
    std::span<const MaterialSamplerBinding> SamplerBindings() const noexcept { return _samplers; }

private:
    struct ConstantFieldTarget {
        MaterialConstantBinding* Block{nullptr};
        const MaterialFieldDesc* Field{nullptr};
    };

    ConstantFieldTarget FindConstantField(std::string_view name) noexcept;
    bool SetConstantField(std::string_view name, std::span<const byte> data) noexcept;

    MaterialLayout _layout;
    vector<MaterialConstantBinding> _constants;
    vector<MaterialTextureBinding> _textures;
    vector<MaterialSamplerBinding> _samplers;
    uint64_t _revision{0};
};

// The pipeline selects a shader binding group; the material owns the Local
// keywords and runtime data reflected from that group.
class MaterialAsset final : public Asset {
public:
    MaterialAsset() noexcept = default;
    MaterialAsset(StreamingAssetRef<ShaderAsset> shaderRef, uint32_t bindingGroup) noexcept;
    ~MaterialAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    StreamingAssetRef<ShaderAsset> GetShader() const noexcept { return _shader; }
    std::optional<uint32_t> GetBindingGroup() const noexcept { return _bindingGroup; }
    bool SetShader(StreamingAssetRef<ShaderAsset> shaderRef, uint32_t bindingGroup) noexcept;
    bool RefreshShaderLayout() noexcept;
    bool IsReady() const noexcept;

    bool EnableLocalKeyword(std::string_view define) noexcept;
    bool DisableLocalKeyword(std::string_view define) noexcept;
    bool IsLocalKeywordEnabled(std::string_view define) const noexcept;
    const vector<string>& GetLocalKeywords() const noexcept { return _localKeywords; }

    std::optional<vector<string>> ResolveVariantDefines(
        uint32_t passIndex,
        std::span<const std::string_view> pipelineGlobalDefines = {}) const noexcept;

    bool SetFloat(std::string_view name, float value) noexcept;
    bool SetVector(std::string_view name, const Eigen::Vector4f& value) noexcept;
    bool SetConstantBuffer(std::string_view name, std::span<const byte> data) noexcept;
    bool SetTexture(
        std::string_view name,
        StreamingAssetRef<TextureAsset> texture,
        const TextureSubViewDesc& view = {}) noexcept;
    bool ClearTexture(std::string_view name) noexcept;
    bool SetSampler(std::string_view name, const render::SamplerDescriptor& sampler) noexcept;
    bool ClearSampler(std::string_view name) noexcept;

    const MaterialParameterStorage& GetParameters() const noexcept { return _parameters; }
    uint64_t GetRevision() const noexcept { return _revision; }

private:
    template <typename F>
    bool MutateParameters(F&& mutation) noexcept {
        if (!RefreshShaderLayout()) {
            return false;
        }
        const uint64_t before = _parameters.GetRevision();
        if (!mutation(_parameters)) {
            return false;
        }
        if (_parameters.GetRevision() != before) {
            ++_revision;
        }
        return true;
    }

    StreamingAssetRef<ShaderAsset> _shader;
    std::optional<uint32_t> _bindingGroup;
    MaterialParameterStorage _parameters;
    vector<string> _localKeywords;
    uint64_t _revision{1};
};

template <>
struct RuntimeTypeTrait<MaterialAsset> {
    static constexpr RuntimeTypeId value{0x955761f3, 0x9814, 0x4a79, 0xb0, 0x34, 0x3d, 0xbe, 0x18, 0xea, 0x73, 0xbd};
    using Bases = std::tuple<Asset>;
};

}  // namespace radray
