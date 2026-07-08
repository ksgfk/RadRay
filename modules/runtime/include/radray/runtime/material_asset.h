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
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/material_render_snapshot.h>

namespace radray {

class MaterialPropertyBlock;

/// 通过资产引用绑定一个【非默认子 view】的纹理 property。
/// Texture 提供 GPU 资源与 view 缓存所有权, SubView 描述与默认全量 SRV 的差异 (mip/array/format/dim)。
/// 绑定时经 TextureAsset::GetOrCreateSrv(SubView) 换成缓存中稳定的 view 指针, 无悬垂风险。
struct TextureSubViewRef {
    StreamingAssetRef<TextureAsset> Texture{};
    TextureSubViewDesc SubView{};
};

/// 一个材质属性的值 (对应 Unity Material 的 property)。
/// - Float / Vector: 按【属性名】写入常量。绑定时 MaterialConstantBinder 用 shader 反射
///   解析该名字是某 cbuffer 的【块名】(整块) 还是【块内字段名】(按偏移打包), 整块提交
///   (push constant 或 CBV)。对齐 Unity 的 SetFloat("_Color") 语义。
/// - Texture (StreamingAssetRef<TextureAsset>): 通过资产引用绑定默认全量 SRV, 绑定时取 GetSrv();
///   能安全跨线程/跨帧持有 (资产 generation 兜底悬垂)。
/// - Texture (TextureSubViewRef): 同一贴图的非默认子 view (mip/array/format 子集), 绑定时经
///   TextureAsset::GetOrCreateSrv 去重取稳定 view 指针。同样零裸指针、不悬垂。
/// - Sampler (SamplerDescriptor): 存状态描述值; 绑定时经 SamplerCache 去重取稳定 sampler 指针。
///   相比裸指针能安全跨线程/跨帧持有 (缓存永生, 指针不悬垂)。
using MaterialPropertyValue = std::variant<
    float,
    Eigen::Vector4f,
    vector<byte>,  // 原始常量块 (整块 push/root constant, 大小须与 shader cbuffer 完全一致)
    StreamingAssetRef<TextureAsset>,
    TextureSubViewRef,
    render::SamplerDescriptor>;

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
    /// 设置一整块常量数据 (对应一个完整 cbuffer 块; Name 为块名)。
    /// size 须与 shader 中该 cbuffer 声明的字节数一致 (超出被截断)。
    /// 若只想设单个字段, 用 SetFloat / SetVector 并以字段名命名即可 (打包器按偏移写入)。
    void SetConstantBlock(std::string_view name, const void* data, size_t size) noexcept;
    /// 通过资产引用设置纹理 (默认全量 SRV)。绑定时取 GetSrv(); 能安全跨线程/跨帧持有。
    void SetTexture(std::string_view name, StreamingAssetRef<TextureAsset> texture) noexcept;
    /// 通过资产引用 + 子 view 描述设置纹理 (同一贴图的非默认 view: mip/array/format 子集)。
    /// 绑定时经 TextureAsset::GetOrCreateSrv(sub) 去重取稳定 view 指针; 同样零裸指针、不悬垂。
    void SetTexture(std::string_view name, StreamingAssetRef<TextureAsset> texture, const TextureSubViewDesc& sub) noexcept;
    /// 设置采样器 (存 descriptor 值)。绑定时经 SamplerCache 去重取稳定指针,
    /// 相比裸指针能安全跨线程/跨帧持有。
    void SetSampler(std::string_view name, const render::SamplerDescriptor& desc) noexcept;

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

    /// 把 property 值写入给定的 ShaderParameterTable。sampler 经 samplerCache 按 descriptor
    /// 去重取稳定指针。返回成功写入的 property 数 (被 shader 接受的)。
    uint32_t ApplyProperties(render::ShaderParameterTable& table, SamplerCache& samplerCache) const noexcept;

    /// 生成渲染侧绘制决策快照 (由组件在 game 线程 Tick 时按需调用)。冻结当前 shader/keyword/
    /// renderQueue/常量/纹理/采样器, 供 render 线程无锁只读。
    /// 注意: 本类是共享只读模板; 快照的所有权与生产决策归调用它的组件。
    shared_ptr<const MaterialRenderSnapshot> CreateSnapshot() const noexcept;

    /// 生成叠加了 per-primitive 覆盖 (MaterialPropertyBlock) 的快照 (对应 Unity 的
    /// Renderer.SetPropertyBlock)。覆盖值按名字盖在模板 property 之上 (同名替换、新名追加),
    /// 共享 MaterialAsset 不被修改。overrides 为空 (nullptr / IsEmpty) 时等价于无参版本。
    /// shader / keyword / renderQueue 仍取自模板 (PropertyBlock 不改变体)。
    shared_ptr<const MaterialRenderSnapshot> CreateSnapshot(const MaterialPropertyBlock* overrides) const noexcept;

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
