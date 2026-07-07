#pragma once

#include <optional>
#include <variant>

#include <radray/types.h>
#include <radray/basic_math.h>
#include <radray/render/common.h>
#include <radray/render/shader_variant_cache.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/texture_asset.h>

namespace radray {

/// 一个材质属性的值 (对应 Unity Material 的 property)。
/// - Float / Vector: 写入常量 (cbuffer, 通过 ShaderParameterTable::SetBytes)。
/// - Texture (裸 TextureView*): 绑定资源 (非拥有指针, 生命周期由资源持有方管理)。
/// - Texture (StreamingAssetRef<TextureAsset>): 通过资产引用绑定, 绑定时取 GetSrv();
///   相比裸指针能安全跨线程/跨帧持有 (资产 generation 兜底悬垂)。
/// - Sampler: 绑定资源 (非拥有指针, 生命周期由资源持有方管理)。
using MaterialPropertyValue = std::variant<
    float,
    Eigen::Vector4f,
    vector<byte>,  // 原始常量块 (整块 push/root constant, 大小须与 shader cbuffer 完全一致)
    render::TextureView*,
    StreamingAssetRef<TextureAsset>,
    render::Sampler*>;

/// 渲染队列排序值 (对应 Unity 的 Material.renderQueue)。
/// 数值越小越先绘制; >= Transparent 的走 back-to-front 半透明排序。
enum class RenderQueue : int32_t {
    Background = 1000,
    Geometry = 2000,
    AlphaTest = 2450,
    GeometryLast = 2500,  // 不透明与半透明的分界
    Transparent = 3000,
    Overlay = 4000,
};

class MaterialAsset;

/// 材质的渲染侧【只读值快照】(对应 UE5 的 FMaterialRenderProxy 的最小化, 但走不可变快照而非
/// enqueue-to-RT 模型)。
///
/// 设计要点:
/// - game 线程在组件 Tick 时由 MaterialAsset::CreateSnapshot() 生成, 之后不可变;
///   render 线程只读, 无锁无竞争 (通过 atomic<shared_ptr<const MaterialRenderSnapshot>> 发布)。
/// - 冻结渲染所需的一切: shader 引用 (稳定 ProgramId + pass) + 启用 keyword + renderQueue
///   + 常量字节块 + 纹理引用 (StreamingAssetRef) + 采样器裸指针。
/// - shared_ptr 引用计数自动管理生命周期: DrawItem 持一份 shared_ptr, 快照存活期覆盖整个渲染,
///   即便 MaterialAsset 在此期间被 Unload 也不悬垂。
/// - ResolveVariant / ApplyProperties / IsTransparent 从快照自身求解, 不再回查 MaterialAsset。
struct MaterialRenderSnapshot {
    /// 一条常量块 property (对应一个 push/root constant cbuffer)。
    struct ConstantEntry {
        string Name;
        vector<byte> Bytes;
    };
    /// 一条纹理 property。Texture 有效时绑定其 GetSrv(); 否则回退 RawView (可为空)。
    struct TextureEntry {
        string Name;
        StreamingAssetRef<TextureAsset> Texture{};
        render::TextureView* RawView{nullptr};
    };
    /// 一条采样器 property (非拥有; app 自管, 生命周期须覆盖渲染)。
    struct SamplerEntry {
        string Name;
        render::Sampler* Sampler{nullptr};
    };

    /// shader 引用 (非拥有 streaming ref)。ResolveVariant 用它懒编译变体。
    StreamingAssetRef<ShaderAsset> Shader{};
    /// 启用的 keyword 名字 (投影用)。
    vector<string> EnabledKeywords;
    int32_t RenderQueue{static_cast<int32_t>(RenderQueue::Geometry)};
    vector<ConstantEntry> Constants;
    vector<TextureEntry> Textures;
    vector<SamplerEntry> Samplers;

    bool IsTransparent() const noexcept {
        return RenderQueue >= static_cast<int32_t>(RenderQueue::Transparent);
    }

    /// 解析指定 pass 在快照 keyword 集下的变体。shader 空 / pass 越界 / 编译失败返回 nullptr。
    Nullable<const render::CompiledShaderVariant*> ResolveVariant(
        render::ShaderVariantCache& cache,
        uint32_t passIndex,
        render::HlslShaderModel sm = render::HlslShaderModel::SM60) const noexcept;

    /// 把快照的 property 值写入 ShaderParameterTable。返回成功写入的 property 数。
    uint32_t ApplyProperties(render::ShaderParameterTable& table) const noexcept;
};

/// 材质资产 (对应 Unity 的一个 Material)。
///
/// 设计要点:
/// - 引用一个 ShaderAsset (非拥有), 提供 property 值 + 启用的 keyword 集。
/// - keyword 集只记录名字; 投影为 bitmask / variant 由被引用的 ShaderAsset 完成。
/// - property 按名字写入 ShaderParameterTable。未在 shader 中出现的名字被 SetXxx 忽略。
/// - 属性/keyword 的读写是纯 CPU 策略, 可 headless 测试;
///   ResolveVariant / ApplyProperties 需要 device 支撑的变体缓存 / 参数表。
class MaterialAsset : public Asset {
public:
    MaterialAsset() noexcept = default;
    explicit MaterialAsset(StreamingAssetRef<ShaderAsset> shader) noexcept;
    ~MaterialAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    StreamingAssetRef<ShaderAsset> GetShader() const noexcept { return _shader; }
    void SetShader(StreamingAssetRef<ShaderAsset> shader) noexcept { _shader = std::move(shader); }

    // ─── render queue ───
    int32_t GetRenderQueue() const noexcept { return _renderQueue; }
    void SetRenderQueue(int32_t queue) noexcept { _renderQueue = queue; }
    void SetRenderQueue(RenderQueue queue) noexcept { _renderQueue = static_cast<int32_t>(queue); }
    /// 队列值 >= Transparent 视为半透明 (走 back-to-front 排序)。
    bool IsTransparent() const noexcept { return _renderQueue >= static_cast<int32_t>(RenderQueue::Transparent); }

    // ─── property ───
    void SetFloat(std::string_view name, float value) noexcept;
    void SetVector(std::string_view name, const Eigen::Vector4f& value) noexcept;
    /// 设置一整块常量数据 (对应一个 push/root constant cbuffer 块)。
    /// size 须与 shader 中该 cbuffer 声明的字节数完全一致, 否则 ApplyProperties 时被忽略。
    void SetConstantBlock(std::string_view name, const void* data, size_t size) noexcept;
    void SetTexture(std::string_view name, render::TextureView* view) noexcept;
    /// 通过资产引用设置纹理 (推荐)。绑定时取 GetSrv(); 相比裸指针能安全跨线程/跨帧持有。
    void SetTexture(std::string_view name, StreamingAssetRef<TextureAsset> texture) noexcept;
    void SetSampler(std::string_view name, render::Sampler* sampler) noexcept;

    /// 查属性值。未设置返回 nullopt。
    std::optional<MaterialPropertyValue> GetProperty(std::string_view name) const noexcept;
    std::optional<float> GetFloat(std::string_view name) const noexcept;
    std::optional<Eigen::Vector4f> GetVector(std::string_view name) const noexcept;

    const unordered_map<string, MaterialPropertyValue>& GetProperties() const noexcept { return _properties; }

    // ─── keyword ───
    /// 启用一个 keyword (仅记录名字; 是否影响变体取决于 shader 是否声明了它)。
    void EnableKeyword(std::string_view name) noexcept;
    void DisableKeyword(std::string_view name) noexcept;
    bool IsKeywordEnabled(std::string_view name) const noexcept;

    /// 返回启用的 keyword 名字 (按启用顺序; 供 ShaderAsset 投影)。
    const vector<string>& GetEnabledKeywords() const noexcept { return _enabledKeywords; }

    /// 解析指定 pass 在当前 keyword 集下的变体。shader 为空 / pass 越界 / 编译失败返回 nullptr。
    Nullable<const render::CompiledShaderVariant*> ResolveVariant(
        render::ShaderVariantCache& cache,
        uint32_t passIndex,
        render::HlslShaderModel sm = render::HlslShaderModel::SM60) noexcept;

    /// 把 property 值写入给定的 ShaderParameterTable。
    /// 返回成功写入的 property 数 (被 shader 接受的)。
    uint32_t ApplyProperties(render::ShaderParameterTable& table) const noexcept;

    /// 生成渲染侧只读值快照 (在 game 线程组件 Tick 时调用)。冻结当前 shader/keyword/
    /// renderQueue/常量/纹理/采样器, 供 render 线程无锁只读。
    shared_ptr<const MaterialRenderSnapshot> CreateSnapshot() const noexcept;

private:
    StreamingAssetRef<ShaderAsset> _shader{};
    int32_t _renderQueue{static_cast<int32_t>(RenderQueue::Geometry)};
    unordered_map<string, MaterialPropertyValue> _properties;
    vector<string> _enabledKeywords;
    unordered_map<string, uint32_t> _keywordIndex;  // 名字 -> 在 _enabledKeywords 的下标
};

template <>
struct RuntimeTypeTrait<MaterialAsset> {
    static constexpr RuntimeTypeId value{0x8b1e4d2a, 0x9f37, 0x4c60, 0xb8, 0x14, 0x5a, 0x6d, 0x0e, 0x92, 0x3f, 0x71};
};

}  // namespace radray
