#include <radray/shader/shader_asset_data.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

#include <fmt/format.h>

#include <radray/file.h>

namespace radray::shader {
namespace {

struct SourceFile {
    string IdentityPath;
    std::filesystem::path Path;
    vector<byte> Bytes;
};

bool IsPathUnderRoot(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) noexcept {
    const std::filesystem::path relative = candidate.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

string RemoveComments(std::string_view source) {
    enum class State : uint8_t {
        Normal,
        LineComment,
        BlockComment,
        String,
        Character,
    };
    State state = State::Normal;
    string result(source.size(), ' ');
    bool escaped = false;
    for (size_t i = 0; i < source.size(); ++i) {
        const char current = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (current == '\n' || current == '\r') {
            result[i] = current;
            if (state == State::LineComment) state = State::Normal;
            continue;
        }
        switch (state) {
            case State::Normal:
                if (current == '/' && next == '/') {
                    state = State::LineComment;
                    ++i;
                } else if (current == '/' && next == '*') {
                    state = State::BlockComment;
                    ++i;
                } else {
                    result[i] = current;
                    if (current == '"')
                        state = State::String;
                    else if (current == '\'')
                        state = State::Character;
                }
                break;
            case State::LineComment: break;
            case State::BlockComment:
                if (current == '*' && next == '/') {
                    state = State::Normal;
                    ++i;
                }
                break;
            case State::String:
            case State::Character:
                result[i] = current;
                if (escaped) {
                    escaped = false;
                } else if (current == '\\') {
                    escaped = true;
                } else if ((state == State::String && current == '"') ||
                           (state == State::Character && current == '\'')) {
                    state = State::Normal;
                }
                break;
        }
    }
    return result;
}

bool ParseIncludes(
    std::string_view source,
    vector<string>& includes,
    string& error) {
    const string uncommented = RemoveComments(source);
    const std::string_view uncommentedView{uncommented};
    size_t lineBegin = 0;
    while (lineBegin <= uncommentedView.size()) {
        const size_t lineEnd = uncommentedView.find('\n', lineBegin);
        const std::string_view line = uncommentedView.substr(
            lineBegin,
            lineEnd == std::string_view::npos
                ? uncommentedView.size() - lineBegin
                : lineEnd - lineBegin);
        size_t offset = line.find_first_not_of(" \t");
        if (offset != std::string_view::npos && line[offset] == '#') {
            ++offset;
            while (offset < line.size() && (line[offset] == ' ' || line[offset] == '\t')) {
                ++offset;
            }
            constexpr std::string_view keyword = "include";
            if (line.substr(offset, keyword.size()) == keyword) {
                offset += keyword.size();
                if (offset < line.size() && line[offset] != ' ' && line[offset] != '\t') {
                    if (lineEnd == std::string_view::npos) break;
                    lineBegin = lineEnd + 1;
                    continue;
                }
                while (offset < line.size() && (line[offset] == ' ' || line[offset] == '\t')) {
                    ++offset;
                }
                if (offset >= line.size() || (line[offset] != '"' && line[offset] != '<')) {
                    error = "macro-based #include cannot be included in the shader source identity";
                    return false;
                }
                const char close = line[offset] == '"' ? '"' : '>';
                const size_t end = line.find(close, offset + 1);
                if (end == std::string_view::npos || end == offset + 1) {
                    error = "malformed #include directive";
                    return false;
                }
                includes.emplace_back(line.substr(offset + 1, end - offset - 1));
            }
        }
        if (lineEnd == std::string_view::npos) break;
        lineBegin = lineEnd + 1;
    }
    return true;
}

void AppendU32(vector<byte>& output, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) {
        output.emplace_back(static_cast<byte>((value >> (i * 8)) & 0xffu));
    }
}

void AppendU64(vector<byte>& output, uint64_t value) {
    for (uint32_t i = 0; i < 8; ++i) {
        output.emplace_back(static_cast<byte>((value >> (i * 8)) & 0xffu));
    }
}

void AppendString(vector<byte>& output, std::string_view value) {
    AppendU32(output, static_cast<uint32_t>(value.size()));
    const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
    output.insert(output.end(), bytes.begin(), bytes.end());
}

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

bool IsPassValid(const ShaderPassDesc& pass, bool requireBakeSet) {
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
    unordered_map<string, size_t> keywordOwners;
    for (size_t groupIndex = 0; groupIndex < pass.VariantDomain.KeywordGroups.size(); ++groupIndex) {
        const ShaderKeywordGroupDesc& group = pass.VariantDomain.KeywordGroups[groupIndex];
        if (!IsKeywordGroupValid(group, programStages)) {
            return false;
        }
        for (const string& alternative : group.Alternatives) {
            if (alternative.empty()) {
                continue;
            }
            const auto [it, inserted] = keywordOwners.emplace(alternative, groupIndex);
            if (!inserted && it->second != groupIndex) {
                return false;
            }
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

    if (requireBakeSet && pass.BakeSet.Variants.empty()) return false;
    for (size_t i = 0; i < pass.BakeSet.Variants.size(); ++i) {
        vector<string> normalized = pass.BakeSet.Variants[i].Defines;
        NormalizeShaderDefines(normalized);
        if (normalized != pass.BakeSet.Variants[i].Defines ||
            !AreShaderDefinesValid(pass, ShaderStage::UNKNOWN, normalized)) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (pass.BakeSet.Variants[j].Defines == normalized) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

ShaderSourceIdentityResult ComputeShaderSourceIdentity(
    const std::filesystem::path& shaderRoot,
    const ShaderPassDesc& pass) noexcept {
    ShaderSourceIdentityResult result;
    try {
        std::error_code error;
        const std::filesystem::path root = std::filesystem::weakly_canonical(shaderRoot, error);
        if (error || !std::filesystem::is_directory(root)) {
            result.Error = fmt::format("shader root '{}' is unavailable", shaderRoot.string());
            return result;
        }
        vector<std::filesystem::path> includeRoots{root};
        for (const string& includeDir : pass.IncludeDirs) {
            const std::filesystem::path includeRoot = std::filesystem::weakly_canonical(
                root / std::filesystem::path{
                           std::u8string{includeDir.begin(), includeDir.end()}},
                error);
            if (error || !std::filesystem::is_directory(includeRoot) ||
                !IsPathUnderRoot(root, includeRoot)) {
                result.Error = fmt::format(
                    "shader include directory '{}' is unavailable",
                    includeDir);
                return result;
            }
            includeRoots.emplace_back(includeRoot);
        }

        vector<SourceFile> files;
        vector<std::filesystem::path> pending{
            root / std::filesystem::path{
                       std::u8string{pass.SourcePath.begin(), pass.SourcePath.end()}}};
        while (!pending.empty()) {
            const std::filesystem::path requested = std::move(pending.back());
            pending.pop_back();
            const std::filesystem::path path = std::filesystem::weakly_canonical(requested, error);
            if (error || !std::filesystem::is_regular_file(path) ||
                !IsPathUnderRoot(root, path)) {
                result.Error = fmt::format(
                    "shader source '{}' is missing or escapes the shader root",
                    requested.string());
                return result;
            }
            const string identityPath = path.lexically_relative(root).generic_string();
            if (std::ranges::any_of(files, [&](const SourceFile& value) {
                    return value.IdentityPath == identityPath;
                })) {
                continue;
            }
            auto bytes = ReadBinaryFile(path);
            if (!bytes.has_value()) {
                result.Error = fmt::format("failed to read shader source '{}'", path.string());
                return result;
            }
            string text{
                reinterpret_cast<const char*>(bytes->data()),
                bytes->size()};
            vector<string> includes;
            if (!ParseIncludes(text, includes, result.Error)) {
                result.Error = fmt::format("{} in '{}'", result.Error, identityPath);
                return result;
            }
            files.emplace_back(SourceFile{
                .IdentityPath = identityPath,
                .Path = path,
                .Bytes = std::move(*bytes)});
            for (const string& include : includes) {
                std::optional<std::filesystem::path> resolved;
                vector<std::filesystem::path> searchRoots{path.parent_path()};
                searchRoots.insert(
                    searchRoots.end(),
                    includeRoots.begin(),
                    includeRoots.end());
                for (const std::filesystem::path& searchRoot : searchRoots) {
                    const std::filesystem::path candidate =
                        searchRoot /
                        std::filesystem::path{std::u8string{include.begin(), include.end()}};
                    if (std::filesystem::is_regular_file(candidate)) {
                        resolved = candidate;
                        break;
                    }
                }
                if (!resolved.has_value()) {
                    result.Error = fmt::format(
                        "include '{}' referenced by '{}' is unavailable",
                        include,
                        identityPath);
                    return result;
                }
                pending.emplace_back(std::move(*resolved));
            }
        }

        std::ranges::sort(files, {}, &SourceFile::IdentityPath);
        vector<byte> identityBytes;
        AppendString(identityBytes, "radray-source-graph-v1");
        result.Identity.emplace();
        for (const SourceFile& file : files) {
            AppendString(identityBytes, file.IdentityPath);
            AppendU64(identityBytes, static_cast<uint64_t>(file.Bytes.size()));
            identityBytes.insert(identityBytes.end(), file.Bytes.begin(), file.Bytes.end());
            result.Identity->Dependencies.emplace_back(file.IdentityPath);
        }
        if (files.empty()) {
            result.Identity.reset();
            result.Error = "shader source graph is empty";
            return result;
        }
        result.Identity->Hash = HashShaderBytes(identityBytes);
        return result;
    } catch (const std::exception& error) {
        result.Identity.reset();
        result.Error = error.what();
        return result;
    } catch (...) {
        result.Identity.reset();
        result.Error = "failed to compute shader source identity";
        return result;
    }
}

void NormalizeShaderDefines(vector<string>& defines) {
    std::erase_if(defines, [](const string& value) noexcept { return value.empty(); });
    std::ranges::sort(defines);
    defines.erase(std::unique(defines.begin(), defines.end()), defines.end());
}

bool IsShaderAssetDataValid(const ShaderAssetData& asset, bool requireBakeSet) noexcept {
    if (asset.Passes.empty() || asset.Passes.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    try {
        if (!std::ranges::all_of(asset.Passes, [requireBakeSet](const ShaderPassDesc& pass) {
                return IsPassValid(pass, requireBakeSet);
            })) {
            return false;
        }
        unordered_map<string, ShaderKeywordScope> scopes;
        for (const ShaderPassDesc& pass : asset.Passes) {
            for (const ShaderKeywordGroupDesc& group : pass.VariantDomain.KeywordGroups) {
                for (const string& alternative : group.Alternatives) {
                    if (alternative.empty()) {
                        continue;
                    }
                    const auto [it, inserted] = scopes.emplace(alternative, group.Scope);
                    if (!inserted && it->second != group.Scope) {
                        return false;
                    }
                }
            }
        }
        return true;
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
            pass.VariantDomain.KeywordGroups,
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
    for (const ShaderKeywordGroupDesc& group : pass.VariantDomain.KeywordGroups) {
        if (stage != ShaderStage::UNKNOWN && group.Stages != ShaderStage::UNKNOWN && !group.Stages.HasFlag(stage)) {
            continue;
        }
        const size_t selected = std::ranges::count_if(
            group.Alternatives,
            [&defines](const string& alternative) noexcept {
                return !alternative.empty() && std::ranges::find(defines, alternative) != defines.end();
            });
        const bool hasDisabledAlternative =
            std::ranges::find(group.Alternatives, string{}) != group.Alternatives.end();
        if (selected > 1 || (selected == 0 && !hasDisabledAlternative)) {
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
        pass.VariantDomain.KeywordGroups,
        [define, stage](const ShaderKeywordGroupDesc& group) noexcept {
            const bool affectsStage = stage == ShaderStage::UNKNOWN ||
                                      group.Stages == ShaderStage::UNKNOWN ||
                                      group.Stages.HasFlag(stage);
            return affectsStage && std::ranges::find(group.Alternatives, define) != group.Alternatives.end();
        });
}

vector<string> ProjectShaderDefines(
    const ShaderPassDesc& pass,
    ShaderStage stage,
    const vector<string>& defines) {
    vector<string> result;
    result.reserve(defines.size());
    for (const string& define : defines) {
        if (DoesShaderDefineAffectStage(pass, define, stage)) {
            result.emplace_back(define);
        }
    }
    NormalizeShaderDefines(result);
    return result;
}

bool IsShaderVariantInDomain(const ShaderPassDesc& pass, const vector<string>& defines) noexcept {
    try {
        vector<string> normalized = defines;
        NormalizeShaderDefines(normalized);
        return AreShaderDefinesValid(pass, ShaderStage::UNKNOWN, normalized);
    } catch (...) {
        return false;
    }
}

bool IsBakedShaderVariant(const ShaderPassDesc& pass, const vector<string>& defines) noexcept {
    try {
        vector<string> normalized = defines;
        NormalizeShaderDefines(normalized);
        return std::ranges::any_of(
            pass.BakeSet.Variants,
            [&normalized](const ShaderVariantKey& variant) noexcept { return variant.Defines == normalized; });
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
