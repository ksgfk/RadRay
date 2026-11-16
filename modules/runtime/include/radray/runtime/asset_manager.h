#pragma once

#include <radray/runtime/material.h>
#include <radray/runtime/shader_asset.h>

namespace radray {

// Single-threaded registry. Registration is forbidden during scene synchronization/recording.
// The device, external material resources and tags must outlive their users.
class AssetManager final {
public:
    explicit AssetManager(render::Device& device) noexcept;
    // All CPU/GPU users must be finished; this object does not wait for GPU completion.
    ~AssetManager() noexcept;
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    std::optional<AssetHandle<ShaderAsset>> CreateShader(const ShaderAssetDesc& desc);
    std::optional<AssetHandle<ShaderAsset>> LoadShader(const std::filesystem::path& indexPath);
    std::optional<AssetHandle<MaterialAsset>> CreateMaterial(const MaterialAssetDesc& desc);
    // Borrowed until the next registration of this asset type. Handles are manager-local.
    Nullable<const ShaderAsset*> Get(AssetHandle<ShaderAsset> handle) const noexcept;
    Nullable<const MaterialAsset*> Get(AssetHandle<MaterialAsset> handle) const noexcept;

private:
    render::Device& _device;
    SparseSet<ShaderAsset> _shaders;
    SparseSet<MaterialAsset> _materials;
};

}  // namespace radray
