#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <string_view>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <radray/json.h>
#include <radray/shader/dxc.h>
#include <radray/shader/shader_binary.h>
#include <radray/shader/spvc.h>

namespace {

using namespace radray;
using namespace radray::shader;
using namespace std::string_view_literals;

struct Arguments {
    std::filesystem::path Input;
    std::filesystem::path Output;
    std::filesystem::path ShaderRoot;
    string Target;
};

void PrintUsage() {
    std::fputs(
        "usage: radray_shader_cooker --input <shader.json> --output <shader.bin> "
        "--shader-root <directory> --target <dxil|spirv|all>\n",
        stderr);
}

void PrintError(std::string_view message) {
    fmt::print(stderr, "shader_cooker: {}\n", message);
}

std::optional<Arguments> ParseArguments(int argc, char** argv) {
    Arguments result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return std::nullopt;
        }
        if (i + 1 >= argc) {
            PrintError(fmt::format("missing value after {}", argument));
            return std::nullopt;
        }
        const std::filesystem::path value{argv[++i]};
        if (argument == "--input")
            result.Input = value;
        else if (argument == "--output")
            result.Output = value;
        else if (argument == "--shader-root")
            result.ShaderRoot = value;
        else if (argument == "--target")
            result.Target = value.string();
        else {
            PrintError(fmt::format("unknown argument {}", argument));
            return std::nullopt;
        }
    }
    if (result.Input.empty() || result.Output.empty() || result.ShaderRoot.empty() || result.Target.empty()) {
        PrintUsage();
        return std::nullopt;
    }
    if (result.Target != "dxil" && result.Target != "spirv" && result.Target != "all") {
        PrintError("--target must be dxil, spirv, or all");
        return std::nullopt;
    }
    return result;
}

bool ReadRequiredString(JsonValue object, const char* key, string& value, string& error) {
    const JsonValue field = object[key];
    if (!field.IsString() || field.AsString().empty()) {
        error = fmt::format("{} must be a non-empty string", key);
        return false;
    }
    value = field.AsString();
    return true;
}

bool ReadOptionalBool(JsonValue object, const char* key, bool& value, string& error) {
    if (!object.Has(key)) return true;
    if (!object[key].IsBool()) {
        error = fmt::format("{} must be a boolean", key);
        return false;
    }
    value = object[key].AsBool();
    return true;
}

bool ReadOptionalFloat(JsonValue object, const char* key, float& value, string& error) {
    if (!object.Has(key)) return true;
    if (!object[key].IsNumber()) {
        error = fmt::format("{} must be a number", key);
        return false;
    }
    const double number = object[key].AsDouble();
    if (!std::isfinite(number) || number < -std::numeric_limits<float>::max() ||
        number > std::numeric_limits<float>::max()) {
        error = fmt::format("{} must be a finite 32-bit number", key);
        return false;
    }
    value = static_cast<float>(number);
    return true;
}

bool ReadRequiredUint32(JsonValue object, const char* key, uint32_t& value, string& error) {
    const JsonValue field = object[key];
    if (!field.IsNumber()) {
        error = fmt::format("{} must be an unsigned 32-bit integer", key);
        return false;
    }
    const double number = field.AsDouble();
    if (!std::isfinite(number) || number < 0.0 ||
        number > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
        std::floor(number) != number) {
        error = fmt::format("{} must be an unsigned 32-bit integer", key);
        return false;
    }
    value = static_cast<uint32_t>(number);
    return true;
}

template <typename Enum, size_t N>
bool ParseNamedEnum(
    JsonValue value,
    const std::array<std::pair<std::string_view, Enum>, N>& names,
    Enum& result) {
    if (!value.IsString()) return false;
    const std::string_view text = value.AsString();
    const auto it = std::ranges::find_if(names, [text](const auto& item) { return item.first == text; });
    if (it == names.end()) return false;
    result = it->second;
    return true;
}

bool ParseShaderModel(JsonValue value, HlslShaderModel& result) {
    static constexpr std::array names{
        std::pair{"6_0"sv, HlslShaderModel::SM60}, std::pair{"6_1"sv, HlslShaderModel::SM61},
        std::pair{"6_2"sv, HlslShaderModel::SM62}, std::pair{"6_3"sv, HlslShaderModel::SM63},
        std::pair{"6_4"sv, HlslShaderModel::SM64}, std::pair{"6_5"sv, HlslShaderModel::SM65},
        std::pair{"6_6"sv, HlslShaderModel::SM66}};
    return ParseNamedEnum(value, names, result);
}

bool ParseCompare(JsonValue value, CompareFunction& result) {
    static constexpr std::array names{
        std::pair{"never"sv, CompareFunction::Never}, std::pair{"less"sv, CompareFunction::Less},
        std::pair{"equal"sv, CompareFunction::Equal}, std::pair{"less-equal"sv, CompareFunction::LessEqual},
        std::pair{"greater"sv, CompareFunction::Greater}, std::pair{"not-equal"sv, CompareFunction::NotEqual},
        std::pair{"greater-equal"sv, CompareFunction::GreaterEqual}, std::pair{"always"sv, CompareFunction::Always}};
    return ParseNamedEnum(value, names, result);
}

bool ParseStencilOperation(JsonValue value, StencilOperation& result) {
    static constexpr std::array names{
        std::pair{"keep"sv, StencilOperation::Keep}, std::pair{"zero"sv, StencilOperation::Zero},
        std::pair{"replace"sv, StencilOperation::Replace}, std::pair{"invert"sv, StencilOperation::Invert},
        std::pair{"increment-clamp"sv, StencilOperation::IncrementClamp},
        std::pair{"decrement-clamp"sv, StencilOperation::DecrementClamp},
        std::pair{"increment-wrap"sv, StencilOperation::IncrementWrap},
        std::pair{"decrement-wrap"sv, StencilOperation::DecrementWrap}};
    return ParseNamedEnum(value, names, result);
}

bool ParseBlendFactor(JsonValue value, BlendFactor& result) {
    static constexpr std::array names{
        std::pair{"zero"sv, BlendFactor::Zero}, std::pair{"one"sv, BlendFactor::One},
        std::pair{"src"sv, BlendFactor::Src}, std::pair{"one-minus-src"sv, BlendFactor::OneMinusSrc},
        std::pair{"src-alpha"sv, BlendFactor::SrcAlpha},
        std::pair{"one-minus-src-alpha"sv, BlendFactor::OneMinusSrcAlpha},
        std::pair{"dst"sv, BlendFactor::Dst}, std::pair{"one-minus-dst"sv, BlendFactor::OneMinusDst},
        std::pair{"dst-alpha"sv, BlendFactor::DstAlpha},
        std::pair{"one-minus-dst-alpha"sv, BlendFactor::OneMinusDstAlpha},
        std::pair{"src-alpha-saturated"sv, BlendFactor::SrcAlphaSaturated},
        std::pair{"constant"sv, BlendFactor::Constant},
        std::pair{"one-minus-constant"sv, BlendFactor::OneMinusConstant},
        std::pair{"src1"sv, BlendFactor::Src1}, std::pair{"one-minus-src1"sv, BlendFactor::OneMinusSrc1},
        std::pair{"src1-alpha"sv, BlendFactor::Src1Alpha},
        std::pair{"one-minus-src1-alpha"sv, BlendFactor::OneMinusSrc1Alpha}};
    return ParseNamedEnum(value, names, result);
}

bool ParseBlendOperation(JsonValue value, BlendOperation& result) {
    static constexpr std::array names{
        std::pair{"add"sv, BlendOperation::Add}, std::pair{"subtract"sv, BlendOperation::Subtract},
        std::pair{"reverse-subtract"sv, BlendOperation::ReverseSubtract},
        std::pair{"min"sv, BlendOperation::Min}, std::pair{"max"sv, BlendOperation::Max}};
    return ParseNamedEnum(value, names, result);
}

bool ParseBlendComponent(JsonValue value, BlendComponent& result, string& error) {
    if (!value.IsObject() || !ParseBlendFactor(value["Src"], result.Src) ||
        !ParseBlendFactor(value["Dst"], result.Dst) || !ParseBlendOperation(value["Op"], result.Op)) {
        error = "blend component requires valid Src, Dst, and Op strings";
        return false;
    }
    return true;
}

bool ParseBlend(JsonValue value, BlendState& result, string& error) {
    if (!value.IsObject() || !ParseBlendComponent(value["Color"], result.Color, error) ||
        !ParseBlendComponent(value["Alpha"], result.Alpha, error)) {
        if (error.empty()) error = "Blend must contain Color and Alpha objects";
        return false;
    }
    return true;
}

bool ParseStencilFace(JsonValue value, StencilFaceState& result, string& error) {
    if (!value.IsObject() || !ParseCompare(value["Compare"], result.Compare) ||
        !ParseStencilOperation(value["Fail"], result.FailOp) ||
        !ParseStencilOperation(value["DepthFail"], result.DepthFailOp) ||
        !ParseStencilOperation(value["Pass"], result.PassOp)) {
        error = "stencil face requires Compare, Fail, DepthFail, and Pass";
        return false;
    }
    return true;
}

bool ParseStencil(JsonValue value, ShaderStencilTestDesc& result, string& error) {
    if (!value.IsObject()) {
        error = "Stencil must be an object";
        return false;
    }
    if (!ReadRequiredUint32(value, "Reference", result.Reference, error) ||
        !ReadRequiredUint32(value, "ReadMask", result.State.ReadMask, error) ||
        !ReadRequiredUint32(value, "WriteMask", result.State.WriteMask, error)) {
        return false;
    }
    return ParseStencilFace(value["Front"], result.State.Front, error) &&
           ParseStencilFace(value["Back"], result.State.Back, error);
}

bool ParseStringArray(JsonValue value, vector<string>& result, string& error) {
    if (!value.IsArray()) {
        error = "expected an array of strings";
        return false;
    }
    result.clear();
    result.reserve(value.Size());
    for (size_t i = 0; i < value.Size(); ++i) {
        if (!value.At(i).IsString()) {
            error = "expected an array of strings";
            return false;
        }
        result.emplace_back(value.At(i).AsString());
    }
    return true;
}

bool ParseStages(JsonValue value, ShaderStages& stages, string& error) {
    if (!value.IsArray()) {
        error = "Stages must be an array";
        return false;
    }
    stages = ShaderStage::UNKNOWN;
    for (size_t i = 0; i < value.Size(); ++i) {
        if (!value.At(i).IsString()) {
            error = "Stages entries must be strings";
            return false;
        }
        const std::string_view stage = value.At(i).AsString();
        if (stage == "vertex")
            stages |= ShaderStage::Vertex;
        else if (stage == "pixel")
            stages |= ShaderStage::Pixel;
        else if (stage == "compute")
            stages |= ShaderStage::Compute;
        else {
            error = fmt::format("unsupported stage {}", stage);
            return false;
        }
    }
    return true;
}

bool ParseColorTarget(JsonValue value, ShaderColorTargetDesc& result, string& error) {
    if (!value.IsObject() || !ReadRequiredUint32(value, "Index", result.Index, error)) {
        if (error.empty()) error = "color target must be an object with numeric Index";
        return false;
    }
    if (value.Has("Blend")) {
        BlendState blend;
        if (!ParseBlend(value["Blend"], blend, error)) return false;
        result.Blend = blend;
    }
    if (value.Has("WriteMask")) {
        const JsonValue mask = value["WriteMask"];
        if (!mask.IsArray()) {
            error = "WriteMask must be an array";
            return false;
        }
        result.WriteMask = ColorWrites{};
        for (size_t i = 0; i < mask.Size(); ++i) {
            if (!mask.At(i).IsString()) {
                error = "WriteMask entries must be strings";
                return false;
            }
            const std::string_view channel = mask.At(i).AsString();
            if (channel == "red")
                result.WriteMask |= ColorWrite::Red;
            else if (channel == "green")
                result.WriteMask |= ColorWrite::Green;
            else if (channel == "blue")
                result.WriteMask |= ColorWrite::Blue;
            else if (channel == "alpha")
                result.WriteMask |= ColorWrite::Alpha;
            else {
                error = fmt::format("unsupported color write channel {}", channel);
                return false;
            }
        }
    }
    return true;
}

bool ParseProgram(JsonValue value, ShaderPassProgramDesc& result, string& error) {
    if (!value.IsObject() || !value["Type"].IsString()) {
        error = "Program must be an object with Type";
        return false;
    }
    const std::string_view type = value["Type"].AsString();
    if (type == "compute") {
        ShaderComputePassDesc compute;
        if (!ReadRequiredString(value, "Entry", compute.EntryPoint, error)) return false;
        result = std::move(compute);
        return true;
    }
    if (type != "graphics") {
        error = fmt::format("unsupported Program.Type {}", type);
        return false;
    }

    ShaderGraphicsPassDesc graphics;
    if (!ReadRequiredString(value, "Vertex", graphics.VertexEntry, error)) return false;
    if (value.Has("Pixel")) {
        string pixel;
        if (!ReadRequiredString(value, "Pixel", pixel, error)) return false;
        graphics.PixelEntry = std::move(pixel);
    }
    if (value.Has("ColorTargets")) {
        const JsonValue targets = value["ColorTargets"];
        if (!targets.IsArray()) {
            error = "ColorTargets must be an array";
            return false;
        }
        for (size_t i = 0; i < targets.Size(); ++i) {
            ShaderColorTargetDesc target;
            if (!ParseColorTarget(targets.At(i), target, error)) return false;
            graphics.ColorTargets.emplace_back(std::move(target));
        }
    }
    if (value.Has("Cull")) {
        static constexpr std::array names{
            std::pair{"front"sv, CullMode::Front}, std::pair{"back"sv, CullMode::Back},
            std::pair{"none"sv, CullMode::None}};
        if (!ParseNamedEnum(value["Cull"], names, graphics.Cull)) {
            error = "Cull must be front, back, or none";
            return false;
        }
    }
    if (value.Has("Depth")) {
        if (value["Depth"].IsString() && value["Depth"].AsString() == "none")
            graphics.Depth.reset();
        else {
            CompareFunction depth{};
            if (!ParseCompare(value["Depth"], depth)) {
                error = "Depth has an unsupported comparison value";
                return false;
            }
            graphics.Depth = depth;
        }
    }
    if (value.Has("Stencil")) {
        ShaderStencilTestDesc stencil;
        if (!ParseStencil(value["Stencil"], stencil, error)) return false;
        graphics.Stencil = stencil;
    }
    if (!ReadOptionalFloat(value, "DepthBiasFactor", graphics.DepthBiasFactor, error) ||
        !ReadOptionalFloat(value, "DepthBiasUnits", graphics.DepthBiasUnits, error) ||
        !ReadOptionalBool(value, "DepthWrite", graphics.DepthWrite, error) ||
        !ReadOptionalBool(value, "DepthClip", graphics.DepthClip, error) ||
        !ReadOptionalBool(value, "AlphaToMask", graphics.AlphaToMask, error) ||
        !ReadOptionalBool(value, "ConservativeRasterization", graphics.ConservativeRasterization, error)) {
        return false;
    }
    result = std::move(graphics);
    return true;
}

bool ParsePass(JsonValue value, ShaderPassDesc& result, string& error) {
    if (!value.IsObject() || !ReadRequiredString(value, "Source", result.SourcePath, error)) return false;
    if (value.Has("Name")) {
        if (!value["Name"].IsString()) {
            error = "Name must be a string";
            return false;
        }
        result.Name = value["Name"].AsString();
    }
    if (value.Has("IncludeDirs") && !ParseStringArray(value["IncludeDirs"], result.IncludeDirs, error)) return false;
    if (value.Has("ShaderModel") && !ParseShaderModel(value["ShaderModel"], result.SM)) {
        error = "ShaderModel must be one of 6_0 through 6_6";
        return false;
    }
    if (!ReadOptionalBool(value, "Optimize", result.IsOptimize, error) ||
        !ReadOptionalBool(value, "EnableUnbounded", result.EnableUnbounded, error)) {
        return false;
    }
    if (!ParseProgram(value["Program"], result.Program, error)) return false;

    if (value.Has("Tags")) {
        const JsonValue tags = value["Tags"];
        if (!tags.IsArray()) {
            error = "Tags must be an array";
            return false;
        }
        for (size_t i = 0; i < tags.Size(); ++i) {
            ShaderTagDesc tag;
            if (!tags.At(i).IsObject() || !ReadRequiredString(tags.At(i), "Name", tag.Name, error) ||
                !ReadRequiredString(tags.At(i), "Value", tag.Value, error)) {
                return false;
            }
            result.Tags.emplace_back(std::move(tag));
        }
    }
    if (value.Has("Keywords")) {
        const JsonValue keywords = value["Keywords"];
        if (!keywords.IsArray()) {
            error = "Keywords must be an array";
            return false;
        }
        for (size_t i = 0; i < keywords.Size(); ++i) {
            const JsonValue item = keywords.At(i);
            ShaderKeywordGroupDesc group;
            if (!item.IsObject() || !ParseStringArray(item["Alternatives"], group.Alternatives, error)) return false;
            if (item.Has("Scope")) {
                if (!item["Scope"].IsString()) {
                    error = "keyword Scope must be local or global";
                    return false;
                }
                const std::string_view scope = item["Scope"].AsString();
                if (scope == "local")
                    group.Scope = ShaderKeywordScope::Local;
                else if (scope == "global")
                    group.Scope = ShaderKeywordScope::Global;
                else {
                    error = "keyword Scope must be local or global";
                    return false;
                }
            }
            if (item.Has("Stages") && !ParseStages(item["Stages"], group.Stages, error)) return false;
            result.VariantDomain.KeywordGroups.emplace_back(std::move(group));
        }
    }

    if (value.Has("BakeSet") && value.Has("Variants")) {
        error = "use either BakeSet or legacy Variants, not both";
        return false;
    }
    if (value.Has("BakeSet") || value.Has("Variants")) {
        const JsonValue variants = value[value.Has("BakeSet") ? "BakeSet" : "Variants"];
        if (!variants.IsArray()) {
            error = "BakeSet must be an array of explicit define arrays";
            return false;
        }
        for (size_t i = 0; i < variants.Size(); ++i) {
            ShaderVariantKey variant;
            if (!ParseStringArray(variants.At(i), variant.Defines, error)) return false;
            NormalizeShaderDefines(variant.Defines);
            result.BakeSet.Variants.emplace_back(std::move(variant));
        }
    }
    return true;
}

std::optional<ShaderAssetData> ParseAsset(const std::filesystem::path& input, string& error) {
    auto document = JsonDocument::ParseFile(input);
    if (!document.has_value() || !document->Root().IsObject()) {
        error = "input is not a valid JSON object";
        return std::nullopt;
    }
    const JsonValue root = document->Root();
    ShaderAssetData result;
    if (!root["AssetId"].IsString() || !Guid::TryParse(root["AssetId"].AsString(), result.AssetId) ||
        result.AssetId.IsEmpty()) {
        error = "AssetId must be a non-empty GUID string";
        return std::nullopt;
    }
    const JsonValue passes = root["Passes"];
    if (!passes.IsArray() || passes.Size() == 0) {
        error = "Passes must be a non-empty array";
        return std::nullopt;
    }
    for (size_t i = 0; i < passes.Size(); ++i) {
        ShaderPassDesc pass;
        if (!ParsePass(passes.At(i), pass, error)) {
            error = fmt::format("Passes[{}]: {}", i, error);
            return std::nullopt;
        }
        result.Passes.emplace_back(std::move(pass));
    }
    if (!IsShaderAssetDataValid(result, false)) {
        error = "ShaderAsset data failed semantic validation";
        return std::nullopt;
    }
    return result;
}

bool IsPathUnderRoot(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const std::filesystem::path relative = candidate.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

bool ValidateSourcePaths(const ShaderAssetData& asset, const std::filesystem::path& shaderRoot, string& error) {
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical(shaderRoot, ec);
    if (ec || !std::filesystem::is_directory(root)) {
        error = fmt::format("shader root '{}' is not a directory", shaderRoot.string());
        return false;
    }
    for (const ShaderPassDesc& pass : asset.Passes) {
        const std::filesystem::path source =
            std::filesystem::weakly_canonical(
                root / std::filesystem::path{std::u8string{pass.SourcePath.begin(), pass.SourcePath.end()}},
                ec);
        if (ec || !IsPathUnderRoot(root, source) || !std::filesystem::is_regular_file(source)) {
            error = fmt::format("source '{}' is missing or escapes shader root", pass.SourcePath);
            return false;
        }
        for (const string& includeDir : pass.IncludeDirs) {
            const std::filesystem::path include =
                std::filesystem::weakly_canonical(
                    root / std::filesystem::path{std::u8string{includeDir.begin(), includeDir.end()}},
                    ec);
            if (ec || !IsPathUnderRoot(root, include) || !std::filesystem::is_directory(include)) {
                error = fmt::format("include directory '{}' is missing or escapes shader root", includeDir);
                return false;
            }
        }
    }
    return true;
}

bool CaptureSourceIdentities(
    ShaderAssetData& asset,
    const std::filesystem::path& shaderRoot,
    string& error) {
    for (size_t passIndex = 0; passIndex < asset.Passes.size(); ++passIndex) {
        ShaderSourceIdentityResult identity = ComputeShaderSourceIdentity(
            shaderRoot,
            asset.Passes[passIndex]);
        if (!identity.Succeeded()) {
            error = fmt::format(
                "pass {} source identity failed: {}",
                passIndex,
                identity.Error);
            return false;
        }
        asset.Passes[passIndex].SourceIdentity = identity.Identity->Hash;
    }
    return true;
}

vector<ShaderStage> GetPassStages(const ShaderPassDesc& pass) {
    if (const auto* graphics = std::get_if<ShaderGraphicsPassDesc>(&pass.Program)) {
        vector<ShaderStage> stages{ShaderStage::Vertex};
        if (graphics->PixelEntry.has_value()) stages.emplace_back(ShaderStage::Pixel);
        return stages;
    }
    return {ShaderStage::Compute};
}

template <typename T>
uint32_t InternValue(vector<T>& table, T value) {
    const auto it = std::ranges::find(table, value);
    if (it != table.end()) return static_cast<uint32_t>(std::distance(table.begin(), it));
    table.emplace_back(std::move(value));
    return static_cast<uint32_t>(table.size() - 1);
}

std::optional<string> SerializeReflection(const ShaderReflectionDesc& reflection) {
    if (const auto* hlsl = std::get_if<HlslShaderDesc>(&reflection)) {
        return SerializeHlslShaderDesc(*hlsl);
    }
    return SerializeSpirvShaderDesc(std::get<SpirvShaderDesc>(reflection));
}

std::optional<uint32_t> InternReflection(
    ShaderBinary& binary,
    ShaderTarget target,
    ShaderReflectionDesc reflection) {
    const auto payload = SerializeReflection(reflection);
    if (!payload.has_value()) return std::nullopt;
    const ShaderHash hash = HashShaderBytes(std::as_bytes(std::span{payload->data(), payload->size()}));
    for (uint32_t i = 0; i < binary.Reflections.size(); ++i) {
        const ShaderReflectionRecord& existing = binary.Reflections[i];
        if (existing.Target != target || existing.Hash != hash) continue;
        const auto existingPayload = SerializeReflection(existing.Reflection);
        if (existingPayload.has_value() && *existingPayload == *payload) return i;
    }
    binary.Reflections.emplace_back(ShaderReflectionRecord{
        .Target = target,
        .Reflection = std::move(reflection),
        .Hash = hash});
    return static_cast<uint32_t>(binary.Reflections.size() - 1);
}

Nullable<const ShaderStageArtifact*> FindProjectedArtifact(
    const ShaderBinary& binary,
    ShaderTarget target,
    uint32_t passIndex,
    ShaderStage stage,
    const vector<string>& projectedDefines) {
    const auto it = std::ranges::find_if(binary.StageArtifacts, [&](const ShaderStageArtifact& artifact) {
        return artifact.Target == target && artifact.PassIndex == passIndex && artifact.Stage == stage &&
               artifact.Defines == projectedDefines;
    });
    return it == binary.StageArtifacts.end() ? nullptr : &*it;
}

std::optional<ShaderReflectionDesc> ReflectCompiledShader(
    Dxc& dxc,
    ShaderTarget target,
    ShaderStage stage,
    std::string_view entry,
    const DxcOutput& compiled,
    string& error) {
    if (target == ShaderTarget::DXIL) {
        auto reflection = dxc.GetShaderDescFromOutput(compiled.Refl);
        if (reflection.has_value()) return ShaderReflectionDesc{std::move(*reflection)};
        error = fmt::format("DXIL reflection failed for entry {}", entry);
        return std::nullopt;
    }
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
    auto reflection = ReflectSpirv(SpirvBytecodeView{
        .Data = compiled.Data,
        .EntryPointName = entry,
        .Stage = stage});
    if (reflection.has_value()) return ShaderReflectionDesc{std::move(*reflection)};
    error = fmt::format("SPIR-V reflection failed for entry {}", entry);
#else
    (void)stage;
    (void)compiled;
    error = "SPIR-V target requires RADRAY_ENABLE_SPIRV_CROSS";
#endif
    return std::nullopt;
}

ShaderStageInterfaceBuildResult NormalizeReflection(
    const ShaderReflectionDesc& reflection,
    ShaderTarget target,
    uint32_t passIndex,
    const vector<string>& fullDefines,
    ShaderStage stage) {
    ShaderInterfaceNormalizationOptions options;
    options.Context.Target = target;
    options.Context.PassIndex = passIndex;
    options.Context.VariantDefines = fullDefines;
    options.Context.Stage = stage;
    if (const auto* hlsl = std::get_if<HlslShaderDesc>(&reflection)) {
        return NormalizeHlslInterface(*hlsl, stage, options);
    }
    return NormalizeSpirvInterface(std::get<SpirvShaderDesc>(reflection), stage, options);
}

string FormatDiagnostics(const vector<ShaderDiagnostic>& diagnostics) {
    if (diagnostics.empty()) return "shader interface construction failed without a diagnostic";
    string result;
    for (size_t i = 0; i < diagnostics.size(); ++i) {
        if (i != 0) result += "; ";
        result += diagnostics[i].Message;
    }
    return result;
}

void AppendInterfaceFields(
    string& result,
    const vector<ShaderInterfaceFieldDesc>& fields,
    std::string_view prefix) {
    for (const ShaderInterfaceFieldDesc& field : fields) {
        string path{prefix};
        if (!path.empty()) path.push_back('.');
        path += field.Name;
        result += fmt::format(
            " field({} off={} size={} scalar={} {}x{} array={} astride={} mstride={} major={})",
            path,
            field.Offset,
            field.Size,
            static_cast<uint32_t>(field.Type.Scalar),
            field.Type.Rows,
            field.Type.Columns,
            field.Type.ArrayCount,
            field.Type.ArrayStride,
            field.Type.MatrixStride,
            field.Type.RowMajor);
        AppendInterfaceFields(result, field.Members, path);
    }
}

string DescribeStageInterface(const ShaderStageInterfaceDesc& interface) {
    string result = fmt::format("stage={}", interface.Stage);
    for (const ShaderBindingGroupInterfaceDesc& group : interface.BindingGroups) {
        result += fmt::format(" group={}", group.GroupIndex);
        for (const ShaderBindingDesc& binding : group.Bindings) {
            result += fmt::format(
                " [{}:{} kind={} count={} stages={}]",
                group.GroupIndex,
                binding.BindingIndex,
                static_cast<uint32_t>(binding.Kind),
                binding.Count,
                binding.Stages.value());
            if (binding.Buffer.has_value()) {
                result += fmt::format(
                    " buffer(bytes={} stride={})",
                    binding.Buffer->ByteSize,
                    binding.Buffer->ElementStride);
                AppendInterfaceFields(result, binding.Buffer->Fields, {});
            }
        }
    }
    for (const ShaderStageIoDesc& input : interface.Inputs) {
        result += fmt::format(
            " in({}{} loc={} builtin={} scalar={} {}x{})",
            input.SemanticName,
            input.SemanticIndex,
            input.Location,
            static_cast<uint32_t>(input.Builtin),
            static_cast<uint32_t>(input.Type.Scalar),
            input.Type.Rows,
            input.Type.Columns);
    }
    for (const ShaderStageIoDesc& output : interface.Outputs) {
        result += fmt::format(
            " out({}{} loc={} builtin={} scalar={} {}x{})",
            output.SemanticName,
            output.SemanticIndex,
            output.Location,
            static_cast<uint32_t>(output.Builtin),
            static_cast<uint32_t>(output.Type.Scalar),
            output.Type.Rows,
            output.Type.Columns);
    }
    return result;
}

bool CompileTarget(
    Dxc& dxc,
    const std::filesystem::path& shaderRoot,
    ShaderBinary& binary,
    ShaderTarget target,
    string& error) {
    const ShaderAssetData& asset = binary.Asset;
    for (uint32_t passIndex = 0; passIndex < asset.Passes.size(); ++passIndex) {
        const ShaderPassDesc& pass = asset.Passes[passIndex];
        vector<string> includeStrings{shaderRoot.string()};
        for (const string& includeDir : pass.IncludeDirs) {
            includeStrings.emplace_back(
                (shaderRoot / std::filesystem::path{std::u8string{includeDir.begin(), includeDir.end()}}).string());
        }
        vector<std::string_view> includes;
        includes.reserve(includeStrings.size());
        for (const string& include : includeStrings) includes.emplace_back(include);

        for (const ShaderVariantKey& variant : pass.BakeSet.Variants) {
            vector<uint32_t> stageArtifactIndices;
            for (ShaderStage stage : GetPassStages(pass)) {
                const auto entry = FindShaderEntryPoint(pass, stage);
                if (!entry.has_value()) {
                    error = fmt::format("pass {} has no entry point for {}", passIndex, stage);
                    return false;
                }
                const vector<string> projectedDefines = ProjectShaderDefines(pass, stage, variant.Defines);
                if (auto existing = FindProjectedArtifact(
                        binary, target, passIndex, stage, projectedDefines);
                    existing.HasValue()) {
                    const uint32_t index = static_cast<uint32_t>(existing.Get() - binary.StageArtifacts.data());
                    stageArtifactIndices.emplace_back(index);
                    continue;
                }
                vector<std::string_view> defines;
                defines.reserve(projectedDefines.size());
                for (const string& define : projectedDefines) defines.emplace_back(define);
                DxcCompileOptions options{
                    .EntryPoint = *entry,
                    .Stage = stage,
                    .SM = pass.SM,
                    .Defines = defines,
                    .Includes = includes,
                    .IsOptimize = pass.IsOptimize,
                    .IsSpirv = target == ShaderTarget::SPIRV,
                    .EnableUnbounded = pass.EnableUnbounded};
                auto compiled = dxc.CompileFile(
                    shaderRoot /
                        std::filesystem::path{std::u8string{pass.SourcePath.begin(), pass.SourcePath.end()}},
                    options);
                if (!compiled.has_value()) {
                    error = fmt::format("DXC failed for pass {} entry {} target {}", passIndex, *entry, target);
                    return false;
                }

                auto reflection = ReflectCompiledShader(dxc, target, stage, *entry, *compiled, error);
                if (!reflection.has_value()) {
                    error = fmt::format("pass {}: {}", passIndex, error);
                    return false;
                }
                auto normalized = NormalizeReflection(*reflection, target, passIndex, variant.Defines, stage);
                if (!normalized.Succeeded()) {
                    error = fmt::format(
                        "interface normalization failed for pass {} entry {} target {}: {}",
                        passIndex,
                        *entry,
                        target,
                        FormatDiagnostics(normalized.Diagnostics));
                    return false;
                }

                for (const ShaderStageArtifact& other : binary.StageArtifacts) {
                    if (other.Target == target || other.PassIndex != passIndex || other.Stage != stage ||
                        other.Defines != projectedDefines) {
                        continue;
                    }
                    if (binary.StageInterfaces[other.InterfaceIndex] != *normalized.Interface) {
                        error = fmt::format(
                            "canonical stage interface mismatch for pass {} entry {} variant [{}] between {} and {}",
                            passIndex,
                            *entry,
                            fmt::join(projectedDefines, ", "),
                            other.Target,
                            target);
                        error += fmt::format(
                            "; first: {}; second: {}",
                            DescribeStageInterface(binary.StageInterfaces[other.InterfaceIndex]),
                            DescribeStageInterface(*normalized.Interface));
                        return false;
                    }
                }

                const auto reflectionIndex = InternReflection(binary, target, std::move(*reflection));
                if (!reflectionIndex.has_value()) {
                    error = fmt::format("reflection serialization failed for pass {} entry {}", passIndex, *entry);
                    return false;
                }
                const uint32_t interfaceIndex = InternValue(binary.StageInterfaces, std::move(*normalized.Interface));

                ShaderStageArtifact artifact{
                    .Target = target,
                    .Category = GetShaderBlobCategory(target),
                    .PassIndex = passIndex,
                    .Stage = stage,
                    .Defines = projectedDefines,
                    .EntryPoint = string{*entry},
                    .Bytecode = std::move(compiled->Data),
                    .ReflectionIndex = *reflectionIndex,
                    .InterfaceIndex = interfaceIndex};
                artifact.BinaryHash = HashShaderBytes(artifact.Bytecode);
                binary.StageArtifacts.emplace_back(std::move(artifact));
                stageArtifactIndices.emplace_back(static_cast<uint32_t>(binary.StageArtifacts.size() - 1));
            }

            vector<const ShaderStageInterfaceDesc*> stageInterfaces;
            stageInterfaces.reserve(stageArtifactIndices.size());
            for (uint32_t artifactIndex : stageArtifactIndices) {
                stageInterfaces.emplace_back(
                    &binary.StageInterfaces[binary.StageArtifacts[artifactIndex].InterfaceIndex]);
            }
            ShaderInterfaceBuildResult programInterface;
            ShaderDiagnosticContext programContext{
                .Target = target,
                .PassIndex = passIndex,
                .VariantDefines = variant.Defines};
            if (std::holds_alternative<ShaderGraphicsPassDesc>(pass.Program)) {
                programInterface = stageInterfaces.size() == 1
                                       ? MergeGraphicsStageInterfaces(*stageInterfaces[0], programContext)
                                       : MergeGraphicsStageInterfaces(
                                             *stageInterfaces[0],
                                             *stageInterfaces[1],
                                             programContext);
            } else {
                programInterface = BuildComputeShaderInterface(*stageInterfaces[0], programContext);
            }
            if (!programInterface.Succeeded()) {
                error = fmt::format(
                    "program interface merge failed for pass {} variant [{}]: {}",
                    passIndex,
                    fmt::join(variant.Defines, ", "),
                    FormatDiagnostics(programInterface.Diagnostics));
                return false;
            }
            for (const ShaderProgramVariantArtifact& other : binary.ProgramVariants) {
                if (other.Target == target || other.PassIndex != passIndex || other.Defines != variant.Defines) continue;
                if (binary.ProgramInterfaces[other.InterfaceIndex] != *programInterface.Interface) {
                    error = fmt::format(
                        "canonical program interface mismatch for pass {} variant [{}] between {} and {}",
                        passIndex,
                        fmt::join(variant.Defines, ", "),
                        other.Target,
                        target);
                    return false;
                }
            }
            const uint32_t programInterfaceIndex =
                InternValue(binary.ProgramInterfaces, std::move(*programInterface.Interface));
            binary.ProgramVariants.emplace_back(ShaderProgramVariantArtifact{
                .Target = target,
                .PassIndex = passIndex,
                .Defines = variant.Defines,
                .StageArtifactIndices = std::move(stageArtifactIndices),
                .InterfaceIndex = programInterfaceIndex});
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#if !defined(RADRAY_ENABLE_DXC)
    (void)argc;
    (void)argv;
    PrintError("this executable was built without RADRAY_ENABLE_DXC");
    return 3;
#else
    auto arguments = ParseArguments(argc, argv);
    if (!arguments.has_value()) return 2;

    string error;
    auto asset = ParseAsset(arguments->Input, error);
    if (!asset.has_value()) {
        PrintError(error);
        return 3;
    }
    if (!ValidateSourcePaths(*asset, arguments->ShaderRoot, error) ||
        !CaptureSourceIdentities(*asset, arguments->ShaderRoot, error)) {
        PrintError(error);
        return 3;
    }
    auto dxc = CreateDxc();
    if (!dxc.HasValue()) {
        PrintError("failed to initialize DXC");
        return 4;
    }

    ShaderBinary binary;
    binary.Asset = std::move(*asset);
    if ((arguments->Target == "dxil" || arguments->Target == "all") &&
        !CompileTarget(*dxc.Get(), arguments->ShaderRoot, binary, ShaderTarget::DXIL, error)) {
        PrintError(error);
        return 4;
    }
    if ((arguments->Target == "spirv" || arguments->Target == "all") &&
        !CompileTarget(*dxc.Get(), arguments->ShaderRoot, binary, ShaderTarget::SPIRV, error)) {
        PrintError(error);
        return 4;
    }
    const bool complete =
        (arguments->Target != "dxil" || binary.IsBakeComplete(ShaderTarget::DXIL)) &&
        (arguments->Target != "spirv" || binary.IsBakeComplete(ShaderTarget::SPIRV)) &&
        (arguments->Target != "all" ||
         (binary.IsBakeComplete(ShaderTarget::DXIL) && binary.IsBakeComplete(ShaderTarget::SPIRV)));
    if (!binary.IsValid() || !complete || !WriteShaderBinary(arguments->Output, binary)) {
        PrintError(fmt::format("failed to write '{}'", arguments->Output.string()));
        return 5;
    }
    fmt::print(
        "cooked {} unique stage artifacts and {} program variants to {}\n",
        binary.StageArtifacts.size(),
        binary.ProgramVariants.size(),
        arguments->Output.string());
    return 0;
#endif
}
