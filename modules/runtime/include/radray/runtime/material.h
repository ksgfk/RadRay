#pragma once

// 材质所有权与 UE 对照见 docs/architecture/runtime.md。

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include <radray/shader/shader_layout.h>
#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/gpu/types.h>
#include <radray/runtime/material_tag.h>
#include <radray/runtime/vertex_input.h>
#include <radray/runtime/asset_handle.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material_parameter_binding_map.h>

namespace radray {
class MaterialAsset;
class ShaderAsset;

// 材质默认状态；每个 Pass 显式选择沿用或覆盖哪些字段。
// 附件格式、采样与几何拓扑由 Pass 提供。
class MaterialRenderState {
public:
    gpu::CullMode Cull{gpu::CullMode::Back};
    std::optional<gpu::BlendState> Blend;
    gpu::ColorWrites ColorWriteMask{gpu::ColorWrite::All};

    gpu::CompareFunction DepthCompare{gpu::CompareFunction::LessEqual};
    gpu::DepthBiasState DepthBias{};
    std::optional<gpu::StencilState> Stencil;

    // 字节大小的字段集中放在自然对齐的状态字段之后。
    bool DepthTestEnable{true};
    bool DepthWriteEnable{true};
    uint8_t StencilReference{0};
    friend bool operator==(const MaterialRenderState&, const MaterialRenderState&) = default;
};

struct MaterialProperties {
    MaterialTagSet Tags;
    MaterialRenderState RenderState;
};

class MaterialShaderProgram {
public:
    string Name;
    AssetHandle<ShaderAsset> Shader;
    uint32_t VariantIndex{0};
    string VertexEntry;
    string PixelEntry;
    friend bool operator==(const MaterialShaderProgram&, const MaterialShaderProgram&) = default;
};

// array<float, 16> 在 CPU 侧统一按列主序（Column-Major，m[column * 4 + row]，与 Eigen::Matrix4f::data() 一致）存储；
// 写入常量块时按着色器反射的 RowMajor 自动适配行列主序。
using MaterialParameterValue = std::variant<
    float, int32_t, uint32_t,
    array<float, 2>, array<float, 3>, array<float, 4>,
    array<int32_t, 4>, array<uint32_t, 4>,
    array<float, 16>,
    Nullable<gpu::TextureView*>,
    Nullable<gpu::Sampler*>>;

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

AssetRef<MaterialAsset> CreateMaterial(AssetManager& assets, const MaterialAssetDesc& desc, AssetId id = {});

bool StageMaterial(AssetTransaction& tx, AssetHandle<MaterialAsset> target, const MaterialAssetDesc& desc);
bool StageMaterial(AssetTransaction& tx, const AssetRef<MaterialAsset>& target, const MaterialAssetDesc& desc);

class MaterialAsset final {
public:
    MaterialAsset(MaterialAsset&&) noexcept = default;
    MaterialAsset& operator=(MaterialAsset&&) noexcept = default;
    MaterialAsset(const MaterialAsset&) = delete;
    MaterialAsset& operator=(const MaterialAsset&) = delete;
    AssetReloadDecision ClassifyReload(const MaterialAsset& candidate) const;
    const MaterialProperties& GetProperties() const noexcept;
    std::span<const MaterialShaderProgram> GetPrograms() const noexcept;
    std::optional<size_t> FindProgram(std::string_view name) const noexcept;
    Nullable<const MaterialShaderProgram*> GetProgram(size_t index) const noexcept;
    std::span<const MaterialParameter> GetParameters() const noexcept;
    std::optional<size_t> FindParameter(std::string_view name) const noexcept;
    Nullable<const MaterialParameterValue*> GetDefaultParameter(size_t index) const noexcept;
    Nullable<const MaterialParameterBindingMap*> GetBindingMap(AssetHandle<ShaderAsset> shader) const noexcept;

private:
    friend AssetRef<MaterialAsset> CreateMaterial(AssetManager&, const MaterialAssetDesc&, AssetId);
    friend bool StageMaterial(AssetTransaction&, AssetHandle<MaterialAsset>, const MaterialAssetDesc&);
    friend bool StageMaterial(AssetTransaction&, const AssetRef<MaterialAsset>&, const MaterialAssetDesc&);
    MaterialAsset() = default;
    template <typename Target>
    static bool Stage(AssetTransaction& tx, const Target& target, AssetHandle<MaterialAsset> self, const MaterialAssetDesc& desc);
    template <typename Resolve>
    static std::optional<MaterialAsset> Create(Resolve&& resolve, const MaterialAssetDesc& desc, AssetHandle<MaterialAsset> self);
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

bool PrepareMaterialVertexInput(const shader::ShaderVariantTargetInterfaceDesc& interface, const VertexInput& meshInput, ResolvedVertexInputState& output);

}  // namespace radray
