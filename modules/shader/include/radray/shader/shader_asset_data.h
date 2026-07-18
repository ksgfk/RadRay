#pragma once

#include <optional>
#include <string_view>
#include <variant>

#include <radray/guid.h>
#include <radray/shader/common.h>
#include <radray/shader/hlsl.h>
#include <radray/types.h>

namespace radray::shader {

inline constexpr uint32_t kMaxColorTargets = 8;

enum class ShaderKeywordScope : uint8_t {
    Local,
    Global,
};

enum class CompareFunction : int32_t {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

enum class CullMode : int32_t {
    Front,
    Back,
    None,
};

enum class StencilOperation : int32_t {
    Keep,
    Zero,
    Replace,
    Invert,
    IncrementClamp,
    DecrementClamp,
    IncrementWrap,
    DecrementWrap,
};

enum class BlendFactor : int32_t {
    Zero,
    One,
    Src,
    OneMinusSrc,
    SrcAlpha,
    OneMinusSrcAlpha,
    Dst,
    OneMinusDst,
    DstAlpha,
    OneMinusDstAlpha,
    SrcAlphaSaturated,
    Constant,
    OneMinusConstant,
    Src1,
    OneMinusSrc1,
    Src1Alpha,
    OneMinusSrc1Alpha,
};

enum class BlendOperation : int32_t {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max,
};

enum class ColorWrite : uint32_t {
    Red = 0x1,
    Green = 0x2,
    Blue = 0x4,
    Alpha = 0x8,
    Color = Red | Green | Blue,
    All = Red | Green | Blue | Alpha,
};

}  // namespace radray::shader

namespace radray {

template <>
struct is_flags<shader::ColorWrite> : public std::true_type {};

}  // namespace radray

namespace radray::shader {

using ColorWrites = EnumFlags<ColorWrite>;

struct BlendComponent {
    BlendFactor Src{BlendFactor::One};
    BlendFactor Dst{BlendFactor::Zero};
    BlendOperation Op{BlendOperation::Add};

    friend bool operator==(const BlendComponent&, const BlendComponent&) = default;
};

struct BlendState {
    BlendComponent Color{};
    BlendComponent Alpha{};

    friend bool operator==(const BlendState&, const BlendState&) = default;
};

struct StencilFaceState {
    CompareFunction Compare{CompareFunction::Always};
    StencilOperation FailOp{StencilOperation::Keep};
    StencilOperation DepthFailOp{StencilOperation::Keep};
    StencilOperation PassOp{StencilOperation::Keep};

    friend bool operator==(const StencilFaceState&, const StencilFaceState&) = default;
};

struct StencilState {
    StencilFaceState Front{};
    StencilFaceState Back{};
    uint32_t ReadMask{0xff};
    uint32_t WriteMask{0xff};

    friend bool operator==(const StencilState&, const StencilState&) = default;
};

struct ShaderTagDesc {
    string Name;
    string Value;

    friend bool operator==(const ShaderTagDesc&, const ShaderTagDesc&) = default;
};

struct ShaderKeywordGroupDesc {
    vector<string> Alternatives;
    ShaderKeywordScope Scope{ShaderKeywordScope::Local};
    ShaderStages Stages{ShaderStage::UNKNOWN};

    friend bool operator==(const ShaderKeywordGroupDesc&, const ShaderKeywordGroupDesc&) = default;
};

struct ShaderVariantKey {
    vector<string> Defines;

    friend bool operator==(const ShaderVariantKey&, const ShaderVariantKey&) = default;
};

using ShaderVariantDesc = ShaderVariantKey;

struct ShaderColorTargetDesc {
    uint32_t Index{0};
    std::optional<BlendState> Blend{};
    ColorWrites WriteMask{ColorWrite::All};

    friend bool operator==(const ShaderColorTargetDesc&, const ShaderColorTargetDesc&) = default;
};

struct ShaderStencilTestDesc {
    uint32_t Reference{0};
    StencilState State{};

    friend bool operator==(const ShaderStencilTestDesc&, const ShaderStencilTestDesc&) = default;
};

struct ShaderGraphicsPassDesc {
    string VertexEntry;
    std::optional<string> PixelEntry;
    vector<ShaderColorTargetDesc> ColorTargets;
    CullMode Cull{CullMode::Back};
    std::optional<CompareFunction> Depth{CompareFunction::LessEqual};
    float DepthBiasFactor{0.0f};
    float DepthBiasUnits{0.0f};
    std::optional<ShaderStencilTestDesc> Stencil{};
    bool DepthWrite{true};
    bool DepthClip{true};
    bool AlphaToMask{false};
    bool ConservativeRasterization{false};

    friend bool operator==(const ShaderGraphicsPassDesc&, const ShaderGraphicsPassDesc&) = default;
};

struct ShaderComputePassDesc {
    string EntryPoint;

    friend bool operator==(const ShaderComputePassDesc&, const ShaderComputePassDesc&) = default;
};

using ShaderPassProgramDesc = std::variant<ShaderGraphicsPassDesc, ShaderComputePassDesc>;

struct ShaderPassDesc {
    string Name;
    string SourcePath;
    vector<string> IncludeDirs;
    HlslShaderModel SM{HlslShaderModel::SM60};
    vector<ShaderKeywordGroupDesc> KeywordGroups;
    vector<ShaderVariantDesc> Variants;
    vector<ShaderTagDesc> Tags;
    ShaderPassProgramDesc Program;
    bool IsOptimize{true};
    bool EnableUnbounded{true};

    friend bool operator==(const ShaderPassDesc&, const ShaderPassDesc&) = default;
};

struct ShaderAssetData {
    Guid AssetId{};
    vector<ShaderPassDesc> Passes;

    friend bool operator==(const ShaderAssetData&, const ShaderAssetData&) = default;
};

void NormalizeShaderDefines(vector<string>& defines);
bool IsShaderAssetDataValid(const ShaderAssetData& asset, bool requireVariants = false) noexcept;
bool AreShaderDefinesValid(const ShaderPassDesc& pass, ShaderStage stage, const vector<string>& defines) noexcept;
bool DoesShaderDefineAffectStage(
    const ShaderPassDesc& pass,
    std::string_view define,
    ShaderStage stage) noexcept;
bool IsDeclaredShaderVariant(const ShaderPassDesc& pass, const vector<string>& defines) noexcept;
std::optional<std::string_view> FindShaderEntryPoint(const ShaderPassDesc& pass, ShaderStage stage) noexcept;

}  // namespace radray::shader
