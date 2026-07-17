#include <radray/runtime/texture_asset.h>

#include <fmt/format.h>

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

std::optional<UploadedTexture> RecordTextureUpload(
    const FrameUploadScope& frame,
    const ImageData& rgba8,
    bool srgb,
    std::string_view debugName) {
    render::Device* device = frame.GetUploader().GetDevice();
    if (device == nullptr || rgba8.Data == nullptr || rgba8.Width == 0 || rgba8.Height == 0) {
        return std::nullopt;
    }
    const render::TextureFormat format = PickFormat(srgb);

    render::TextureDescriptor texDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Width = rgba8.Width,
        .Height = rgba8.Height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
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

    TextureUploadRequest request{};
    request.SrcData = rgba8.GetSpan();
    request.DstTexture = texture.get();
    request.DstRange = render::SubresourceRange{0, 1, 0, 1};
    request.SrcRowPitch = 0;  // 紧凑行距(width * 4)。
    request.Before = render::TextureState::Undefined;
    request.After = render::TextureState::ShaderRead;
    frame.GetUploader().UploadTexture(frame.GetCommandBuffer(), request);

    return UploadedTexture{std::move(texture), std::move(srv)};
}

AssetLoadTask LoadTextureFromImageTask(
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
    std::optional<UploadedTexture> uploaded = RecordTextureUpload(frame, rgba8, options.Srgb, name);
    if (!uploaded.has_value()) {
        co_return AssetLoadResult::Failure(fmt::format("texture '{}' upload recording failed", name));
    }
    render::Device* device = frame.GetUploader().GetDevice();
    co_await frame.WaitGpu();

    co_return AssetLoadResult::Success(make_unique<TextureAsset>(
        device,
        std::move(name),
        std::move(uploaded->Texture),
        std::move(uploaded->Srv)));
}

AssetLoadTask LoadTextureFromMemoryTask(
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
    co_return co_await LoadTextureFromImageTask(frameUploads, std::move(name), std::move(image), std::move(options));
}

}  // namespace

TextureAsset::TextureAsset(
    render::Device* device,
    string name,
    unique_ptr<render::Texture> texture,
    unique_ptr<render::TextureView> srv) noexcept
    : _device(device), _name(std::move(name)), _texture(std::move(texture)), _srv(std::move(srv)) {
}

TextureAsset::~TextureAsset() noexcept = default;

void TextureAsset::OnUnload(IRenderResourceRecycler& recycler) {
    _name.clear();
    for (auto& [key, view] : _viewCache) {
        recycler.RecycleRenderResource(std::move(view));
    }
    _viewCache.clear();
    recycler.RecycleRenderResource(std::move(_srv));
    recycler.RecycleRenderResource(std::move(_texture));
}

AssetTypeId TextureAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<TextureAsset>;
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
        .Task = LoadTextureFromImageTask(frameUploads, name, std::move(image), options),
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
        .Task = LoadTextureFromMemoryTask(frameUploads, name, std::move(encodedBytes), options),
        .DebugName = std::move(name)});
}

}  // namespace radray
