#pragma once

#include <filesystem>
#include <optional>

#include <radray/types.h>
#include <radray/structured_buffer.h>
#include <radray/render/common.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material_parameter_layout.h>
#include <radray/runtime/shader_identity.h>
#include <radray/runtime/shader_variant.h>

namespace radray {

class GpuSystem;

enum class MaterialBlendMode : uint32_t {
    Opaque,
    Masked,
    Blend,
};

struct MaterialShaderSet {
    CompiledShaderEntry VS{};
    std::optional<CompiledShaderEntry> PS{};
    render::RootSignature* RootSig{nullptr};
    RootSignatureLayoutKey RootLayout{};
};

/// Material 初始化描述。承载一个图形材质所需的 shader 源与渲染状态。
/// 对应 UE5 UMaterial 的最小化等价:一个 pass、一组 VS/PS、固定渲染状态。
/// PSO 真正创建依赖顶点布局与 RT 格式,故不在此处而在 PSOCache 按需组合。
struct MaterialDescriptor {
    std::filesystem::path ShaderPath{};
    string ShaderName{};
    string VsEntry{"VSMain"};
    string PsEntry{"PSMain"};
    string DepthPsEntry{"PSDepthOnlyMain"};
    MaterialBlendMode BlendMode{MaterialBlendMode::Opaque};
    bool TwoSided{false};
    float AlphaCutoff{0.5f};
    render::PrimitiveState Primitive{render::PrimitiveState::Default()};
    /// per-material 绑定频率所在的 descriptor set 索引(register space)。
    /// 约定:set0=per-view,set1=per-material,push-constant=per-object。
    uint32_t MaterialSetIndex{1};
    /// per-material cbuffer 名字。为空时取该 set 上第一个 cbuffer。
    string MaterialCBufferName{};
};

/// 图形材质资产。对应 UE5 的 UMaterial / FMaterial(最小化)。
///
/// 设计:
/// - 走 AssetManager(与 StaticMesh 一致)，【构造函数一次性初始化】: 构造时
///   编译 shader、从 GpuSystem 取共享 RootSignature，不再设二段式 Initialize。
/// - 【RootSignature 不独占】: RootSignature 由参与的 shader 绑定布局决定，
///   由 GpuSystem 按 layout 缓存共享，Material 仅持非拥有指针。
/// - shader 也是非拥有(来自 GpuSystem 的 shader 缓存)。
/// - 不持有 PSO：PSO 依赖顶点布局 + RT 格式，由 PSOCache 按 (Material, 顶点签名, RT 格式)
///   组合缓存。
class Material : public Asset {
public:
    /// 构造时立即初始化: 编译默认 VS/PS、取共享 RootSignature、填充材质语义。
    /// 初始化失败时保持无效状态(IsValid() 返回 false)，不抛异常。
    Material(GpuSystem& gpuSystem, const MaterialDescriptor& desc) noexcept;
    ~Material() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    bool IsValid() const noexcept {
        return _defaultShaders.RootSig != nullptr && _defaultShaders.VS.Target != nullptr &&
            _defaultShaders.PS.has_value() && _defaultShaders.PS->Target != nullptr;
    }

    std::string_view GetVsEntry() const noexcept { return _vsEntry; }
    std::string_view GetPsEntry() const noexcept { return _psEntry; }
    std::string_view GetDepthPsEntry() const noexcept { return _depthPsEntry; }
    render::RootSignature* GetRootSignature() const noexcept { return _defaultShaders.RootSig; }

    MaterialBlendMode GetBlendMode() const noexcept { return _blendMode; }
    bool IsTwoSided() const noexcept { return _twoSided; }
    float GetAlphaCutoff() const noexcept { return _alphaCutoff; }
    bool IsMasked() const noexcept { return _blendMode == MaterialBlendMode::Masked; }
    bool IsTransparent() const noexcept { return _blendMode == MaterialBlendMode::Blend; }
    const render::PrimitiveState& GetPrimitiveState() const noexcept { return _primitive; }
    const MaterialShaderSet* GetShaderSet(const ShaderVariantKey& key, PixelShaderMode psMode) const;

    std::optional<render::BindingParameterId> FindParameterId(std::string_view name) const noexcept;

    /// per-material 参数布局(后端无关)。从 PS/VS 反射的 MaterialSetIndex set 抽取。
    /// shader 没有 per-material cbuffer 时为一个空布局(HasConstantBuffer()==false)。
    const MaterialParameterLayout& GetParameterLayout() const noexcept { return _paramLayout; }

    /// per-material set 索引(register space)。
    uint32_t GetMaterialSetIndex() const noexcept { return _materialSetIndex; }

    /// 默认值存储模板。MaterialInstance 克隆它得到自己的参数副本。
    /// 无 per-material cbuffer 时为 std::nullopt。
    const std::optional<StructuredBufferStorage>& GetStorageTemplate() const noexcept { return _storageTemplate; }

private:
    MaterialShaderSet CompileShaderSet(const ShaderVariantKey& key, PixelShaderMode psMode) const;

    GpuSystem* _gpuSystem{nullptr};
    std::filesystem::path _shaderPath{};
    string _shaderName{};
    mutable unordered_map<ShaderVariantKey, MaterialShaderSet, ShaderVariantKeyHash> _variants;
    mutable unordered_map<ShaderVariantKey, MaterialShaderSet, ShaderVariantKeyHash> _alphaClipVariants;
    MaterialShaderSet _defaultShaders{};
    string _vsEntry{};
    string _psEntry{};
    string _depthPsEntry{};
    MaterialBlendMode _blendMode{MaterialBlendMode::Opaque};
    bool _twoSided{false};
    float _alphaCutoff{0.5f};
    render::PrimitiveState _primitive{render::PrimitiveState::Default()};
    uint32_t _materialSetIndex{1};
    MaterialParameterLayout _paramLayout{};
    std::optional<StructuredBufferStorage> _storageTemplate{};
};

template <>
struct RuntimeTypeTrait<Material> {
    static constexpr RuntimeTypeId value{0x4b2c7d9e, 0x1a3f, 0x4e58, 0x9c, 0x6d, 0x2f, 0x71, 0x8b, 0x40, 0x55, 0xc3};
};

/// Material 的加载工厂。纯 CPU(编译 shader + 取共享 RootSignature),无 GPU 上传阶段。
AssetLoadTask LoadMaterial(GpuSystem& gpuSystem, const MaterialDescriptor& desc);

}  // namespace radray
