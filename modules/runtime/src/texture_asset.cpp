#include <radray/runtime/texture_asset.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include <fmt/format.h>

#include <radray/file.h>
#include <radray/logger.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/image_asset.h>

std::size_t std::hash<radray::TextureSubViewDesc>::operator()(
    const radray::TextureSubViewDesc& desc) const noexcept {
    radray::HashCode hash;
    hash.Add(static_cast<radray::int32_t>(desc.Dim));
    hash.Add(static_cast<radray::int32_t>(desc.Format));
    hash.Add(desc.Range.BaseArrayLayer);
    hash.Add(desc.Range.ArrayLayerCount);
    hash.Add(desc.Range.BaseMipLevel);
    hash.Add(desc.Range.MipLevelCount);
    return hash.ToHashCode();
}

namespace radray {
namespace {

render::TextureFormat PickFormat(bool srgb) noexcept {
    return srgb ? render::TextureFormat::RGBA8_UNORM_SRGB : render::TextureFormat::RGBA8_UNORM;
}

/// 在 upload phase 内从 RGBA8 CPU 像素建 device-local 贴图 + SRV,录制上传命令。
/// 不等 fence(由调用方 co_await frame.WaitGpu())。失败返回 nullopt。
struct UploadedTexture {
    unique_ptr<render::Texture> Texture;
    unique_ptr<render::TextureView> Srv;
};

float SrgbToLinear(uint32_t value) noexcept {
    const float normalized = static_cast<float>(value) / 255.0f;
    return normalized <= 0.04045f
               ? normalized / 12.92f
               : std::pow((normalized + 0.055f) / 1.055f, 2.4f);
}

uint32_t LinearToSrgb(float value) noexcept {
    const float encoded = value <= 0.0031308f
                              ? value * 12.92f
                              : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint32_t>(std::lround(std::clamp(encoded, 0.0f, 1.0f) * 255.0f));
}

vector<vector<byte>> BuildRgba8MipChain(const ImageData& rgba8, bool generateMips, bool srgb) {
    vector<vector<byte>> mipChain;
    mipChain.emplace_back(rgba8.GetSpan().begin(), rgba8.GetSpan().end());
    if (!generateMips) {
        return mipChain;
    }

    uint32_t sourceWidth = rgba8.Width;
    uint32_t sourceHeight = rgba8.Height;
    while (sourceWidth > 1 || sourceHeight > 1) {
        const uint32_t destinationWidth = std::max(sourceWidth / 2, 1u);
        const uint32_t destinationHeight = std::max(sourceHeight / 2, 1u);
        const vector<byte>& source = mipChain.back();
        vector<byte> destination(static_cast<size_t>(destinationWidth) * destinationHeight * 4);

        for (uint32_t y = 0; y < destinationHeight; ++y) {
            for (uint32_t x = 0; x < destinationWidth; ++x) {
                float totals[4]{};
                uint32_t sampleCount = 0;
                for (uint32_t offsetY = 0; offsetY < 2; ++offsetY) {
                    const uint32_t sourceY = y * 2 + offsetY;
                    if (sourceY >= sourceHeight) {
                        continue;
                    }
                    for (uint32_t offsetX = 0; offsetX < 2; ++offsetX) {
                        const uint32_t sourceX = x * 2 + offsetX;
                        if (sourceX >= sourceWidth) {
                            continue;
                        }
                        const size_t sourceOffset =
                            (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4;
                        for (size_t channel = 0; channel < 4; ++channel) {
                            const uint32_t sample = std::to_integer<uint32_t>(source[sourceOffset + channel]);
                            totals[channel] += srgb && channel < 3
                                                   ? SrgbToLinear(sample)
                                                   : static_cast<float>(sample);
                        }
                        ++sampleCount;
                    }
                }
                const size_t destinationOffset =
                    (static_cast<size_t>(y) * destinationWidth + x) * 4;
                for (size_t channel = 0; channel < 4; ++channel) {
                    const float average = totals[channel] / static_cast<float>(sampleCount);
                    const uint32_t encoded = srgb && channel < 3
                                                 ? LinearToSrgb(average)
                                                 : static_cast<uint32_t>(std::lround(average));
                    destination[destinationOffset + channel] = static_cast<byte>(encoded);
                }
            }
        }

        mipChain.push_back(std::move(destination));
        sourceWidth = destinationWidth;
        sourceHeight = destinationHeight;
    }
    return mipChain;
}

std::optional<UploadedTexture> RecordTextureUpload(
    const FrameUploadScope& frame,
    const ImageData& rgba8,
    bool srgb,
    bool generateMips,
    std::string_view debugName) {
    render::Device* device = frame.GetUploader().GetDevice();
    if (device == nullptr || rgba8.Data == nullptr || rgba8.Width == 0 || rgba8.Height == 0) {
        return std::nullopt;
    }
    const render::TextureFormat format = PickFormat(srgb);
    const vector<vector<byte>> mipChain = BuildRgba8MipChain(rgba8, generateMips, srgb);

    render::TextureDescriptor texDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Width = rgba8.Width,
        .Height = rgba8.Height,
        .DepthOrArraySize = 1,
        .MipLevels = static_cast<uint32_t>(mipChain.size()),
        .SampleCount = 1,
        .Format = format,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::Resource | render::TextureUse::CopyDestination,
        .Hints = render::ResourceHint::None};
    auto texOpt = device->CreateTexture(texDesc);
    if (!texOpt.HasValue()) {
        RADRAY_ERR_LOG("TextureAsset: CreateTexture failed for '{}'", debugName);
        return std::nullopt;
    }
    auto texture = texOpt.Release();
    texture->SetDebugName(fmt::format("texasset_{}", debugName));

    render::TextureViewDescriptor viewDesc{
        .Target = texture.get(),
        .Dim = render::TextureDimension::Dim2D,
        .Format = format,
        .Range = render::SubresourceRange::AllSub(),
        .Usage = render::TextureViewUsage::Resource};
    auto srvOpt = device->CreateTextureView(viewDesc);
    if (!srvOpt.HasValue()) {
        RADRAY_ERR_LOG("TextureAsset: CreateTextureView failed for '{}'", debugName);
        return std::nullopt;
    }
    auto srv = srvOpt.Release();
    srv->SetDebugName(fmt::format("texasset_srv_{}", debugName));

    for (uint32_t mipLevel = 0; mipLevel < mipChain.size(); ++mipLevel) {
        TextureUploadRequest request{};
        request.SrcData = mipChain[mipLevel];
        request.DstTexture = texture.get();
        request.DstRange = render::SubresourceRange{
            .BaseArrayLayer = 0,
            .ArrayLayerCount = 1,
            .BaseMipLevel = mipLevel,
            .MipLevelCount = 1};
        request.SrcRowPitch = 0;
        request.Before = mipLevel == 0
                             ? render::TextureState::Undefined
                             : render::TextureState::ShaderRead;
        request.After = render::TextureState::ShaderRead;
        frame.GetUploader().UploadTexture(frame.GetCommandBuffer(), request);
    }

    return UploadedTexture{std::move(texture), std::move(srv)};
}

task<AssetLoadResult> LoadTextureFromImageTask(
    FrameUploadScheduler& frameUploads,
    string name,
    ImageData image,
    TextureAssetLoadOptions options) {
    // RGBA8 归一(GPU 仅支持 RGBA8 上传路径)。
    ImageData rgba8 = ConvertToRGBA8(image);
    if (rgba8.Data == nullptr || rgba8.Width == 0 || rgba8.Height == 0) {
        if (options.FallbackImage.Data != nullptr) {
            rgba8 = ConvertToRGBA8(options.FallbackImage);
        }
    }
    if (rgba8.Data == nullptr || rgba8.Width == 0 || rgba8.Height == 0) {
        co_return AssetLoadResult::Failure(fmt::format("texture '{}' has no valid pixels", name));
    }

    FrameUploadScope frame = co_await frameUploads.BeginUpload();
    std::optional<UploadedTexture> uploaded = RecordTextureUpload(
        frame,
        rgba8,
        options.Srgb,
        options.GenerateMips,
        name);
    if (!uploaded.has_value()) {
        co_return AssetLoadResult::Failure(fmt::format("texture '{}' upload recording failed", name));
    }
    render::Device* device = frame.GetUploader().GetDevice();
    co_await frame.WaitGpu();

    co_return AssetLoadResult::Success(
        make_unique<TextureAsset>(
            device,
            std::move(name),
            std::move(uploaded->Texture),
            std::move(uploaded->Srv)));
}

task<AssetLoadResult> LoadTextureFromMemoryTask(
    FrameUploadScheduler& frameUploads,
    string name,
    vector<byte> encodedBytes,
    TextureAssetLoadOptions options) {
    std::optional<ImageData> decoded = DecodeImageBytes(encodedBytes);
    ImageData image;
    if (decoded.has_value()) {
        image = std::move(decoded.value());
    } else if (options.FallbackImage.Data != nullptr) {
        image = options.FallbackImage;
    } else {
        co_return AssetLoadResult::Failure(fmt::format("texture '{}' decode failed", name));
    }
    // 复用 image 路径(其内部再做 RGBA8 归一与上传)。
    co_return co_await LoadTextureFromImageTask(
        frameUploads, std::move(name), std::move(image), std::move(options));
}

}  // namespace

bool TextureImportSettings::Deserialize(const JsonValue& json) {
    JsonObjectReader object{json};
    if (!object.IsValid()) {
        return false;
    }
    const size_t knownMemberCount = static_cast<size_t>(object.Has("srgb")) +
                                    static_cast<size_t>(object.Has("generateMips"));
    if (json.Size() != knownMemberCount) {
        return false;
    }
    TextureImportSettings decoded;
    if (!object.MemberIfPresent("srgb", decoded.Srgb) ||
        !object.MemberIfPresent("generateMips", decoded.GenerateMips)) {
        return false;
    }
    *this = decoded;
    return true;
}

bool TextureImportSettings::Serialize(JsonWriteContext& context) const noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           object.Member("srgb", Srgb) &&
           object.Member("generateMips", GenerateMips);
}

TextureImporter::TextureImporter(FrameUploadScheduler& frameUploads) noexcept
    : _frameUploads(frameUploads) {
}

std::string_view TextureImporter::GetTypeName() const noexcept {
    return "texture";
}

std::span<const std::string_view> TextureImporter::GetFileExtensions() const noexcept {
    static constexpr std::array<std::string_view, 3> extensions{".png", ".jpg", ".jpeg"};
    return extensions;
}

task<AssetLoadResult> TextureImporter::LoadTyped(
    std::filesystem::path path,
    TextureImportSettings settings) {
    std::optional<vector<byte>> encoded = ReadBinaryFile(path);
    if (!encoded.has_value()) {
        co_return AssetLoadResult::Failure(fmt::format("cannot read texture source '{}'", path.string()));
    }
    TextureAssetLoadOptions options{
        .Srgb = settings.Srgb,
        .GenerateMips = settings.GenerateMips};
    co_return co_await CreateTextureAssetFromMemory(
        _frameUploads,
        path.filename().string(),
        std::move(encoded.value()),
        std::move(options));
}

task<AssetLoadResult> CreateTextureAssetFromImage(
    FrameUploadScheduler& frameUploads,
    string name,
    ImageData image,
    TextureAssetLoadOptions options) {
    return LoadTextureFromImageTask(
        frameUploads,
        std::move(name),
        std::move(image),
        std::move(options));
}

task<AssetLoadResult> CreateTextureAssetFromMemory(
    FrameUploadScheduler& frameUploads,
    string name,
    vector<byte> encodedBytes,
    TextureAssetLoadOptions options) {
    return LoadTextureFromMemoryTask(
        frameUploads,
        std::move(name),
        std::move(encodedBytes),
        std::move(options));
}

TextureAsset::TextureAsset(
    render::Device* device,
    string name,
    unique_ptr<render::Texture> texture,
    unique_ptr<render::TextureView> srv) noexcept
    : _device(device),
      _name(std::move(name)),
      _texture(std::move(texture)),
      _srv(std::move(srv)) {
}

TextureAsset::~TextureAsset() noexcept = default;

void TextureAsset::OnUnload(AssetManager& manager) {
    // 【整包交出, 销毁顺序由 lambda 的成员声明顺序表达】: view 引用 texture, 故 view 必须
    // 先死。捕获列表里 views / srv 声明在 texture 之前, 而 lambda 的捕获成员按声明顺序
    // 构造、逆序析构 —— 这就是全部保证, 不依赖任何队列语义。
    //
    // 【为何要延迟】: 写进描述符堆的 view 会被 GPU 用到 fence 之后, 而本函数发生在引用
    // 归零的那一帧。见 asset.h 与 AssetManager::DeferDestroy。
    manager.DeferDestroy(
        [views = std::move(_viewCache),
         srv = std::move(_srv),
         texture = std::move(_texture)]() noexcept {});
    _viewCache.clear();
    _name.clear();
}

render::TextureView* TextureAsset::GetOrCreateSrv(const TextureSubViewDesc& sub) noexcept {
    if (sub.IsDefault()) {
        return _srv.get();
    }
    if (_device == nullptr || _texture == nullptr) {
        return nullptr;
    }
    if (auto it = _viewCache.find(sub); it != _viewCache.end()) {
        return it->second.get();
    }
    // Format::UNKNOWN 表示沿用底层贴图格式。
    const render::TextureFormat format =
        sub.Format == render::TextureFormat::UNKNOWN ? _texture->GetDesc().Format : sub.Format;
    render::TextureViewDescriptor viewDesc{
        .Target = _texture.get(),
        .Dim = sub.Dim,
        .Format = format,
        .Range = sub.Range,
        .Usage = render::TextureViewUsage::Resource};
    auto viewOpt = _device->CreateTextureView(viewDesc);
    if (!viewOpt.HasValue()) {
        RADRAY_ERR_LOG("TextureAsset::GetOrCreateSrv: CreateTextureView failed for '{}'", _name);
        return nullptr;
    }
    auto view = viewOpt.Release();
    view->SetDebugName(fmt::format("texasset_subsrv_{}", _name));
    render::TextureView* raw = view.get();
    _viewCache.emplace(sub, std::move(view));
    return raw;
}

StreamingAssetRef<TextureAsset> LoadTextureAssetFromImage(
    AssetManager& assetManager,
    FrameUploadScheduler& frameUploads,
    const AssetId& assetId,
    string name,
    ImageData image,
    const TextureAssetLoadOptions& options) {
    return assetManager.Load<TextureAsset>(AssetLoadRequest{
        .Id = assetId,
        .Task = CreateTextureAssetFromImage(frameUploads, name, std::move(image), options),
        .DebugName = std::move(name)});
}

StreamingAssetRef<TextureAsset> LoadTextureAssetFromMemory(
    AssetManager& assetManager,
    FrameUploadScheduler& frameUploads,
    const AssetId& assetId,
    string name,
    vector<byte> encodedBytes,
    const TextureAssetLoadOptions& options) {
    return assetManager.Load<TextureAsset>(AssetLoadRequest{
        .Id = assetId,
        .Task = CreateTextureAssetFromMemory(frameUploads, name, std::move(encodedBytes), options),
        .DebugName = std::move(name)});
}

}  // namespace radray
