#include <radray/shader/shader_asset_data.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace radray::shader {
namespace {

bool IsUtf8Valid(std::string_view value) noexcept {
    const auto* data = reinterpret_cast<const uint8_t*>(value.data());
    size_t i = 0;
    while (i < value.size()) {
        const uint8_t lead = data[i++];
        if (lead <= 0x7f) continue;

        size_t continuationCount = 0;
        uint8_t firstMinimum = 0x80;
        uint8_t firstMaximum = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) {
            continuationCount = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            continuationCount = 2;
            if (lead == 0xe0) firstMinimum = 0xa0;
            if (lead == 0xed) firstMaximum = 0x9f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            continuationCount = 3;
            if (lead == 0xf0) firstMinimum = 0x90;
            if (lead == 0xf4) firstMaximum = 0x8f;
        } else {
            return false;
        }
        if (continuationCount > value.size() - i ||
            data[i] < firstMinimum || data[i] > firstMaximum) {
            return false;
        }
        ++i;
        for (size_t j = 1; j < continuationCount; ++j, ++i) {
            if (data[i] < 0x80 || data[i] > 0xbf) return false;
        }
    }
    return true;
}

template <typename Enum>
bool IsEnumInRange(Enum value, Enum maximum) noexcept {
    using Underlying = std::underlying_type_t<Enum>;
    const Underlying raw = static_cast<Underlying>(value);
    if constexpr (std::is_signed_v<Underlying>) {
        if (raw < 0) return false;
    }
    return raw <= static_cast<Underlying>(maximum);
}

bool IsBlendComponentValid(const BlendComponent& component) noexcept {
    return IsEnumInRange(component.Src, BlendFactor::OneMinusSrc1Alpha) &&
           IsEnumInRange(component.Dst, BlendFactor::OneMinusSrc1Alpha) &&
           IsEnumInRange(component.Op, BlendOperation::Max);
}

bool IsStencilFaceValid(const StencilFaceState& face) noexcept {
    return IsEnumInRange(face.Compare, CompareFunction::Always) &&
           IsEnumInRange(face.FailOp, StencilOperation::DecrementWrap) &&
           IsEnumInRange(face.DepthFailOp, StencilOperation::DecrementWrap) &&
           IsEnumInRange(face.PassOp, StencilOperation::DecrementWrap);
}

bool IsRelativeResourcePathValid(std::string_view path, bool allowEmpty) noexcept {
    if (path.empty()) {
        return allowEmpty;
    }
    if (!IsUtf8Valid(path) || path.front() == '/' || path.front() == '\\' ||
        path.find('\\') != std::string_view::npos) {
        return false;
    }
    size_t begin = 0;
    while (begin < path.size()) {
        const size_t separator = path.find('/', begin);
        const size_t end = separator == std::string_view::npos ? path.size() : separator;
        const std::string_view component = path.substr(begin, end - begin);
        if (component.empty() || component == "." || component == ".." ||
            component.find(':') != std::string_view::npos || component.find('\0') != std::string_view::npos) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        begin = separator + 1;
    }
    return false;
}

uint32_t GetProgramStageMask(const ShaderPassDesc& pass) noexcept {
    if (const auto* graphics = std::get_if<ShaderGraphicsPassDesc>(&pass.Program)) {
        uint32_t result = static_cast<uint32_t>(ShaderStage::Vertex);
        if (graphics->PixelEntry.has_value()) {
            result |= static_cast<uint32_t>(ShaderStage::Pixel);
        }
        return result;
    }
    return static_cast<uint32_t>(ShaderStage::Compute);
}

bool IsKeywordGroupValid(const ShaderKeywordGroupDesc& group, uint32_t programStages) noexcept {
    if (group.Alternatives.empty() || !IsEnumInRange(group.Scope, ShaderKeywordScope::Global)) {
        return false;
    }
    const uint32_t stages = group.Stages.value();
    if (stages != 0 && (stages & ~programStages) != 0) {
        return false;
    }
    for (size_t i = 0; i < group.Alternatives.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (group.Alternatives[i] == group.Alternatives[j]) {
                return false;
            }
        }
    }
    return true;
}

bool IsPassValid(const ShaderPassDesc& pass, bool requireVariants) {
    if (!IsRelativeResourcePathValid(pass.SourcePath, false) ||
        !IsEnumInRange(pass.SM, HlslShaderModel::SM66)) {
        return false;
    }
    for (const string& includeDir : pass.IncludeDirs) {
        if (!IsRelativeResourcePathValid(includeDir, true)) {
            return false;
        }
    }

    if (const auto* graphics = std::get_if<ShaderGraphicsPassDesc>(&pass.Program)) {
        if (graphics->VertexEntry.empty() ||
            (graphics->PixelEntry.has_value() && graphics->PixelEntry->empty()) ||
            graphics->ColorTargets.size() > kMaxColorTargets ||
            !IsEnumInRange(graphics->Cull, CullMode::None) ||
            (graphics->Depth.has_value() &&
             !IsEnumInRange(*graphics->Depth, CompareFunction::Always)) ||
            !std::isfinite(graphics->DepthBiasFactor) ||
            !std::isfinite(graphics->DepthBiasUnits)) {
            return false;
        }
        if (graphics->Stencil.has_value()) {
            const StencilState& stencil = graphics->Stencil->State;
            if (!IsStencilFaceValid(stencil.Front) || !IsStencilFaceValid(stencil.Back)) {
                return false;
            }
        }
        for (size_t i = 0; i < graphics->ColorTargets.size(); ++i) {
            const ShaderColorTargetDesc& target = graphics->ColorTargets[i];
            if (target.Index >= kMaxColorTargets ||
                (target.WriteMask.value() & ~static_cast<uint32_t>(ColorWrite::All)) != 0 ||
                (target.Blend.has_value() &&
                 (!IsBlendComponentValid(target.Blend->Color) ||
                  !IsBlendComponentValid(target.Blend->Alpha)))) {
                return false;
            }
            for (size_t j = 0; j < i; ++j) {
                if (target.Index == graphics->ColorTargets[j].Index) {
                    return false;
                }
            }
        }
    } else if (std::get<ShaderComputePassDesc>(pass.Program).EntryPoint.empty()) {
        return false;
    }

    const uint32_t programStages = GetProgramStageMask(pass);
    for (const ShaderKeywordGroupDesc& group : pass.KeywordGroups) {
        if (!IsKeywordGroupValid(group, programStages)) {
            return false;
        }
    }
    for (size_t i = 0; i < pass.Tags.size(); ++i) {
        if (pass.Tags[i].Name.empty()) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (pass.Tags[i].Name == pass.Tags[j].Name) {
                return false;
            }
        }
    }

    if (requireVariants) {
        const bool hasEmptyVariant = std::ranges::any_of(
            pass.Variants,
            [](const ShaderVariantDesc& variant) noexcept { return variant.Defines.empty(); });
        if (pass.Variants.empty() || !hasEmptyVariant) return false;
    }
    for (size_t i = 0; i < pass.Variants.size(); ++i) {
        vector<string> normalized = pass.Variants[i].Defines;
        NormalizeShaderDefines(normalized);
        if (normalized != pass.Variants[i].Defines || !AreShaderDefinesValid(pass, ShaderStage::UNKNOWN, normalized)) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (pass.Variants[j].Defines == normalized) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

void NormalizeShaderDefines(vector<string>& defines) {
    std::erase_if(defines, [](const string& value) noexcept { return value.empty(); });
    std::ranges::sort(defines);
    defines.erase(std::unique(defines.begin(), defines.end()), defines.end());
}

bool IsShaderAssetDataValid(const ShaderAssetData& asset, bool requireVariants) noexcept {
    if (asset.Passes.empty() || asset.Passes.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    try {
        return std::ranges::all_of(asset.Passes, [requireVariants](const ShaderPassDesc& pass) {
            return IsPassValid(pass, requireVariants);
        });
    } catch (...) {
        return false;
    }
}

bool AreShaderDefinesValid(
    const ShaderPassDesc& pass,
    ShaderStage stage,
    const vector<string>& defines) noexcept {
    for (const string& define : defines) {
        const bool declared = std::ranges::any_of(
            pass.KeywordGroups,
            [stage, &define](const ShaderKeywordGroupDesc& group) noexcept {
                const bool affectsStage = stage == ShaderStage::UNKNOWN ||
                                          group.Stages == ShaderStage::UNKNOWN ||
                                          group.Stages.HasFlag(stage);
                return affectsStage && std::ranges::find(group.Alternatives, define) != group.Alternatives.end();
            });
        if (!declared) {
            return false;
        }
    }
    for (const ShaderKeywordGroupDesc& group : pass.KeywordGroups) {
        if (stage != ShaderStage::UNKNOWN && group.Stages != ShaderStage::UNKNOWN && !group.Stages.HasFlag(stage)) {
            continue;
        }
        const size_t selected = std::ranges::count_if(
            group.Alternatives,
            [&defines](const string& alternative) noexcept {
                return !alternative.empty() && std::ranges::find(defines, alternative) != defines.end();
            });
        if (selected > 1) {
            return false;
        }
    }
    return true;
}

bool DoesShaderDefineAffectStage(
    const ShaderPassDesc& pass,
    std::string_view define,
    ShaderStage stage) noexcept {
    return std::ranges::any_of(
        pass.KeywordGroups,
        [define, stage](const ShaderKeywordGroupDesc& group) noexcept {
            const bool affectsStage = stage == ShaderStage::UNKNOWN ||
                                      group.Stages == ShaderStage::UNKNOWN ||
                                      group.Stages.HasFlag(stage);
            return affectsStage && std::ranges::find(group.Alternatives, define) != group.Alternatives.end();
        });
}

bool IsDeclaredShaderVariant(const ShaderPassDesc& pass, const vector<string>& defines) noexcept {
    try {
        vector<string> normalized = defines;
        NormalizeShaderDefines(normalized);
        return std::ranges::any_of(pass.Variants, [&normalized](const ShaderVariantDesc& variant) noexcept {
            return variant.Defines == normalized;
        });
    } catch (...) {
        return false;
    }
}

std::optional<std::string_view> FindShaderEntryPoint(
    const ShaderPassDesc& pass,
    ShaderStage stage) noexcept {
    if (const auto* graphics = std::get_if<ShaderGraphicsPassDesc>(&pass.Program)) {
        if (stage == ShaderStage::Vertex) {
            return graphics->VertexEntry;
        }
        if (stage == ShaderStage::Pixel && graphics->PixelEntry.has_value()) {
            return *graphics->PixelEntry;
        }
        return std::nullopt;
    }
    if (stage == ShaderStage::Compute) {
        return std::get<ShaderComputePassDesc>(pass.Program).EntryPoint;
    }
    return std::nullopt;
}

}  // namespace radray::shader
