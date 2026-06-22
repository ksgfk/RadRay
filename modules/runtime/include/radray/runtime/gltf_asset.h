#pragma once

#include <filesystem>

#include <radray/basic_math.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material.h>
#include <radray/runtime/material_instance.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/texture_asset.h>

namespace radray {

class Actor;
class FrameUploadScheduler;
class GpuSystem;
class World;

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

struct GltfPrimitiveDesc {
    string Name;
    int NodeIndex{-1};
    uint32_t SourceMaterialIndex{0};
    StreamingAssetRef<StaticMesh> Mesh;
    StreamingAssetRef<Material> Material;
    /// per-使用点材质参数(从 glTF 材质转换而来,按名写入 gMaterial cbuffer)。
    vector<MaterialParameterAssignment> MaterialParams;
    /// per-使用点材质贴图(从 glTF 材质转换而来,按名绑定 shader 贴图槽)。
    vector<MaterialTextureAssignment> MaterialTextures;
    Eigen::Vector3f BoundsMin{Eigen::Vector3f::Zero()};
    Eigen::Vector3f BoundsMax{Eigen::Vector3f::Zero()};
    bool HasBounds{false};
};

struct GltfTextureDesc {
    string Name;
    StreamingAssetRef<TextureAsset> Texture;
};

struct GltfAssetLoadOptions {
    StreamingAssetRefAny DefaultMaterial{};
    GpuSystem* Gpu{nullptr};
    MaterialDescriptor MaterialTemplate{};
};

class GltfAsset : public Asset {
public:
    GltfAsset() noexcept = default;
    GltfAsset(
        std::filesystem::path path,
        vector<GltfNodeDesc> nodes,
        vector<int> rootNodes,
        vector<GltfPrimitiveDesc> primitives,
        vector<string> materialNames,
        vector<GltfTextureDesc> textures,
        Eigen::Vector3f boundsMin,
        Eigen::Vector3f boundsMax,
        bool hasBounds) noexcept;
    ~GltfAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    Actor* ExportToScene(World& world) const;

    const std::filesystem::path& GetPath() const noexcept { return _path; }
    const vector<GltfNodeDesc>& GetNodes() const noexcept { return _nodes; }
    const vector<int>& GetRootNodes() const noexcept { return _rootNodes; }
    const vector<GltfPrimitiveDesc>& GetPrimitives() const noexcept { return _primitives; }
    const vector<string>& GetMaterialNames() const noexcept { return _materialNames; }
    const vector<GltfTextureDesc>& GetTextures() const noexcept { return _textures; }
    const Eigen::Vector3f& GetBoundsMin() const noexcept { return _boundsMin; }
    const Eigen::Vector3f& GetBoundsMax() const noexcept { return _boundsMax; }
    bool HasBounds() const noexcept { return _hasBounds; }

private:
    std::filesystem::path _path;
    vector<GltfNodeDesc> _nodes;
    vector<int> _rootNodes;
    vector<GltfPrimitiveDesc> _primitives;
    vector<string> _materialNames;
    vector<GltfTextureDesc> _textures;
    Eigen::Vector3f _boundsMin{Eigen::Vector3f::Zero()};
    Eigen::Vector3f _boundsMax{Eigen::Vector3f::Zero()};
    bool _hasBounds{false};
};

template <>
struct RuntimeTypeTrait<GltfAsset> {
    static constexpr RuntimeTypeId value{0xf4412d58, 0x716b, 0x4389, 0xb2, 0x7f, 0x0d, 0x39, 0x80, 0x74, 0x4d, 0x71};
};

StreamingAssetRef<GltfAsset> LoadGltfAsset(
    AssetManager& assetManager,
    FrameUploadScheduler& frameUploads,
    const std::filesystem::path& path,
    const GltfAssetLoadOptions& options = {});

}  // namespace radray
