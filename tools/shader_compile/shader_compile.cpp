#include <cctype>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include <fmt/format.h>

#include <radray/file.h>
#include <radray/shader_compiler/client.h>
#include <radray/types.h>

namespace {

using namespace radray;

constexpr std::string_view kUsage =
    "usage: radray_shader_compile --shader-root <dir> --source <logical.hlsl> "
    "--output <prefix>\n"
    "                              [--target <dxil|spirv|all>] [--include-path <dir>]...\n"
    "\n"
    "The source name and angle-bracket includes are shader-root-relative. The tool writes\n"
    "<prefix>.dxil.bin and/or <prefix>.spirv.bin as raw compiler metadata blobs.\n";

struct Arguments {
    std::filesystem::path ShaderRoot;
    vector<std::filesystem::path> IncludePaths;
    string Source;
    std::filesystem::path OutputPrefix;
    shader::ShaderTargetMask Targets{shader::ShaderTargetMask::All};
};

void PrintError(std::string_view message) {
    std::fputs(fmt::format("radray_shader_compile: {}\n", message).c_str(), stderr);
}

bool TakeValue(
    int argc,
    char** argv,
    int& index,
    std::string_view option,
    std::string_view& value) {
    if (index + 1 >= argc) {
        PrintError(fmt::format("option '{}' requires a value", option));
        return false;
    }
    value = argv[++index];
    return true;
}

std::optional<shader::ShaderTargetMask> ParseTargets(std::string_view value) {
    string normalized{value};
    for (char& character : normalized) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (normalized == "dxil") {
        return shader::ShaderTargetMask::DXIL;
    }
    if (normalized == "spirv") {
        return shader::ShaderTargetMask::SPIRV;
    }
    if (normalized == "all") {
        return shader::ShaderTargetMask::All;
    }
    return std::nullopt;
}

std::optional<Arguments> ParseArguments(int argc, char** argv) {
    Arguments args{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        std::string_view value{};
        if (option == "--shader-root") {
            if (!TakeValue(argc, argv, index, option, value)) {
                return std::nullopt;
            }
            args.ShaderRoot = std::filesystem::path{value};
        } else if (option == "--source") {
            if (!TakeValue(argc, argv, index, option, value)) {
                return std::nullopt;
            }
            args.Source = string{value};
        } else if (option == "--include-path") {
            if (!TakeValue(argc, argv, index, option, value)) {
                return std::nullopt;
            }
            args.IncludePaths.emplace_back(value);
        } else if (option == "--output") {
            if (!TakeValue(argc, argv, index, option, value)) {
                return std::nullopt;
            }
            args.OutputPrefix = std::filesystem::path{value};
        } else if (option == "--target") {
            if (!TakeValue(argc, argv, index, option, value)) {
                return std::nullopt;
            }
            const std::optional<shader::ShaderTargetMask> targets = ParseTargets(value);
            if (!targets.has_value()) {
                PrintError(fmt::format("unknown target '{}'; expected dxil, spirv, or all", value));
                return std::nullopt;
            }
            args.Targets = targets.value();
        } else if (option == "--help" || option == "-h") {
            std::fputs(string{kUsage}.c_str(), stdout);
            return std::nullopt;
        } else {
            PrintError(fmt::format("unknown option '{}'", option));
            std::fputs(string{kUsage}.c_str(), stderr);
            return std::nullopt;
        }
    }

    if (args.ShaderRoot.empty() || args.Source.empty() || args.OutputPrefix.empty()) {
        PrintError("--shader-root, --source, and --output are required");
        return std::nullopt;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(args.ShaderRoot, error) || error) {
        PrintError(fmt::format("'{}' is not a readable shader root", args.ShaderRoot.generic_string()));
        return std::nullopt;
    }
    if (!shader::IsLogicalSourceName(args.Source)) {
        PrintError(fmt::format("source '{}' is not a logical root-relative name", args.Source));
        return std::nullopt;
    }
    args.IncludePaths.insert(args.IncludePaths.begin(), args.ShaderRoot);
    return args;
}

vector<shader::KeywordAssignment> DefaultAssignments(const shader::ShaderContract& contract) {
    vector<shader::KeywordAssignment> result;
    result.reserve(contract.KeywordGroups.size());
    for (const shader::KeywordGroup& group : contract.KeywordGroups) {
        if (group.Values.empty()) {
            return {};
        }
        result.push_back({group.Name, group.Values.front()});
    }
    return result;
}

std::string_view TargetName(shader::ShaderTarget target) noexcept {
    return target == shader::ShaderTarget::DXIL ? "dxil" : "spirv";
}

void PrintDiagnostics(const vector<shader::CompileDiagnostic>& diagnostics) {
    for (const shader::CompileDiagnostic& diagnostic : diagnostics) {
        PrintError(fmt::format("{}: {}", diagnostic.Code, diagnostic.Message));
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::optional<Arguments> args = ParseArguments(argc, argv);
    if (!args.has_value()) {
        return 1;
    }

    const std::optional<vector<byte>> source = ReadBinaryFile(args->ShaderRoot / args->Source);
    if (!source.has_value() || source->empty()) {
        PrintError(fmt::format("source '{}' could not be read", args->Source));
        return 1;
    }

    shader_compiler::Client client;
    if (!client.IsAvailable()) {
        PrintError("RadRay DXC compiler client is unavailable");
        return 1;
    }

    const shader_compiler::DiscoveryResult discovery = client.DiscoverSourceContract(
        args->Source,
        *source,
        shader::ShaderTarget::DXIL,
        args->IncludePaths);
    if (!discovery.Succeeded()) {
        PrintDiagnostics(discovery.Diagnostics);
        return 1;
    }

    const vector<shader::KeywordAssignment> assignments = DefaultAssignments(discovery.Contract);
    if (assignments.size() != discovery.Contract.KeywordGroups.size()) {
        PrintError("every keyword group must declare at least one value");
        return 1;
    }

    const shader::CompileVariantResult result = client.CompileVariant(shader::CompileVariantRequest{
        .SourceName = args->Source,
        .RootSource = *source,
        .Defines = {},
        .Assignments = assignments,
        .Targets = args->Targets,
        .Policy = {},
        .ExpectedContract = discovery.Contract.Hash},
        args->IncludePaths);
    if (!result.Diagnostics.empty()) {
        PrintDiagnostics(result.Diagnostics);
    }
    if (result.Status != shader::CompileStatus::Success) {
        return 1;
    }

    for (const shader::CompileTargetLane& lane : result.Lanes) {
        const std::string suffix = fmt::format(".{}.bin", TargetName(lane.Target));
        std::filesystem::path output = args->OutputPrefix;
        output += suffix;
        if (!WriteBinaryFile(output, lane.Metadata)) {
            PrintError(fmt::format("failed to write '{}'", output.generic_string()));
            return 1;
        }
        std::fputs(
            fmt::format(
                "radray_shader_compile: wrote {} ({} bytes)\n",
                output.generic_string(),
                lane.Metadata.size())
                .c_str(),
            stdout);
    }
    return 0;
}
