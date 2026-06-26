#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/render/tag_set.h>
#include <radray/runtime/render/keyword_set.h>

namespace radray::srp {

/// Shader 身份。等价 Unity ShaderId:一个 `Shader` 资产的唯一标识,
/// 参与 ShaderVariantCache 的 key。【Material 实例不进 key】,故海量实例共享变体。
struct ShaderId {
    uint64_t Value{0};

    friend bool operator==(ShaderId, ShaderId) noexcept = default;
};

/// 一个 keyword 轴的定义(对齐 URP 的 multi_compile / shader_feature 一轴)。
/// 最小化:一个轴 = 一组互斥的 define 名(其中空串表示"该轴关闭")。
/// 当前框架只用它做声明/校验;实际变体由 KeywordSet 直接驱动。
struct KeywordAxis {
    string Name;              ///< 轴名(诊断用)
    vector<string> Keywords;  ///< 该轴上的候选 keyword(define 名)
};

/// 一个 LightMode pass 的源描述。承载编译一段 VS/PS 所需的一切【除 keyword 之外】的信息:
/// shader 文件路径 + 入口点 + 该 pass 的 tag 集。space1 布局由编译后反射得到,不在此声明。
struct ShaderPassSource {
    string ShaderPath;        ///< HLSL 文件路径(GpuSystem 以 shaderlib 为 include 根)
    string ShaderName;        ///< 缓存身份名(空则取路径)
    string VsEntry{"VSMain"};
    std::optional<string> PsEntry{"PSMain"};  ///< nullopt = depth-only,无 PS
    TagSet Tags;              ///< 至少含 "LightMode" = <本 pass 的 lightMode>
};

/// 多 LightMode 代码容器。对应 Unity 的 .shader / ShaderGraph 产物:
///   一个 Shader 资产 = 多个 pass,每个 pass 带一个 LightMode 标签。
/// 它【不拥有任何编译产物】(那些落在 ShaderVariantCache),只描述"按 LightMode 能给出哪段源"。
///
/// 设计依据:srp_runtime_architecture.md §1/§7、srp_runtime_design.md §5。
class Shader {
public:
    Shader(ShaderId id, string name) noexcept : _id(id), _name(std::move(name)) {}
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&&) noexcept = default;
    Shader& operator=(Shader&&) noexcept = default;

    ShaderId Id() const noexcept { return _id; }
    std::string_view Name() const noexcept { return _name; }

    /// 注册一个 LightMode pass。source.Tags 必须含 "LightMode"。
    void AddPass(ShaderPassSource source);

    /// 本 shader 是否有某 LightMode 的 pass(第二层 relevance 的依据之一)。
    bool HasPass(std::string_view lightMode) const noexcept;

    /// 取某 LightMode 的 pass 源。不存在返回 nullptr。
    const ShaderPassSource* GetPassSource(std::string_view lightMode) const noexcept;

    /// 取某 LightMode 的完整 tag 集。不存在返回 nullptr。
    const TagSet* GetTags(std::string_view lightMode) const noexcept;

    /// 按 pass 想要的 LightMode 优先级列表,找本 shader 命中的那个。
    /// wanted[0] 最优先;命中即把它写入 *out 并返回 true;全不命中返回 false(relevance 失败)。
    /// 对齐 SRP:CreateDrawingSettings 用 SetShaderPassName(i, tag),index = 优先级。
    bool ResolveTag(const WantedLightModes& wanted, std::string_view* out) const noexcept;

    /// keyword 轴(诊断/校验用)。
    std::span<const KeywordAxis> KeywordAxes() const noexcept { return _axes; }
    void AddKeywordAxis(KeywordAxis axis) { _axes.push_back(std::move(axis)); }

private:
    ShaderId _id;
    string _name;
    vector<ShaderPassSource> _passes;  ///< 数量极小,线性查足够
    vector<KeywordAxis> _axes;
};

}  // namespace radray::srp
