#pragma once

#include <filesystem>
#include <functional>
#include <span>

#include <radray/basic_math.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/texture_asset.h>

namespace radray {

class Actor;
class World;
class FrameUploadScheduler;

/// glTF alpha 混合模式 (对应 glTF 2.0 material.alphaMode)。
enum class GltfAlphaMode {
    Opaque,
    Mask,
    Blend,
};

/// 一张已上传 GPU 的 glTF 纹理引用 (非拥有语义: 生命周期挂在 GltfAsset 上)。
/// Srgb 标记该图在何种色彩空间上传 (baseColor/emissive = sRGB, 其余 = linear)。
struct GltfTextureRef {
    StreamingAssetRef<TextureAsset> Texture{};
    bool Srgb{false};
};

/// 中性材质描述 (shader-无关)。数值全部来自 glTF, 由上层 MaterialFactory 翻译成引擎材质。
/// 贴图索引指向 GltfAsset::GetTextures(); -1 表示该槽无贴图。
struct GltfMaterialDesc {
    string Name{"Default"};

    // ── core metallic-roughness ──
    Eigen::Vector4f BaseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float MetallicFactor{1.0f};
    float RoughnessFactor{1.0f};
    int BaseColorTexture{-1};          // sRGB
    int MetallicRoughnessTexture{-1};  // linear (G=roughness, B=metallic)

    // ── normal / occlusion / emissive ──
    int NormalTexture{-1};   // linear
    float NormalScale{1.0f};
    int OcclusionTexture{-1};  // linear (R 通道)
    float OcclusionStrength{1.0f};
    Eigen::Vector3f EmissiveFactor{0.0f, 0.0f, 0.0f};
    float EmissiveStrength{1.0f};  // KHR_materials_emissive_strength (无扩展时为 1)
    int EmissiveTexture{-1};       // sRGB

    // ── alpha / 双面 ──
    GltfAlphaMode AlphaMode{GltfAlphaMode::Opaque};
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

/// 一个 glTF 节点 (对应 cgltf_node)。已完成 RH->LH 反射转换。
struct GltfNodeDesc {
    string Name;
    int Parent{-1};
    vector<int> Children;
    Eigen::Vector3f Translation{Eigen::Vector3f::Zero()};
    Eigen::Quaternionf Rotation{Eigen::Quaternionf::Identity()};
    Eigen::Vector3f Scale{Eigen::Vector3f::Ones()};
    Eigen::Matrix4f WorldMatrix{Eigen::Matrix4f::Identity()};
    bool HasMesh{false};
};

/// 一个已上传的 glTF primitive (单 section 覆盖整个 primitive)。
struct GltfPrimitiveDesc {
    string Name;
    int NodeIndex{-1};
    uint32_t MaterialIndex{0};
    StreamingAssetRef<StaticMesh> Mesh{};
    Eigen::Vector3f BoundsMin{Eigen::Vector3f::Zero()};
    Eigen::Vector3f BoundsMax{Eigen::Vector3f::Zero()};
    bool HasBounds{false};
};

struct GltfAssetLoadOptions {
    // 预留: 未来可加导入缩放 / 是否生成切线开关等。
};

/// 材质工厂回调: 把中性材质描述翻译成引擎 MaterialAsset。
/// textures 为整个 glTF 的纹理表 (用 GltfMaterialDesc 的贴图索引下标访问)。
/// 返回空引用表示该 primitive 不绑材质 (不会绘制)。
/// 返回 StreamingAssetRef: MaterialAsset 生命周期由回调方 (通常经 AssetManager) 管理,
/// 组件材质槽持该引用 (generation 检测兜底悬垂)。
using GltfMaterialFactory =
    std::function<StreamingAssetRef<MaterialAsset>(const GltfMaterialDesc&, std::span<const GltfTextureRef>)>;

/// glTF 资产 (对应一个已解析 + GPU 上传完成的 .gltf/.glb)。
/// 纯数据 + shader-无关: 持有节点树 / primitive (mesh refs) / 中性材质描述 / 纹理表。
/// 场景实例化通过 SpawnScene, 材质翻译交给调用方注入的 GltfMaterialFactory。
class GltfAsset : public Asset {
public:
    GltfAsset() noexcept = default;
    GltfAsset(
        std::filesystem::path path,
        vector<GltfNodeDesc> nodes,
        vector<int> rootNodes,
        vector<GltfPrimitiveDesc> primitives,
        vector<GltfMaterialDesc> materials,
        vector<GltfTextureRef> textures,
        Eigen::Vector3f boundsMin,
        Eigen::Vector3f boundsMax,
        bool hasBounds) noexcept;
    ~GltfAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    /// 实例化为场景 Actor 树, 返回根 Actor。
    /// materialFactory 把每个 primitive 的中性材质描述翻译成 MaterialAsset* 塞进材质槽。
    Actor* SpawnScene(World& world, const GltfMaterialFactory& materialFactory) const;

    const std::filesystem::path& GetPath() const noexcept { return _path; }
    const vector<GltfNodeDesc>& GetNodes() const noexcept { return _nodes; }
    const vector<int>& GetRootNodes() const noexcept { return _rootNodes; }
    const vector<GltfPrimitiveDesc>& GetPrimitives() const noexcept { return _primitives; }
    const vector<GltfMaterialDesc>& GetMaterials() const noexcept { return _materials; }
    const vector<GltfTextureRef>& GetTextures() const noexcept { return _textures; }
    const Eigen::Vector3f& GetBoundsMin() const noexcept { return _boundsMin; }
    const Eigen::Vector3f& GetBoundsMax() const noexcept { return _boundsMax; }
    bool HasBounds() const noexcept { return _hasBounds; }

private:
    std::filesystem::path _path;
    vector<GltfNodeDesc> _nodes;
    vector<int> _rootNodes;
    vector<GltfPrimitiveDesc> _primitives;
    vector<GltfMaterialDesc> _materials;
    vector<GltfTextureRef> _textures;
    Eigen::Vector3f _boundsMin{Eigen::Vector3f::Zero()};
    Eigen::Vector3f _boundsMax{Eigen::Vector3f::Zero()};
    bool _hasBounds{false};
};

/// 异步加载一个 glTF 文件 (协程)。几何 + 纹理在 GPU 上传完成后资产才 ready。
StreamingAssetRef<GltfAsset> LoadGltfAsset(
    AssetManager& assetManager,
    FrameUploadScheduler& frameUploads,
    const std::filesystem::path& path,
    const GltfAssetLoadOptions& options = {});

template <>
struct RuntimeTypeTrait<GltfAsset> {
    static constexpr RuntimeTypeId value{0xf4412d58, 0x716b, 0x4389, 0xb2, 0x7f, 0x0d, 0x39, 0x80, 0x74, 0x4d, 0x71};
    using Bases = std::tuple<Asset>;
};

}  // namespace radray
