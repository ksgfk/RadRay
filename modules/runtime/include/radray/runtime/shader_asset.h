#pragma once

#include <optional>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/render/shader_variant_cache.h>
#include <radray/runtime/asset.h>

namespace radray {

/// 一个 shader 的 keyword 声明表 (对应 Unity 的 shader_feature keyword 集合)。
///
/// 设计要点:
/// - 有序表, keyword 名字 -> 稳定 bit 位 (加入顺序即 bit 序)。
/// - 上限 64 个 keyword (投影为 uint64_t bitmask, 与 render::ShaderVariantKey::Bitmask 对齐)。
/// - 纯 CPU 逻辑, 不依赖 render device / DXC, 可 headless 测试。
class ShaderKeywordSet {
public:
    static constexpr uint32_t kMaxKeywords = 64;

    ShaderKeywordSet() noexcept = default;

    /// 追加一个 keyword。返回其 bit 序 (从 0 起)。
    /// 失败 (重复名字 / 超过 kMaxKeywords) 返回 nullopt 并记录错误。
    std::optional<uint32_t> Add(std::string_view name) noexcept;

    /// 查名字对应的 bit 序。未声明返回 nullopt。
    std::optional<uint32_t> IndexOf(std::string_view name) const noexcept;

    uint32_t Count() const noexcept { return static_cast<uint32_t>(_names.size()); }

    const vector<string>& GetNames() const noexcept { return _names; }

    /// 把一组启用的 keyword 名字投影为 bitmask。
    /// 未在本表声明的名字被忽略 (对应 Unity: 未知 keyword 不产生变体位)。
    uint64_t Project(std::span<const std::string_view> enabled) const noexcept;

    /// 把 bitmask 解析为喂给 DXC 的宏 token 列表 (形如 "NAME=1")。
    vector<string> ResolveDefines(uint64_t bitmask) const noexcept;

private:
    vector<string> _names;
    unordered_map<string, uint32_t> _index;
};

/// 一个顶点缓冲布局的拥有式描述 (render::VertexBufferLayout 的 Elements 是 span, 不拥有;
/// ShaderPassDesc 需长期持有, 故用拥有式 vector 存 element, 提交前再转 span)。
struct OwningVertexBufferLayout {
    uint64_t ArrayStride{0};
    render::VertexStepMode StepMode{render::VertexStepMode::Vertex};
    vector<render::VertexElement> Elements{};
};

/// 一个渲染 pass 的描述 (对应 Unity ShaderLab 里的一个 Pass)。
/// PassTag 对应 Unity 的 LightMode, 用于与 RenderPipelinePass 匹配。
struct ShaderPassDesc {
    string PassTag;
    string Source;
    string VertexEntry{"VSMain"};
    string PixelEntry{"PSMain"};
    // 该 pass 的固定渲染状态 (blend / depth / raster 等)。variant 只影响编译, 不影响这些。
    render::PrimitiveState Primitive{render::PrimitiveState::Default()};
    std::optional<render::DepthStencilState> DepthStencil{};
    render::MultiSampleState MultiSample{};
    vector<render::ColorTargetState> ColorTargets{};
    // 顶点输入布局 (拥有式)。为空表示无顶点缓冲输入 (如全屏三角靠 SV_VertexID)。
    vector<OwningVertexBufferLayout> VertexLayouts{};
    // shader 源里 #include 的搜索目录 (透传给 DXC 的 -I)。为空表示不额外加 include 根。
    vector<string> IncludeDirs{};
};

/// shader 资产 (对应 Unity 的一个 .shader / Shader 对象)。
///
/// - 持有 program 身份 ProgramId (构造时生成的稳定 Guid), 独立于 AssetManager 分配的 AssetId。
///   render::ShaderVariantCache 用 ProgramId 区分不同 program, 因此即使未入库也有效。
/// - 持有 keyword 表 (全 pass 共享) + pass 列表。
/// - GetOrCreateVariant 把 (pass, 启用 keyword) 投影为 render::ShaderVariantDescriptor,
///   驱动 render::ShaderVariantCache 懒编译并缓存, 返回编译好的变体。
class ShaderAsset : public Asset {
public:
    ShaderAsset() noexcept;
    explicit ShaderAsset(ShaderKeywordSet keywords, vector<ShaderPassDesc> passes) noexcept;
    ~ShaderAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    const Guid& GetProgramId() const noexcept { return _programId; }

    const ShaderKeywordSet& GetKeywords() const noexcept { return _keywords; }
    ShaderKeywordSet& GetKeywords() noexcept { return _keywords; }

    const vector<ShaderPassDesc>& GetPasses() const noexcept { return _passes; }

    /// 按 PassTag (=LightMode) 找 pass 序号。未找到返回 nullopt。
    std::optional<uint32_t> FindPassByTag(std::string_view passTag) const noexcept;

    /// 懒编译并缓存指定 pass 在给定启用 keyword 下的变体。
    /// enabled 中未声明的 keyword 被忽略。失败返回 nullptr。
    Nullable<const render::CompiledShaderVariant*> GetOrCreateVariant(
        render::ShaderVariantCache& cache,
        uint32_t passIndex,
        std::span<const std::string_view> enabledKeywords,
        render::HlslShaderModel sm = render::HlslShaderModel::SM60) noexcept;

private:
    Guid _programId;
    ShaderKeywordSet _keywords;
    vector<ShaderPassDesc> _passes;
};

template <>
struct RuntimeTypeTrait<ShaderAsset> {
    static constexpr RuntimeTypeId value{0x3f7c1a8e, 0x2d64, 0x4b91, 0xa5, 0x0e, 0x71, 0x3c, 0x9a, 0x28, 0x6d, 0xf4};
};

}  // namespace radray
