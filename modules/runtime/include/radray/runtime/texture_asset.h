#pragma once

#include <radray/hash.h>
#include <radray/image_data.h>
#include <radray/render/rhi.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>

namespace radray {

class FrameUploadScheduler;

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

/// GPU 贴图的【内容】。持有已上传的 device-local render::Texture + 一个默认全量 SRV
/// (render::TextureView), 并内建一个 view 缓存承载"同一贴图的非默认子 view"
/// (对应 UE5 挂在 texture 上的 FRHITextureViewCache)。
///
/// 【为何贴图需要内容/槽位分离】: 绑定点与描述符里存的是 render::TextureView* 裸指针,
/// 而写进描述符堆的 view 会被 GPU 用到 fence 之后。若 Unload 能直接带走 view, 那些
/// 描述符就在飞行中悬垂 —— 判据正是 asset.h 说的"存在无法枚举的指针持有者"。
///
/// 设计:
/// - 构造即完整: CPU 解码 + GPU 上传由加载协程在构造前完成, 内容一出生即可被采样绑定。
/// - 与 ImageAsset (纯 CPU) 解耦: ImageAsset 持像素, 本内容持 GPU 资源。
/// - view 所有权归本内容: 默认 SRV 存 _srv; 非默认子 view 经 GetOrCreateSrv 按 descriptor
///   去重, unique_ptr 永生缓存至内容归零。因此绑定点拿到的 view 指针在【持有一份
///   ContentRef 期间】永不悬垂, 材质快照只需存"content 引用 + 描述值", 零裸指针。
///
/// 【view 缓存与"内容不可变"不矛盾】: 缓存是纯派生数据 —— 同一个 TextureSubViewDesc 永远
/// 得到同一个 view, 填充顺序不改变任何观察结果。同 ShaderPassProgram 惰性编译字节码。
class TextureContent : public AssetContent {
public:
    TextureContent(
        AssetContentKey key,
        IRenderResourceRecycler& recycler,
        render::Device* device,
        string name,
        unique_ptr<render::Texture> texture,
        unique_ptr<render::TextureView> srv) noexcept;
    ~TextureContent() noexcept override;

    bool IsValid() const noexcept { return _texture != nullptr && _srv != nullptr; }

    const string& GetName() const noexcept { return _name; }
    render::Texture* GetTexture() const noexcept { return _texture.get(); }
    render::TextureView* GetSrv() const noexcept { return _srv.get(); }

    /// 按子 view 描述取 SRV。默认描述 (sub.IsDefault()) 直接返回 _srv;
    /// 否则按 descriptor 去重: 命中返回缓存指针, 未命中创建并永生缓存。
    /// device 为空 / 贴图无效 / 创建失败返回 nullptr。
    /// 返回指针在【本内容】存活期内稳定 —— 持有一份 ContentRef 即保证不悬垂。
    render::TextureView* GetOrCreateSrv(const TextureSubViewDesc& sub) noexcept;

protected:
    void ReleaseRenderResources(IRenderResourceRecycler& recycler) noexcept override;

private:
    render::Device* _device{nullptr};
    string _name;
    unique_ptr<render::Texture> _texture;
    unique_ptr<render::TextureView> _srv;
    unordered_map<TextureSubViewDesc, unique_ptr<render::TextureView>> _viewCache;
};

using TextureContentRef = AssetContentRef<TextureContent>;

/// GPU 贴图资产。对应 UE5 的 UTexture2D (最小化)。只做标识与槽位, 数据在 TextureContent。
class TextureAsset : public Asset {
public:
    explicit TextureAsset(TextureContentRef content) noexcept;
    ~TextureAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    /// 取内容的强引用。持有它期间内容保证存活, 即使本资产的槽位已被 Unload。
    ///
    /// 【刻意不提供 GetSrv / GetTexture 等转发】: 见 Asset 的说明。绑定热路径请把
    /// ContentRef 提出来存住 (材质快照本就该存它), 而不是每帧穿两层。
    TextureContentRef AcquireContent() const noexcept { return _content; }

    /// 内容是否仍挂在本槽位上。OnUnload 之后为 false。
    bool HasContent() const noexcept { return _content.HasValue(); }

private:
    TextureContentRef _content;
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

template <>
struct RuntimeTypeTrait<TextureAsset> {
    static constexpr RuntimeTypeId value{0x7c3e9a14, 0x8b2d, 0x4f61, 0xa9, 0x05, 0x3e, 0x6c, 0x1d, 0x82, 0x4b, 0x90};
    using Bases = std::tuple<Asset>;
};

}  // namespace radray
