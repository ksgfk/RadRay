#pragma once

#include <optional>
#include <string_view>
#include <variant>

#include <radray/render/common.h>
#include <radray/runtime/asset.h>

namespace radray {

enum class ShaderKeywordScope {
    Local,
    Global,
};

struct ShaderTagDesc {
    string Name;
    string Value;
};

struct ShaderKeywordGroupDesc {
    /// 空字符串表示无定义
    vector<string> Alternatives;
    ShaderKeywordScope Scope{ShaderKeywordScope::Local};
    /// UNKNOWN 表示当前 program 的全部 stage
    render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
};

struct ShaderColorTargetDesc {
    uint32_t Index{0};
    std::optional<render::BlendState> Blend{std::nullopt};
    render::ColorWrites WriteMask{render::ColorWrite::All};
};

struct ShaderStencilTestDesc {
    uint32_t Reference{0};
    render::StencilState State{render::StencilState::Default()};
};

struct ShaderGraphicsPassDesc {
    string VertexEntry;
    std::optional<string> PixelEntry;
    vector<ShaderColorTargetDesc> ColorTargets;
    render::CullMode Cull{render::CullMode::Back};
    std::optional<render::CompareFunction> Depth{render::CompareFunction::LessEqual};
    float DepthBiasFactor{0.0f};
    float DepthBiasUnits{0.0f};
    std::optional<ShaderStencilTestDesc> Stencil{std::nullopt};
    bool DepthWrite{true};
    bool DepthClip{true};
    bool AlphaToMask{false};
    bool ConservativeRasterization{false};
};

struct ShaderComputePassDesc {
    string EntryPoint;
};

using ShaderPassProgramDesc = std::variant<ShaderGraphicsPassDesc, ShaderComputePassDesc>;

struct ShaderPassDesc {
    string Name;
    /// 相对于 shader source root 的 UTF-8 路径，使用 '/' 分隔。
    string SourcePath;
    vector<ShaderKeywordGroupDesc> KeywordGroups;
    vector<ShaderTagDesc> Tags;
    ShaderPassProgramDesc Program;
};

/// Shader 的纯 CPU 声明资产。Pass 顺序在资产生命周期内保持稳定，可作为后续 variant/PSO
/// 缓存的 Pass 下标；编译产物和 GPU 对象不由本资产持有。
class ShaderAsset final : public Asset {
public:
    ShaderAsset() noexcept = default;
    explicit ShaderAsset(vector<ShaderPassDesc> passes) noexcept;
    ~ShaderAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;

    AssetTypeId GetTypeId() const noexcept override;

    bool IsValid() const noexcept;

    const vector<ShaderPassDesc>& GetPasses() const noexcept { return _passes; }

    /// Pass Name 允许为空或重复；与 Unity 一样，查询返回第一个匹配项。
    std::optional<uint32_t> FindPassByName(std::string_view name) const noexcept;

    /// 按 Tag 的 name/value 查找，返回第一个匹配 Pass 的下标。
    std::optional<uint32_t> FindPassByTag(std::string_view name, std::string_view value) const noexcept;

private:
    vector<ShaderPassDesc> _passes;
};

template <>
struct RuntimeTypeTrait<ShaderAsset> {
    static constexpr RuntimeTypeId value{0x1ed35d36, 0xfc77, 0x456e, 0xa9, 0x10, 0x5c, 0xa4, 0x49, 0x69, 0x57, 0xb3};
    using Bases = std::tuple<Asset>;
};

}  // namespace radray
