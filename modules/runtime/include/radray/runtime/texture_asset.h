#pragma once

#include <filesystem>

#include <radray/hash.h>
#include <radray/image_data.h>
#include <radray/render/rhi.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_database.h>
#include <radray/runtime/asset_manager.h>

namespace radray {

class FrameUploadScheduler;

class TextureImportSettings;

template <>
struct RuntimeTypeTrait<TextureImportSettings> {
    static constexpr RuntimeTypeId value{0xbb83ae65, 0x95ec, 0x4737, 0xb9, 0x02, 0x64, 0x64, 0x72, 0xec, 0x6d, 0x9c};
};

class TextureImportSettings final : public AssetImportSettings {
public:
    bool Deserialize(const JsonValue& json) override;
    bool Serialize(JsonWriteContext& context) const noexcept override;

    bool Srgb{true};
    bool GenerateMips{true};
};

/// 一个【非默认 SRV】的差异描述值 (对应 UE5 的 FRHITextureSRVCreateInfo)。
///
/// 只承载 view 与"默认全量 SRV"不同的维度: dimension / format / 子资源范围。
/// 纯值语义, 可自由拷贝 / 跨帧 / 跨线程持有 (无裸指针)。绑定时经 TextureAsset::GetOrCreateSrv
/// 换成缓存中稳定的 render::TextureView*。默认构造 (== Default()) 表示"用默认全量 SRV"。
struct TextureSubViewDesc {
    render::TextureDimension Dim{render::TextureDimension::Dim2D};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};  // UNKNOWN = 沿用底层贴图格式
    render::SubresourceRange Range{render::SubresourceRange::AllSub()};

    /// 默认全量 SRV 描述 (等价于 TextureAsset 构造时建的 _srv)。
    static TextureSubViewDesc Default() noexcept { return TextureSubViewDesc{}; }

    constexpr bool IsDefault() const noexcept {
        constexpr render::SubresourceRange all = render::SubresourceRange::AllSub();
        return Dim == render::TextureDimension::Dim2D &&
               Format == render::TextureFormat::UNKNOWN &&
               Range.BaseArrayLayer == all.BaseArrayLayer &&
               Range.ArrayLayerCount == all.ArrayLayerCount &&
               Range.BaseMipLevel == all.BaseMipLevel &&
               Range.MipLevelCount == all.MipLevelCount;
    }

    friend bool operator==(const TextureSubViewDesc& lhs, const TextureSubViewDesc& rhs) noexcept = default;
};

}  // namespace radray

namespace std {

template <>
struct hash<radray::TextureSubViewDesc> {
    size_t operator()(const radray::TextureSubViewDesc& desc) const noexcept;
};

}  // namespace std

namespace radray {

/// GPU 贴图资产。对应 UE5 的 UTexture2D (最小化)。持有已上传的 device-local
/// render::Texture + 默认全量 SRV, 并内建一个按 TextureSubViewDesc 去重的子 view 缓存
/// (对应 UE5 挂在 texture 上的 FRHITextureViewCache)。
///
/// 构造即完整 (CPU 解码 + GPU 上传由加载协程在构造前完成), 与纯 CPU 的 ImageAsset 解耦。
/// view 所有权归本资产且永生至资产销毁, 故绑定点拿到的 view 指针在持有一份
/// StreamingAssetRef 期间永不悬垂 —— 材质快照只需存 "ref + 描述值", 零裸指针。
class TextureAsset : public Asset {
public:
    TextureAsset(
        render::Device* device,
        string name,
        unique_ptr<render::Texture> texture,
        unique_ptr<render::TextureView> srv) noexcept;
    ~TextureAsset() noexcept override;

    void OnUnload(AssetManager& manager) override;

    bool IsValid() const noexcept { return _texture != nullptr && _srv != nullptr; }

    const string& GetName() const noexcept { return _name; }
    render::Texture* GetTexture() const noexcept { return _texture.get(); }
    render::TextureView* GetSrv() const noexcept { return _srv.get(); }

    /// 按子 view 描述取 SRV。默认描述 (sub.IsDefault()) 直接返回 _srv;
    /// 否则按 descriptor 去重: 命中返回缓存指针, 未命中创建并永生缓存。
    /// device 为空 / 贴图无效 / 创建失败返回 nullptr。
    /// 返回指针在【本资产】存活期内稳定 —— 持有一份 StreamingAssetRef 即保证不悬垂。
    render::TextureView* GetOrCreateSrv(const TextureSubViewDesc& sub) noexcept;

private:
    render::Device* _device{nullptr};
    string _name;
    unique_ptr<render::Texture> _texture;
    unique_ptr<render::TextureView> _srv;
    unordered_map<TextureSubViewDesc, unique_ptr<render::TextureView>> _viewCache;
};

struct TextureAssetLoadOptions {
    /// true 时按 sRGB 解释纹理(GPU 采样时做 sRGB→linear)。base color / emissive 用 true;
    /// normal / metallic-roughness / occlusion 用 false。
    bool Srgb{false};
    /// true 时在 CPU 侧生成完整 RGBA8 mip 链并逐级上传。
    bool GenerateMips{false};
    /// 解码失败时的回退像素(CPU)。为空时加载失败。
    ImageData FallbackImage{};
};

/// importer 级构造任务：只产出 AssetLoadResult，不创建 AssetManager slot。
task<AssetLoadResult> CreateTextureAssetFromImage(
    FrameUploadScheduler& frameUploads,
    string name,
    ImageData image,
    TextureAssetLoadOptions options = {});

task<AssetLoadResult> CreateTextureAssetFromMemory(
    FrameUploadScheduler& frameUploads,
    string name,
    vector<byte> encodedBytes,
    TextureAssetLoadOptions options = {});

class TextureImporter final : public TypedAssetImporter<TextureImportSettings> {
public:
    explicit TextureImporter(FrameUploadScheduler& frameUploads) noexcept;

    std::string_view GetTypeName() const noexcept override;
    std::span<const std::string_view> GetFileExtensions() const noexcept override;

protected:
    task<AssetLoadResult> LoadTyped(
        std::filesystem::path path,
        TextureImportSettings settings) override;

private:
    FrameUploadScheduler& _frameUploads;
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

template <>
struct RuntimeTypeTrait<TextureAsset> {
    static constexpr RuntimeTypeId value{0x7c3e9a14, 0x8b2d, 0x4f61, 0xa9, 0x05, 0x3e, 0x6c, 0x1d, 0x82, 0x4b, 0x90};
};

}  // namespace radray
