// radray_shader_gen —— 从 HLSL 反射生成 *.shader.json 的起始模板。作者期一次性工具,
// 输出需人工收敛 (烘焙已收敛 manifest 的是 radray_shader_cook)。
//
// 生成的文件可以直接被 ParseShaderAssetDesc 解析并 cook; "_TODO" 数组列出反射原理上
// 给不出、需要作者确认的字段。用法与逐项填法见 docs/guide/shader-authoring.md。

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string_view>

#include <fmt/format.h>

#include <radray/enum_flags.h>
#include <radray/file.h>
#include <radray/shader/dxc.h>
#include <radray/shader/shader_asset_template.h>
#include <radray/types.h>

namespace {

using namespace radray;

constexpr std::string_view kUsage =
    "usage: radray_shader_gen --shader-root <dir> --source <rel.hlsl>\n"
    "                         --stage <vertex|pixel|compute>=<EntryPoint> [--stage ...]\n"
    "                         [--name <AssetName>] [--pass <PassName>]\n"
    "                         [--sm <SM60|SM61|SM62|SM63|SM64|SM65|SM66>]\n"
    "                         [--define <MACRO[=VALUE]>]... [--probe <MACRO,MACRO,...>]...\n"
    "                         [--no-spirv] [--no-vertex-input] [--no-optimize]\n"
    "                         [--no-unbounded] [--no-keyword-pragma] [--no-auto-probe]\n"
    "                         [--keyword-entry-only]\n"
    "                         [-o <out.shader.json>] [--force] [--stdout]\n"
    "\n"
    "KeywordGroups come from '#pragma radray_keyword_group' in the source and everything it\n"
    "includes, and are also used to probe bindings hidden behind #ifdef, so --probe is\n"
    "rarely needed. Pass --keyword-entry-only to ignore declarations from included headers.\n";

struct Arguments {
    std::filesystem::path ShaderRoot;
    string Source;
    string AssetName;
    string PassName;
    render::HlslShaderModel ShaderModel{render::HlslShaderModel::SM60};
    vector<ShaderStageDesc> Stages;
    vector<string> Defines;
    vector<vector<string>> Probes;
    std::filesystem::path Output;
    bool ToStdout{false};
    bool UseSpirv{true};
    bool GenerateVertexInput{true};
    bool IsOptimize{true};
    bool EnableUnbounded{true};
    bool ParseKeywordPragmas{true};
    /// 只认入口文件里的 keyword 声明, 不继承 include 链带来的组。
    bool KeywordEntryFileOnly{false};
    bool AutoProbe{true};
    /// 允许覆盖已存在的输出文件。默认拒绝: 目标通常已被人工补齐过 Residency /
    /// BakeVariants, 而生成结果只是模板, 覆盖会静默丢掉那些决策。
    bool Force{false};
};

void PrintError(std::string_view message) {
    std::fputs(fmt::format("radray_shader_gen: {}\n", message).c_str(), stderr);
}

/// "vertex=VSMain" -> (Vertex, "VSMain")。stage 名大小写不敏感。
std::optional<ShaderStageDesc> ParseStageArgument(std::string_view text) {
    const size_t separator = text.find('=');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 == text.size()) {
        return std::nullopt;
    }
    const std::string_view stageName = text.substr(0, separator);
    string normalized{stageName};
    if (!normalized.empty()) {
        normalized[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(normalized[0])));
        for (size_t i = 1; i < normalized.size(); ++i) {
            normalized[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(normalized[i])));
        }
    }
    const std::optional<render::ShaderStage> stage =
        EnumCast<render::ShaderStage>(normalized);
    if (!stage.has_value() || stage.value() == render::ShaderStage::UNKNOWN) {
        return std::nullopt;
    }
    ShaderStageDesc desc{};
    desc.Stage = stage.value();
    desc.EntryPoint = string{text.substr(separator + 1)};
    return desc;
}

/// "A,B,C" -> ["A", "B", "C"]。空段被丢弃。
vector<string> SplitList(std::string_view text) {
    vector<string> result;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = text.find(',', begin);
        const std::string_view piece =
            text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        if (!piece.empty()) {
            result.emplace_back(piece);
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

/// 取一个需要值的选项。缺值时报错并返回 false。
bool TakeValue(int argc, char** argv, int& index, std::string_view option, std::string_view& out) {
    if (index + 1 >= argc) {
        PrintError(fmt::format("option '{}' requires a value", option));
        return false;
    }
    out = argv[++index];
    return true;
}

std::optional<Arguments> ParseArguments(int argc, char** argv) {
    Arguments args{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view option{argv[i]};
        std::string_view value{};
        if (option == "--shader-root") {
            if (!TakeValue(argc, argv, i, option, value)) return std::nullopt;
            args.ShaderRoot = std::filesystem::path{value};
        } else if (option == "--source") {
            if (!TakeValue(argc, argv, i, option, value)) return std::nullopt;
            args.Source = string{value};
        } else if (option == "--name") {
            if (!TakeValue(argc, argv, i, option, value)) return std::nullopt;
            args.AssetName = string{value};
        } else if (option == "--pass") {
            if (!TakeValue(argc, argv, i, option, value)) return std::nullopt;
            args.PassName = string{value};
        } else if (option == "--sm") {
            if (!TakeValue(argc, argv, i, option, value)) return std::nullopt;
            const std::optional<render::HlslShaderModel> sm =
                EnumCast<render::HlslShaderModel>(value);
            if (!sm.has_value()) {
                PrintError(fmt::format("unknown shader model '{}'", value));
                return std::nullopt;
            }
            args.ShaderModel = sm.value();
        } else if (option == "--stage") {
            if (!TakeValue(argc, argv, i, option, value)) return std::nullopt;
            std::optional<ShaderStageDesc> stage = ParseStageArgument(value);
            if (!stage.has_value()) {
                PrintError(fmt::format(
                    "malformed --stage '{}'; expected <vertex|pixel|compute>=<EntryPoint>", value));
                return std::nullopt;
            }
            args.Stages.push_back(std::move(stage.value()));
        } else if (option == "--define") {
            if (!TakeValue(argc, argv, i, option, value)) return std::nullopt;
            args.Defines.emplace_back(value);
        } else if (option == "--probe") {
            if (!TakeValue(argc, argv, i, option, value)) return std::nullopt;
            vector<string> probe = SplitList(value);
            if (probe.empty()) {
                PrintError("--probe requires at least one macro");
                return std::nullopt;
            }
            args.Probes.push_back(std::move(probe));
        } else if (option == "-o" || option == "--output") {
            if (!TakeValue(argc, argv, i, option, value)) return std::nullopt;
            args.Output = std::filesystem::path{value};
        } else if (option == "--stdout") {
            args.ToStdout = true;
        } else if (option == "--no-spirv") {
            args.UseSpirv = false;
        } else if (option == "--no-vertex-input") {
            args.GenerateVertexInput = false;
        } else if (option == "--no-optimize") {
            args.IsOptimize = false;
        } else if (option == "--no-unbounded") {
            args.EnableUnbounded = false;
        } else if (option == "--no-keyword-pragma") {
            args.ParseKeywordPragmas = false;
        } else if (option == "--keyword-entry-only") {
            args.KeywordEntryFileOnly = true;
        } else if (option == "--no-auto-probe") {
            args.AutoProbe = false;
        } else if (option == "--force") {
            args.Force = true;
        } else if (option == "-h" || option == "--help") {
            std::fputs(string{kUsage}.c_str(), stdout);
            return std::nullopt;
        } else {
            PrintError(fmt::format("unknown option '{}'", option));
            std::fputs(string{kUsage}.c_str(), stderr);
            return std::nullopt;
        }
    }

    if (args.ShaderRoot.empty()) {
        PrintError("--shader-root is required");
        return std::nullopt;
    }
    if (args.Source.empty()) {
        PrintError("--source is required");
        return std::nullopt;
    }
    if (args.Stages.empty()) {
        PrintError("at least one --stage is required");
        return std::nullopt;
    }
    // 默认输出到源文件同目录的 <stem>.shader.json, 与 cook 的目录约定一致。
    if (args.Output.empty() && !args.ToStdout) {
        const std::filesystem::path source = args.ShaderRoot / std::filesystem::path{args.Source};
        args.Output = source.parent_path() /
                      (source.stem().generic_string() + ".shader.json");
    }
    return args;
}

}  // namespace

int main(int argc, char** argv) {
#if !defined(RADRAY_ENABLE_SHADER_JIT)
    PrintError("this build has no DXC; template generation is unavailable");
    return 1;
#else
    std::optional<Arguments> args = ParseArguments(argc, argv);
    if (!args.has_value()) {
        return 1;
    }

    // 先查覆盖再干活: 生成要编译好几轮, 没必要等到最后才发现写不出去。
    if (!args->ToStdout && !args->Force) {
        std::error_code exists;
        if (std::filesystem::exists(args->Output, exists) && !exists) {
            PrintError(fmt::format(
                "'{}' already exists; generated output is only a template and would discard "
                "hand-authored fields (Residency, BakeVariants, ImmutableSampler, ...). "
                "Write elsewhere and diff, or pass --force to overwrite.",
                args->Output.generic_string()));
            return 1;
        }
    }

    Nullable<shared_ptr<render::Dxc>> dxcResult = render::CreateDxc();
    if (!dxcResult.HasValue()) {
        PrintError("failed to create DXC");
        return 1;
    }
    const shared_ptr<render::Dxc> dxc = dxcResult.Release();

    ShaderTemplatePassSeed passSeed{};
    passSeed.Name = args->PassName;
    passSeed.Source = args->Source;
    passSeed.Stages = args->Stages;
    passSeed.ShaderModel = args->ShaderModel;
    passSeed.Defines = args->Defines;
    passSeed.ProbeDefineSets = args->Probes;
    passSeed.IsOptimize = args->IsOptimize;
    passSeed.EnableUnbounded = args->EnableUnbounded;

    ShaderTemplateSeed seed{};
    seed.Name = args->AssetName;
    seed.Source = args->Source;
    seed.Passes.push_back(std::move(passSeed));

    const ShaderTemplateOptions options{
        .ShaderRoot = args->ShaderRoot,
        .UseSpirvReflection = args->UseSpirv,
        .GenerateVertexInput = args->GenerateVertexInput,
        .ParseKeywordPragmas = args->ParseKeywordPragmas,
        .KeywordPragmaScope = args->KeywordEntryFileOnly
                                  ? ShaderKeywordPragmaScope::EntryFileOnly
                                  : ShaderKeywordPragmaScope::IncludeChain,
        .ProbeDeclaredKeywords = args->AutoProbe};

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, seed, options, diagnostic);
    if (!generated.has_value()) {
        PrintError(diagnostic.ToString());
        return 1;
    }
    for (const ShaderAssetDiagnostic& warning : generated->Warnings) {
        std::fputs(fmt::format("radray_shader_gen: warning: {}\n", warning.ToString()).c_str(), stderr);
    }

    std::optional<string> json = SerializeShaderAssetTemplate(generated.value());
    if (!json.has_value()) {
        PrintError("failed to serialize the generated template");
        return 1;
    }

    if (args->ToStdout) {
        std::fputs(json->c_str(), stdout);
        std::fputc('\n', stdout);
    } else if (!WriteTextFile(args->Output, json.value())) {
        PrintError(fmt::format("failed to write '{}'", args->Output.generic_string()));
        return 1;
    } else {
        std::fputs(
            fmt::format(
                "radray_shader_gen: wrote '{}' with {} item(s) to review\n",
                args->Output.generic_string(),
                generated->Todos.size())
                .c_str(),
            stdout);
    }
    return 0;
#endif
}
