#pragma once

#include <span>

#include <radray/basic_math.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/texture_asset.h>
#include <radray/types.h>

namespace radray {

/// 标准材质的 alpha 混合模式 (语义对齐 glTF 2.0 material.alphaMode, 但与任何导入器解耦)。
enum class StandardAlphaMode {
    Opaque,
    Mask,
    Blend,
};

/// 一张已上传 GPU 的贴图引用 (非拥有语义: 生命周期挂在产出方, 如 GltfAsset)。
/// Srgb 标记该图在何种色彩空间上传 (baseColor/emissive = sRGB, 其余 = linear)。
struct StandardMaterialTexture {
    StreamingAssetRef<TextureAsset> Texture{};
    bool Srgb{false};
};

/// 中性 PBR metallic-roughness 材质描述 (导入器无关, shader 无关)。
///
/// 模型导入器 (glTF / FBX / ...) 只负责产出本描述 + 贴图表, 具体翻译成引擎材质由
/// 当前渲染管线提供的 IStandardMaterialFactory 完成 (对应 Unity 的 MaterialDescription)。
/// 贴图索引指向随描述一起传入的贴图表; -1 表示该槽无贴图。
struct StandardMaterialDescription {
    string Name{"Default"};

    // ── core metallic-roughness ──
    Eigen::Vector4f BaseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float MetallicFactor{1.0f};
    float RoughnessFactor{1.0f};
    int BaseColorTexture{-1};          // sRGB
    int MetallicRoughnessTexture{-1};  // linear (G=roughness, B=metallic)

    // ── normal / occlusion / emissive ──
    int NormalTexture{-1};  // linear
    float NormalScale{1.0f};
    int OcclusionTexture{-1};  // linear (R 通道)
    float OcclusionStrength{1.0f};
    Eigen::Vector3f EmissiveFactor{0.0f, 0.0f, 0.0f};
    float EmissiveStrength{1.0f};  // KHR_materials_emissive_strength (无扩展时为 1)
    int EmissiveTexture{-1};       // sRGB

    // ── alpha / 双面 ──
    StandardAlphaMode AlphaMode{StandardAlphaMode::Opaque};
    float AlphaCutoff{0.5f};
    bool DoubleSided{false};

    // ── 扩展 (映射到 Principled 参数) ──
    // KHR_materials_specular
    float Specular{0.5f};
    float SpecularTint{0.0f};
    // KHR_materials_clearcoat
    float Clearcoat{0.0f};
    float ClearcoatGloss{0.0f};
    // KHR_materials_sheen
    float Sheen{0.0f};
    float SheenTint{0.0f};

    bool HasBaseColorTexture() const noexcept { return BaseColorTexture >= 0; }
    bool HasMetallicRoughnessTexture() const noexcept { return MetallicRoughnessTexture >= 0; }
    bool HasNormalTexture() const noexcept { return NormalTexture >= 0; }
    bool HasOcclusionTexture() const noexcept { return OcclusionTexture >= 0; }
    bool HasEmissiveTexture() const noexcept { return EmissiveTexture >= 0; }
};

/// 标准材质工厂: 把中性材质描述翻译成当前渲染管线的引擎 MaterialAsset。
///
/// 每个 RenderPipeline 实现并持有一份 (生命周期 == 管线); 模型导入 (GltfAsset::SpawnScene)
/// 通过 RenderPipeline::GetStandardMaterialFactory() 取得, 无需知道具体管线类型。
/// 对应 Unity 里 AssetPostprocessor.OnPreprocessMaterialDescription + RenderPipelineAsset.defaultMaterial
/// 两个机制的合并抽象。
class IStandardMaterialFactory {
public:
    virtual ~IStandardMaterialFactory() = default;

    /// 把一个中性材质描述 + 贴图表翻译成引擎材质。
    /// textures 为整个导入源的纹理表 (用描述里的贴图索引下标访问)。
    /// 返回空引用表示翻译失败 (调用方可回退到 GetDefaultMaterial)。
    virtual StreamingAssetRef<MaterialAsset> CreateMaterial(
        const StandardMaterialDescription& desc,
        std::span<const StandardMaterialTexture> textures) = 0;

    /// 无材质 primitive 的兜底材质 (对应 Unity RenderPipelineAsset.defaultMaterial)。
    virtual StreamingAssetRef<MaterialAsset> GetDefaultMaterial() = 0;
};

}  // namespace radray
