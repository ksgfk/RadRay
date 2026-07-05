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

namespace radray {

/// 一个材质属性的值 (对应 Unity Material 的 property)。
/// - Float / Vector: 写入常量 (cbuffer, 通过 ShaderParameterTable::SetBytes)。
/// - Texture / Sampler: 绑定资源 (非拥有指针, 生命周期由资源持有方管理)。
using MaterialPropertyValue = std::variant<
    float,
    Eigen::Vector4f,
    vector<byte>,  // 原始常量块 (整块 push/root constant, 大小须与 shader cbuffer 完全一致)
    render::TextureView*,
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
