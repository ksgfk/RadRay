#include <radray/runtime/texture_asset.h>

#include <array>

#include <fmt/format.h>

#include <radray/logger.h>

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
    (void)settings;
    // TODO: 待上层 GPU 上传调度设计确定后恢复纹理解码、mip 准备与上传加载。
    co_return AssetLoadResult::Failure(fmt::format("texture GPU upload is not implemented: '{}'", path.string()));
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

}  // namespace radray
