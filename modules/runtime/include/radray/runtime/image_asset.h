#pragma once

#include <filesystem>

#include <radray/image_data.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>

namespace radray {

class ImageAsset : public Asset {
public:
    ImageAsset() noexcept = default;
    ImageAsset(string name, ImageData image) noexcept;
    ~ImageAsset() noexcept override;

    void OnUnload() override;
    AssetTypeId GetTypeId() const noexcept override;

    bool IsValid() const noexcept { return _image.Data != nullptr && _image.Width != 0 && _image.Height != 0; }

    const string& GetName() const noexcept { return _name; }
    const ImageData& GetImage() const noexcept { return _image; }
    ImageData& GetImage() noexcept { return _image; }

private:
    string _name;
    ImageData _image;
};

template <>
struct RuntimeTypeTrait<ImageAsset> {
    static constexpr RuntimeTypeId value{0x59d6a2e3, 0x5c8a, 0x49be, 0xb1, 0x32, 0x2f, 0x4d, 0x65, 0xb9, 0x1a, 0x07};
};

struct ImageAssetLoadOptions {
    bool ConvertToRgba8{true};
    ImageData FallbackImage{};
};

ImageData MakeSolidImage(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
ImageData ConvertToRGBA8(const ImageData& src);

/// 解码 PNG/JPEG 编码字节为 ImageData。失败返回 nullopt。RGB 源自动补 alpha=0xff。
/// 供不走 ImageAsset 的调用方(如 glTF 加载协程直接解码后上传 GPU)复用。
std::optional<ImageData> DecodeImageBytes(std::span<const byte> encoded);

StreamingAssetRef<ImageAsset> LoadImageAsset(
    AssetManager& assetManager,
    const std::filesystem::path& path,
    const ImageAssetLoadOptions& options = {});

StreamingAssetRef<ImageAsset> LoadImageAsset(
    AssetManager& assetManager,
    const AssetId& assetId,
    const std::filesystem::path& path,
    const ImageAssetLoadOptions& options = {});

StreamingAssetRef<ImageAsset> LoadImageAssetFromMemory(
    AssetManager& assetManager,
    const AssetId& assetId,
    string name,
    vector<byte> encodedBytes,
    const ImageAssetLoadOptions& options = {});

}  // namespace radray
