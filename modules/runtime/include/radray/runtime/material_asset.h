#pragma once

#include <optional>
#include <span>
#include <string_view>

#include <radray/basic_math.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/shader_parameters.h>
#include <radray/types.h>

namespace radray {

// Graphics-material wrapper over the generic, multi-group ShaderParameterSet.
// Pipeline-owned groups are removed by the policy before the layout is built.
class MaterialAsset final : public Asset {
public:
    MaterialAsset() noexcept = default;
    MaterialAsset(
        StreamingAssetRef<ShaderAsset> shaderRef,
        PipelineBindingPolicy policy = {}) noexcept;
    ~MaterialAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    StreamingAssetRef<ShaderAsset> GetShader() const noexcept { return _shader; }
    const PipelineBindingPolicy& GetBindingPolicy() const noexcept { return _policy; }
    bool SetShader(
        StreamingAssetRef<ShaderAsset> shaderRef,
        PipelineBindingPolicy policy = {}) noexcept;
    bool RefreshShaderLayout() noexcept;
    bool ApplyResolvedPrograms(
        std::span<const ShaderResolvedProgram> programs) noexcept;
    bool IsReady() const noexcept;
    bool HasCompleteParametersFor(
        const shader::ShaderInterfaceDesc& interface,
        const shader::ShaderDiagnosticContext& context = {}) const noexcept;
    const vector<ShaderBindingDiagnostic>& GetLayoutDiagnostics() const noexcept {
        return _layoutDiagnostics;
    }

    bool EnableLocalKeyword(std::string_view define) noexcept;
    bool DisableLocalKeyword(std::string_view define) noexcept;
    bool IsLocalKeywordEnabled(std::string_view define) const noexcept;
    const vector<string>& GetLocalKeywords() const noexcept { return _localKeywords; }

    std::optional<vector<string>> ResolveVariantDefines(
        uint32_t passIndex,
        std::span<const std::string_view> pipelineGlobalDefines = {}) const noexcept;

    bool SetFloat(std::string_view name, float value, uint32_t arrayIndex = 0) noexcept;
    bool SetFloat(
        ShaderParameterLocation location,
        std::string_view field,
        float value,
        uint32_t arrayIndex = 0) noexcept;
    bool SetInt(std::string_view name, int32_t value, uint32_t arrayIndex = 0) noexcept;
    bool SetInt(
        ShaderParameterLocation location,
        std::string_view field,
        int32_t value,
        uint32_t arrayIndex = 0) noexcept;
    bool SetUInt(std::string_view name, uint32_t value, uint32_t arrayIndex = 0) noexcept;
    bool SetUInt(
        ShaderParameterLocation location,
        std::string_view field,
        uint32_t value,
        uint32_t arrayIndex = 0) noexcept;
    bool SetBool(std::string_view name, bool value, uint32_t arrayIndex = 0) noexcept;
    bool SetBool(
        ShaderParameterLocation location,
        std::string_view field,
        bool value,
        uint32_t arrayIndex = 0) noexcept;
    bool SetVector(
        std::string_view name,
        const Eigen::Vector4f& value,
        uint32_t arrayIndex = 0) noexcept;
    bool SetVector(
        ShaderParameterLocation location,
        std::string_view field,
        const Eigen::Vector4f& value,
        uint32_t arrayIndex = 0) noexcept;
    bool SetValue(
        std::string_view name,
        shader::ShaderScalarType scalar,
        uint32_t columns,
        std::span<const byte> data,
        uint32_t arrayIndex = 0) noexcept;
    bool SetValue(
        ShaderParameterLocation location,
        std::string_view field,
        shader::ShaderScalarType scalar,
        uint32_t columns,
        std::span<const byte> data,
        uint32_t arrayIndex = 0) noexcept;
    bool SetMatrix(
        std::string_view name,
        std::span<const float> rowMajorValues,
        uint32_t rows,
        uint32_t columns,
        uint32_t arrayIndex = 0) noexcept;
    bool SetMatrix(
        ShaderParameterLocation location,
        std::string_view field,
        std::span<const float> rowMajorValues,
        uint32_t rows,
        uint32_t columns,
        uint32_t arrayIndex = 0) noexcept;
    bool SetConstantBuffer(std::string_view name, std::span<const byte> data) noexcept;
    bool SetConstantBuffer(
        ShaderParameterLocation location,
        std::span<const byte> data) noexcept;
    bool SetTexture(
        std::string_view name,
        StreamingAssetRef<TextureAsset> texture,
        const TextureSubViewDesc& view = {},
        uint32_t arrayIndex = 0) noexcept;
    bool SetTexture(
        ShaderParameterLocation location,
        StreamingAssetRef<TextureAsset> texture,
        const TextureSubViewDesc& view = {},
        uint32_t arrayIndex = 0) noexcept;
    bool SetSampler(
        std::string_view name,
        const render::SamplerDescriptor& sampler,
        uint32_t arrayIndex = 0) noexcept;
    bool SetSampler(
        ShaderParameterLocation location,
        const render::SamplerDescriptor& sampler,
        uint32_t arrayIndex = 0) noexcept;
    bool SetBuffer(
        std::string_view name,
        Nullable<render::Buffer*> buffer,
        render::BufferRange range = render::BufferRange::AllRange(),
        uint32_t arrayIndex = 0) noexcept;
    bool SetBuffer(
        ShaderParameterLocation location,
        Nullable<render::Buffer*> buffer,
        render::BufferRange range = render::BufferRange::AllRange(),
        uint32_t arrayIndex = 0) noexcept;
    bool ClearResource(std::string_view name, uint32_t arrayIndex = 0) noexcept;
    bool ClearResource(
        ShaderParameterLocation location,
        uint32_t arrayIndex = 0) noexcept;

    const ShaderParameterSet& GetParameters() const noexcept { return _parameters; }
    uint64_t GetRevision() const noexcept { return _revision; }

private:
    template <typename F>
    bool MutateParameters(F&& mutation) noexcept {
        if (!RefreshShaderLayout()) return false;
        const uint64_t before = _parameters.GetRevision();
        if (!mutation(_parameters)) return false;
        if (_parameters.GetRevision() != before) ++_revision;
        return true;
    }

    bool ApplyResolvedInterfaces(
        std::span<const ShaderProgramInterfaceRecord> interfaces) noexcept;
    bool InstallLayout(ShaderParameterLayoutBuildResult result, const ShaderAsset* source) noexcept;

    StreamingAssetRef<ShaderAsset> _shader;
    PipelineBindingPolicy _policy;
    ShaderParameterSet _parameters;
    vector<ShaderBindingDiagnostic> _layoutDiagnostics;
    vector<ShaderProgramInterfaceRecord> _resolvedInterfaces;
    unordered_map<uint32_t, shader::ShaderHash> _resolvedSourceIdentities;
    vector<string> _localKeywords;
    const ShaderAsset* _layoutShader{nullptr};
    bool _layoutInitialized{false};
    uint64_t _revision{1};
};

template <>
struct RuntimeTypeTrait<MaterialAsset> {
    static constexpr RuntimeTypeId value{0x955761f3, 0x9814, 0x4a79, 0xb0, 0x34, 0x3d, 0xbe, 0x18, 0xea, 0x73, 0xbd};
    using Bases = std::tuple<Asset>;
};

}  // namespace radray
