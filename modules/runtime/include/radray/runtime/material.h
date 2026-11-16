#pragma once

// 材质所有权与 UE 对照见 docs/architecture/runtime.md。

#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/render/rhi.h>
#include <radray/runtime/material_tag.h>
#include <radray/runtime/vertex_input.h>
#include <radray/runtime/asset_handle.h>
#include <radray/runtime/material_parameter_binding_map.h>

namespace radray {

// 材质默认状态；每个 Pass 显式选择沿用或覆盖哪些字段。
// 附件格式、采样与几何拓扑由 Pass 提供。
struct MaterialRenderState {
    render::CullMode Cull{render::CullMode::Back};
    std::optional<render::BlendState> Blend;
    render::ColorWrites ColorWriteMask{render::ColorWrite::All};

    render::CompareFunction DepthCompare{render::CompareFunction::LessEqual};
    render::DepthBiasState DepthBias{};
    std::optional<render::StencilState> Stencil;

    // 字节大小的字段集中放在自然对齐的状态字段之后。
    bool DepthTestEnable{true};
    bool DepthWriteEnable{true};
    uint8_t StencilReference{0};
};

struct MaterialProperties {
    MaterialTagSet Tags;
    MaterialRenderState RenderState;
};

struct MaterialShaderProgram {
    string Name;
    AssetHandle<ShaderAsset> Shader;
    uint32_t VariantIndex{0};
    string VertexEntry;
    string PixelEntry;
};

bool PrepareMaterialVertexInput(const ShaderTargetProgramInterfaceDesc& interface, const VertexInput& meshInput, ResolvedVertexInputState& output);

using MaterialParameterValue = std::variant<float, array<float, 4>, Nullable<render::TextureView*>, Nullable<render::Sampler*>>;
struct MaterialParameter {
    string Name;
    MaterialParameterValue DefaultValue;
};
struct MaterialFamilyParameters {
    AssetHandle<ShaderAsset> Shader;
    string ConstantBlockName;
    vector<string> Parameters;
};
struct MaterialAssetDesc {
    MaterialProperties Properties;
    vector<MaterialShaderProgram> Programs;
    vector<MaterialParameter> Parameters;
    vector<MaterialFamilyParameters> Families;
};

class MaterialAsset final {
public:
    MaterialAsset(MaterialAsset&&) noexcept = default;
    MaterialAsset& operator=(MaterialAsset&&) noexcept = default;
    MaterialAsset(const MaterialAsset&) = delete;
    MaterialAsset& operator=(const MaterialAsset&) = delete;
    const MaterialProperties& GetProperties() const noexcept;
    std::span<const MaterialShaderProgram> GetPrograms() const noexcept;
    std::optional<size_t> FindProgram(std::string_view name) const noexcept;
    Nullable<const MaterialShaderProgram*> GetProgram(size_t index) const noexcept;
    std::span<const MaterialParameter> GetParameters() const noexcept;
    std::optional<size_t> FindParameter(std::string_view name) const noexcept;
    Nullable<const MaterialParameterValue*> GetDefaultParameter(size_t index) const noexcept;
    Nullable<const MaterialParameterBindingMap*> GetBindingMap(AssetHandle<ShaderAsset> shader) const noexcept;

private:
    friend class AssetManager;
    MaterialAsset() = default;
    static std::optional<MaterialAsset> Create(const AssetManager& assets, const MaterialAssetDesc& desc);
    struct FamilyBinding {
        AssetHandle<ShaderAsset> Shader;
        MaterialParameterBindingMap Map;
    };
    MaterialProperties _properties;
    vector<MaterialShaderProgram> _programs;
    vector<MaterialParameter> _parameters;
    vector<FamilyBinding> _bindings;
};

class MaterialInstance {
public:
    explicit MaterialInstance(AssetHandle<MaterialAsset> material) noexcept;
    AssetHandle<MaterialAsset> GetMaterial() const noexcept;
    Nullable<const MaterialParameterValue*> GetParameter(const AssetManager& assets, size_t index) const noexcept;
    bool SetParameter(const AssetManager& assets, size_t index, MaterialParameterValue value);
    bool ResetParameter(const AssetManager& assets, size_t index) noexcept;

private:
    AssetHandle<MaterialAsset> _material;
    vector<std::optional<MaterialParameterValue>> _overrides;
};

class MaterialRenderProxy {
public:
    // Explicit snapshots. Failure leaves the previous snapshot unchanged.
    bool Update(const AssetManager& assets, AssetHandle<MaterialAsset> material);
    bool Update(const AssetManager& assets, const MaterialInstance& instance);
    AssetHandle<MaterialAsset> GetMaterial() const noexcept;
    Nullable<const MaterialParameterValue*> GetParameter(size_t index) const noexcept;

private:
    AssetHandle<MaterialAsset> _material;
    vector<MaterialParameterValue> _parameters;
};

}  // namespace radray
