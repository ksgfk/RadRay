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

uint64_t StableHash64(std::string_view text) noexcept {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : text) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

AssetId MakeImageAssetId(const std::filesystem::path& path) {
    const string key = fmt::format("image:{}", std::filesystem::absolute(path).generic_string());
    std::array<uint8_t, Guid::Size> bytes{};
    uint64_t h0 = StableHash64(key);
    uint64_t h1 = StableHash64(fmt::format("{}:salt", key));
    for (size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>((h0 >> ((7 - i) * 8)) & 0xffu);
        bytes[i + 8] = static_cast<uint8_t>((h1 >> ((7 - i) * 8)) & 0xffu);
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x40u);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);
    return AssetId{bytes};
}

AssetLoadTask LoadImageAssetTask(std::filesystem::path path, ImageAssetLoadOptions options) {
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

AssetLoadTask LoadImageAssetFromMemoryTask(string name, vector<byte> encodedBytes, ImageAssetLoadOptions options) {
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

void ImageAsset::OnUnload() {
    _name.clear();
    _image = ImageData{};
}

AssetTypeId ImageAsset::GetTypeId() const noexcept {
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
