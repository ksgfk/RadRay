#pragma once

#include <filesystem>

#include <radray/hash.h>
#include <radray/image_data.h>
#include <radray/render/common.h>
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

    bool IsDefault() const noexcept {
        const render::SubresourceRange all = render::SubresourceRange::AllSub();
        return Dim == render::TextureDimension::Dim2D &&
               Format == render::TextureFormat::UNKNOWN &&
               Range.BaseArrayLayer == all.BaseArrayLayer &&
               Range.ArrayLayerCount == all.ArrayLayerCount &&
               Range.BaseMipLevel == all.BaseMipLevel &&
               Range.MipLevelCount == all.MipLevelCount;
    }
};

/// TextureAsset view 缓存的纯 POD key (仿 render::GraphicsPsoKey / SamplerKey)。
///
/// 所有字段为标量, 无指针 / optional / span。构造时以 `TextureViewKey{}` 清零, 再逐字段赋值,
/// 保证 padding 恒为 0, 从而可安全用于 PodHasher (byte-wise xxHash) 与 PodEqual (memcmp)。
struct TextureViewKey {
    int32_t Dim;
    int32_t Format;
    uint32_t BaseArrayLayer;
    uint32_t ArrayLayerCount;
    uint32_t BaseMipLevel;
    uint32_t MipLevelCount;
};

static_assert(std::is_trivially_copyable_v<TextureViewKey>, "TextureViewKey must be trivially copyable");

/// 从 TextureSubViewDesc 构造清零的 POD key。
TextureViewKey BuildTextureViewKey(const TextureSubViewDesc& desc) noexcept;

/// GPU 贴图资产。对应 UE5 的 UTexture2D(最小化):持有已上传的 device-local
/// render::Texture + 一个默认全量 SRV(render::TextureView), 并内建一个 view 缓存
/// 承载"同一贴图的非默认子 view"(对应 UE5 挂在 texture 上的 FRHITextureViewCache)。
///
/// 设计:
/// - 构造即完整:CPU 解码 + GPU 上传由加载协程在构造前完成,资产入库即可被采样绑定。
/// - 与 ImageAsset(纯 CPU)解耦:ImageAsset 持像素,TextureAsset 持 GPU 资源。
/// - view 所有权归本资产: 默认 SRV 存 _srv; 非默认子 view 经 GetOrCreateSrv 按 descriptor 去重,
///   unique_ptr 永生缓存至资产卸载。因此绑定点拿到的 view 指针在资产存活期内【永不悬垂】,
///   材质快照只需存"asset 引用 + 描述值", 零裸指针。
/// - OnUnload 释放 GPU 资源 (含整个 view 缓存)。
class TextureAsset : public Asset {
public:
    TextureAsset() noexcept = default;
    TextureAsset(
        render::Device* device,
        string name,
        unique_ptr<render::Texture> texture,
        unique_ptr<render::TextureView> srv) noexcept;
    ~TextureAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    bool IsValid() const noexcept { return _texture != nullptr && _srv != nullptr; }

    const string& GetName() const noexcept { return _name; }
    render::Texture* GetTexture() const noexcept { return _texture.get(); }
    render::TextureView* GetSrv() const noexcept { return _srv.get(); }

    /// 按子 view 描述取 SRV。默认描述 (sub.IsDefault()) 直接返回 _srv;
    /// 否则按 descriptor 去重: 命中返回缓存指针, 未命中创建并永生缓存。
    /// device 为空 / 贴图无效 / 创建失败返回 nullptr。返回指针在资产存活期内稳定不悬垂。
    render::TextureView* GetOrCreateSrv(const TextureSubViewDesc& sub) noexcept;

private:
    render::Device* _device{nullptr};
    string _name;
    unique_ptr<render::Texture> _texture;
    unique_ptr<render::TextureView> _srv;
    unordered_map<TextureViewKey, unique_ptr<render::TextureView>, PodHasher<TextureViewKey>, PodEqual<TextureViewKey>> _viewCache;
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
