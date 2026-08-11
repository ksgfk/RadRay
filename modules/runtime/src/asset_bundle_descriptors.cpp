#include <radray/runtime/asset_bundle_descriptors.h>

#include <radray/runtime/image_asset.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/texture_asset.h>

namespace radray {

RuntimeTypeId ImageAssetDescriptor::GetTypeId() const noexcept {
    return runtime_type_id_v<ImageAsset>;
}

unique_ptr<const AssetDescriptor> ImageAssetDescriptor::Clone() const {
    return make_unique<ImageAssetDescriptor>(ConvertToRgba8);
}

RuntimeTypeId TextureAssetDescriptor::GetTypeId() const noexcept {
    return runtime_type_id_v<TextureAsset>;
}

unique_ptr<const AssetDescriptor> TextureAssetDescriptor::Clone() const {
    return make_unique<TextureAssetDescriptor>(Srgb);
}

RuntimeTypeId StaticMeshAssetDescriptor::GetTypeId() const noexcept {
    return runtime_type_id_v<StaticMesh>;
}

unique_ptr<const AssetDescriptor> StaticMeshAssetDescriptor::Clone() const {
    return make_unique<StaticMeshAssetDescriptor>();
}

RuntimeTypeId ShaderAssetDescriptor::GetTypeId() const noexcept {
    return runtime_type_id_v<ShaderAsset>;
}

unique_ptr<const AssetDescriptor> ShaderAssetDescriptor::Clone() const {
    return make_unique<ShaderAssetDescriptor>(Representation, Target);
}

}  // namespace radray
