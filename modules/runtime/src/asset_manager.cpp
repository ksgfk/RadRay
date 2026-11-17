#include <radray/runtime/asset_manager.h>

#include <radray/logger.h>

namespace radray {

AssetManager::AssetManager(gpu::Device& device) noexcept : _device(device) {}
AssetManager::~AssetManager() noexcept = default;
std::optional<AssetHandle<ShaderAsset>> AssetManager::CreateShader(const ShaderAssetDesc& desc) {
    auto asset = ShaderAsset::Create(_device, desc);
    if (!asset) {
        RADRAY_ERR_LOG("Shader asset creation failed");
        return std::nullopt;
    }
    return AssetHandle<ShaderAsset>{_shaders.Emplace(std::move(*asset))};
}
std::optional<AssetHandle<ShaderAsset>> AssetManager::LoadShader(const std::filesystem::path& indexPath) {
    auto asset = ShaderAsset::Load(_device, indexPath);
    if (!asset) {
        RADRAY_ERR_LOG("Shader asset load failed: {}", indexPath.generic_string());
        return std::nullopt;
    }
    return AssetHandle<ShaderAsset>{_shaders.Emplace(std::move(*asset))};
}
std::optional<AssetHandle<MaterialAsset>> AssetManager::CreateMaterial(const MaterialAssetDesc& desc) {
    auto asset = MaterialAsset::Create(*this, desc);
    if (!asset) {
        RADRAY_ERR_LOG("Material asset creation failed");
        return std::nullopt;
    }
    const auto slot = _materials.Emplace(std::move(*asset));
    const AssetHandle<MaterialAsset> handle{slot};
    for (auto& binding : _materials.Get(slot)._bindings) binding.Map._material = handle;
    return handle;
}
Nullable<const ShaderAsset*> AssetManager::Get(AssetHandle<ShaderAsset> handle) const noexcept { return _shaders.TryGet(handle._handle); }
Nullable<const MaterialAsset*> AssetManager::Get(AssetHandle<MaterialAsset> handle) const noexcept { return _materials.TryGet(handle._handle); }

}  // namespace radray
