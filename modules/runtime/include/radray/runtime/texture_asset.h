#pragma once

#include <filesystem>

#include <radray/image_data.h>
#include <radray/render/common.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>

namespace radray {

class FrameUploadScheduler;

/// GPU 贴图资产。对应 UE5 的 UTexture2D(最小化):持有已上传的 device-local
/// render::Texture + 一个 SRV(render::TextureView)。
///
/// 设计:
/// - 构造即完整:CPU 解码 + GPU 上传由加载协程在构造前完成,资产入库即可被采样绑定。
/// - 与 ImageAsset(纯 CPU)解耦:ImageAsset 持像素,TextureAsset 持 GPU 资源。
/// - OnUnload 释放 GPU 资源。
class TextureAsset : public Asset {
public:
    TextureAsset() noexcept = default;
    TextureAsset(
        string name,
        unique_ptr<render::Texture> texture,
        unique_ptr<render::TextureView> srv) noexcept;
    ~TextureAsset() noexcept override;

    void OnUnload() override;
    AssetTypeId GetTypeId() const noexcept override;

    bool IsValid() const noexcept { return _texture != nullptr && _srv != nullptr; }

    const string& GetName() const noexcept { return _name; }
    render::Texture* GetTexture() const noexcept { return _texture.get(); }
    render::TextureView* GetSrv() const noexcept { return _srv.get(); }

private:
    string _name;
    unique_ptr<render::Texture> _texture;
    unique_ptr<render::TextureView> _srv;
};

template <>
struct RuntimeTypeTrait<TextureAsset> {
    static constexpr RuntimeTypeId value{0x7c3e9a14, 0x8b2d, 0x4f61, 0xa9, 0x05, 0x3e, 0x6c, 0x1d, 0x82, 0x4b, 0x90};
};

struct TextureAssetLoadOptions {
    /// true 时按 sRGB 解释纹理(GPU 采样时做 sRGB→linear)。base color / emissive 用 true;
    /// normal / metallic-roughness / occlusion 用 false。
    bool Srgb{false};
    /// 解码失败时的回退像素(CPU)。为空时加载失败。
    ImageData FallbackImage{};
};

/// 从已解码的 CPU 像素(ImageData)创建 GPU 贴图。协程内部 co_await 帧顶 upload phase
/// 录制上传,再等 GPU fence,完成后一次性构造 TextureAsset。
StreamingAssetRef<TextureAsset> LoadTextureAssetFromImage(
    AssetManager& assetManager,
    FrameUploadScheduler& frameUploads,
    const AssetId& assetId,
    string name,
    ImageData image,
    const TextureAssetLoadOptions& options = {});

/// 从编码字节(PNG/JPEG)解码后创建 GPU 贴图。
StreamingAssetRef<TextureAsset> LoadTextureAssetFromMemory(
    AssetManager& assetManager,
    FrameUploadScheduler& frameUploads,
    const AssetId& assetId,
    string name,
    vector<byte> encodedBytes,
    const TextureAssetLoadOptions& options = {});

}  // namespace radray
