#include <radray/runtime/image_asset.h>

#include <array>
#include <cstring>
#include <optional>
#include <fstream>
#include <sstream>
#include <string_view>

#include <fmt/format.h>

namespace radray {
namespace {

std::optional<ImageData> DecodeImageFromStream(std::istream& stream) {
    if (ImageData::IsPNG(stream)) {
        stream.clear();
        stream.seekg(0, std::ios::beg);
        return ImageData::LoadPNG(stream, PNGLoadSettings{.AddAlphaIfRGB = 0xffu});
    }
    stream.clear();
    stream.seekg(0, std::ios::beg);
    if (ImageData::IsJPEG(stream)) {
        stream.clear();
        stream.seekg(0, std::ios::beg);
        return ImageData::LoadJPEG(stream, JPEGLoadSettings{.AddAlphaIfRGB = 0xffu});
    }
    return std::nullopt;
}

ImageData ApplyImageLoadOptions(ImageData image, const ImageAssetLoadOptions& options) {
    if (options.ConvertToRgba8) {
        image = ConvertToRGBA8(image);
    }
    return image;
}

ImageData ResolveImageLoadFailure(const ImageAssetLoadOptions& options) {
    if (options.FallbackImage.Data != nullptr && options.FallbackImage.Width != 0 && options.FallbackImage.Height != 0) {
        return options.FallbackImage;
    }
    return {};
}

AssetId MakeImageAssetId(const std::filesystem::path& path) {
    return MakeAssetIdFromPath("image", path);
}

task<AssetLoadResult> LoadImageAssetTask(std::filesystem::path path, ImageAssetLoadOptions options) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream.is_open()) {
        ImageData fallback = ResolveImageLoadFailure(options);
        if (fallback.Data == nullptr) {
            co_return AssetLoadResult::Failure(fmt::format("failed to open image '{}'", path.string()));
        }
        co_return AssetLoadResult::Success(make_unique<ImageAsset>(path.string(), std::move(fallback)));
    }

    std::optional<ImageData> image = DecodeImageFromStream(stream);
    if (!image.has_value()) {
        ImageData fallback = ResolveImageLoadFailure(options);
        if (fallback.Data == nullptr) {
            co_return AssetLoadResult::Failure(fmt::format("unsupported image '{}'", path.string()));
        }
        co_return AssetLoadResult::Success(make_unique<ImageAsset>(path.string(), std::move(fallback)));
    }

    co_return AssetLoadResult::Success(
        make_unique<ImageAsset>(path.string(), ApplyImageLoadOptions(std::move(image.value()), options)));
}

task<AssetLoadResult> LoadImageAssetFromMemoryTask(string name, vector<byte> encodedBytes, ImageAssetLoadOptions options) {
    string storage;
    storage.resize(encodedBytes.size());
    if (!encodedBytes.empty()) {
        std::memcpy(storage.data(), encodedBytes.data(), encodedBytes.size());
    }

    std::istringstream stream{storage, std::ios::binary};
    std::optional<ImageData> image = DecodeImageFromStream(stream);
    if (!image.has_value()) {
        ImageData fallback = ResolveImageLoadFailure(options);
        if (fallback.Data == nullptr) {
            co_return AssetLoadResult::Failure(fmt::format("unsupported image '{}'", name));
        }
        co_return AssetLoadResult::Success(make_unique<ImageAsset>(std::move(name), std::move(fallback)));
    }

    co_return AssetLoadResult::Success(
        make_unique<ImageAsset>(std::move(name), ApplyImageLoadOptions(std::move(image.value()), options)));
}

}  // namespace

ImageAsset::ImageAsset(string name, ImageData image) noexcept
    : _name(std::move(name)), _image(std::move(image)) {
}

ImageAsset::~ImageAsset() noexcept = default;

void ImageAsset::OnUnload(AssetManager& manager) {
    // 【纯 CPU 资产, 无事可做】: 析构函数会释放 _name / _image, 且那才是唯一正确的地方
    // (见 Asset::OnUnload)。这里刻意留空而不是搬空成员 —— 提前清理只会制造一个
    // "已析构但还没死" 的中间态。
    (void)manager;
}

RuntimeTypeId ImageAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<ImageAsset>;
}

ImageData MakeSolidImage(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    ImageData img;
    img.Width = 1;
    img.Height = 1;
    img.Format = ImageFormat::RGBA8_BYTE;
    img.Data = make_unique<byte[]>(4);
    img.Data[0] = static_cast<byte>(r);
    img.Data[1] = static_cast<byte>(g);
    img.Data[2] = static_cast<byte>(b);
    img.Data[3] = static_cast<byte>(a);
    return img;
}

std::optional<ImageData> DecodeImageBytes(std::span<const byte> encoded) {
    if (encoded.empty()) {
        return std::nullopt;
    }
    string storage;
    storage.resize(encoded.size());
    std::memcpy(storage.data(), encoded.data(), encoded.size());
    std::istringstream stream{storage, std::ios::binary};
    return DecodeImageFromStream(stream);
}

ImageData ConvertToRGBA8(const ImageData& src) {
    if (src.Format == ImageFormat::RGBA8_BYTE) {
        return src;
    }
    if (src.Format == ImageFormat::RGB8_BYTE) {
        return src.RGB8ToRGBA8(0xff);
    }
    if (src.Format == ImageFormat::R8_BYTE) {
        ImageData out;
        out.Width = src.Width;
        out.Height = src.Height;
        out.Format = ImageFormat::RGBA8_BYTE;
        out.Data = make_unique<byte[]>(out.GetSize());
        const size_t count = static_cast<size_t>(src.Width) * src.Height;
        for (size_t i = 0; i < count; ++i) {
            byte v = src.Data[i];
            out.Data[i * 4 + 0] = v;
            out.Data[i * 4 + 1] = v;
            out.Data[i * 4 + 2] = v;
            out.Data[i * 4 + 3] = byte{0xff};
        }
        return out;
    }
    return MakeSolidImage(255, 255, 255, 255);
}

StreamingAssetRef<ImageAsset> LoadImageAsset(
    AssetManager& assetManager,
    const std::filesystem::path& path,
    const ImageAssetLoadOptions& options) {
    return LoadImageAsset(assetManager, MakeImageAssetId(path), path, options);
}

StreamingAssetRef<ImageAsset> LoadImageAsset(
    AssetManager& assetManager,
    const AssetId& assetId,
    const std::filesystem::path& path,
    const ImageAssetLoadOptions& options) {
    return assetManager.Load<ImageAsset>(AssetLoadRequest{
        .Id = assetId,
        .Task = LoadImageAssetTask(path, options),
        .DebugName = path.string()});
}

StreamingAssetRef<ImageAsset> LoadImageAssetFromMemory(
    AssetManager& assetManager,
    const AssetId& assetId,
    string name,
    vector<byte> encodedBytes,
    const ImageAssetLoadOptions& options) {
    return assetManager.Load<ImageAsset>(AssetLoadRequest{
        .Id = assetId,
        .Task = LoadImageAssetFromMemoryTask(name, std::move(encodedBytes), options),
        .DebugName = std::move(name)});
}

}  // namespace radray
