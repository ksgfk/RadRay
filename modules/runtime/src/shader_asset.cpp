#include <radray/runtime/shader_asset.h>

#include <limits>
#include <utility>

namespace radray {
namespace {

bool IsSourcePathValid(std::string_view path) noexcept {
    if (path.empty() || path.front() == '/' || path.front() == '\\' || path.find('\\') != std::string_view::npos) {
        return false;
    }

    size_t componentBegin = 0;
    while (componentBegin < path.size()) {
        const size_t separator = path.find('/', componentBegin);
        const size_t componentEnd = separator == std::string_view::npos ? path.size() : separator;
        const std::string_view component = path.substr(componentBegin, componentEnd - componentBegin);
        if (component.empty() || component == "." || component == ".." ||
            component.find(':') != std::string_view::npos || component.find('\0') != std::string_view::npos) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        componentBegin = separator + 1;
    }
    return false;
}

bool IsKeywordGroupValid(const ShaderKeywordGroupDesc& group, uint32_t programStages) noexcept {
    if (group.Alternatives.empty()) {
        return false;
    }

    switch (group.Scope) {
        case ShaderKeywordScope::Local:
        case ShaderKeywordScope::Global:
            break;
        default:
            return false;
    }

    const uint32_t keywordStages = group.Stages.value();
    if (keywordStages != 0 && (keywordStages & ~programStages) != 0) {
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

bool AreTagsValid(const vector<ShaderTagDesc>& tags) noexcept {
    for (size_t i = 0; i < tags.size(); ++i) {
        if (tags[i].Name.empty()) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (tags[i].Name == tags[j].Name) {
                return false;
            }
        }
    }
    return true;
}

bool IsGraphicsProgramValid(const ShaderGraphicsPassDesc& graphics, uint32_t& stages) noexcept {
    if (graphics.VertexEntry.empty() || (graphics.PixelEntry.has_value() && graphics.PixelEntry->empty())) {
        return false;
    }

    stages = static_cast<uint32_t>(render::ShaderStage::Vertex);
    if (graphics.PixelEntry.has_value()) {
        stages |= static_cast<uint32_t>(render::ShaderStage::Pixel);
    }

    for (size_t i = 0; i < graphics.ColorTargets.size(); ++i) {
        const ShaderColorTargetDesc& target = graphics.ColorTargets[i];
        if (target.Index >= render::kMaxColorTargets) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (target.Index == graphics.ColorTargets[j].Index) {
                return false;
            }
        }
    }
    return true;
}

bool IsPassValid(const ShaderPassDesc& pass) noexcept {
    if (!IsSourcePathValid(pass.SourcePath) || !AreTagsValid(pass.Tags)) {
        return false;
    }

    uint32_t programStages = 0;
    if (const auto* graphics = std::get_if<ShaderGraphicsPassDesc>(&pass.Program)) {
        if (!IsGraphicsProgramValid(*graphics, programStages)) {
            return false;
        }
    } else {
        const auto& compute = std::get<ShaderComputePassDesc>(pass.Program);
        if (compute.EntryPoint.empty()) {
            return false;
        }
        programStages = static_cast<uint32_t>(render::ShaderStage::Compute);
    }

    for (const ShaderKeywordGroupDesc& group : pass.KeywordGroups) {
        if (!IsKeywordGroupValid(group, programStages)) {
            return false;
        }
    }
    return true;
}

}  // namespace

ShaderAsset::ShaderAsset(vector<ShaderPassDesc> passes) noexcept
    : _passes(std::move(passes)) {
}

ShaderAsset::~ShaderAsset() noexcept = default;

void ShaderAsset::OnUnload(IRenderResourceRecycler& /*recycler*/) {
    _passes.clear();
}

AssetTypeId ShaderAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<ShaderAsset>;
}

bool ShaderAsset::IsValid() const noexcept {
    if (_passes.empty() || _passes.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    for (const ShaderPassDesc& pass : _passes) {
        if (!IsPassValid(pass)) {
            return false;
        }
    }
    return true;
}

std::optional<uint32_t> ShaderAsset::FindPassByName(std::string_view name) const noexcept {
    if (_passes.size() > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    for (size_t i = 0; i < _passes.size(); ++i) {
        if (_passes[i].Name == name) {
            return static_cast<uint32_t>(i);
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> ShaderAsset::FindPassByTag(std::string_view name, std::string_view value) const noexcept {
    if (_passes.size() > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    for (size_t i = 0; i < _passes.size(); ++i) {
        for (const ShaderTagDesc& tag : _passes[i].Tags) {
            if (tag.Name == name && tag.Value == value) {
                return static_cast<uint32_t>(i);
            }
        }
    }
    return std::nullopt;
}

}  // namespace radray
