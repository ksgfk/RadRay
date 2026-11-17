#include <radray/shader/shader_compiler_contract.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include <radray/file.h>
#include <radray/json.h>
#include <radray/logger.h>
#include <radray/shader/client.h>
#include <radray/types.h>

namespace {

using namespace radray;

constexpr std::string_view kUsage =
    "usage: radray_shader_compile --shader-root <dir> --source <logical.hlsl> "
    "--output <prefix>\n"
    "                              [--target <dxil|spirv|all>] [--include-path <dir>]...\n"
    "                              [--keyword NAME=VALUE]... [--manifest <file>]\n"
    "\n"
    "The source name and angle-bracket includes are shader-root-relative. The tool writes\n"
    "<prefix>.dxil.bin and/or <prefix>.spirv.bin as raw compiler metadata blobs.\n";

struct Arguments {
    std::filesystem::path ShaderRoot;
    vector<std::filesystem::path> IncludePaths;
    string Source;
    std::filesystem::path OutputPrefix;
    shader::ShaderTargetMask Targets{shader::ShaderTargetMask::All};
    vector<shader::KeywordAssignment> Keywords;
    std::filesystem::path Manifest;
};

void PrintError(std::string_view message) {
    std::fputs(radray::format("radray_shader_compile: {}\n", message).c_str(), stderr);
}

bool TakeValue(
    int argc,
    char** argv,
    int& index,
    std::string_view option,
    std::string_view& value) {
    if (index + 1 >= argc) {
        PrintError(radray::format("option '{}' requires a value", option));
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
        } else if (option == "--manifest") {
            if (!TakeValue(argc, argv, index, option, value)) return std::nullopt;
            args.Manifest = std::filesystem::path{value};
        } else if (option == "--keyword") {
            if (!TakeValue(argc, argv, index, option, value)) return std::nullopt;
            const auto equal = value.find('=');
            if (equal == std::string_view::npos || equal == 0 || equal + 1 == value.size() || value.find('=', equal + 1) != std::string_view::npos) {
                PrintError("--keyword expects NAME=VALUE");
                return std::nullopt;
            }
            args.Keywords.push_back({string{value.substr(0, equal)}, string{value.substr(equal + 1)}});
        } else if (option == "--target") {
            if (!TakeValue(argc, argv, index, option, value)) {
                return std::nullopt;
            }
            const std::optional<shader::ShaderTargetMask> targets = ParseTargets(value);
            if (!targets.has_value()) {
                PrintError(radray::format("unknown target '{}'; expected dxil, spirv, or all", value));
                return std::nullopt;
            }
            args.Targets = targets.value();
        } else if (option == "--help" || option == "-h") {
            std::fputs(string{kUsage}.c_str(), stdout);
            return std::nullopt;
        } else {
            PrintError(radray::format("unknown option '{}'", option));
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
        PrintError(radray::format("'{}' is not a readable shader root", args.ShaderRoot.generic_string()));
        return std::nullopt;
    }
    if (!shader::IsLogicalSourceName(args.Source)) {
        PrintError(radray::format("source '{}' is not a logical root-relative name", args.Source));
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

void PrintDiagnostics(std::string_view diagnostics) {
    if (!diagnostics.empty()) PrintError(diagnostics);
}

}  // namespace

int main(int argc, char** argv) {
    const std::optional<Arguments> args = ParseArguments(argc, argv);
    if (!args.has_value()) {
        return 1;
    }

    const std::optional<vector<byte>> source = ReadBinaryFile(args->ShaderRoot / args->Source);
    if (!source.has_value() || source->empty()) {
        PrintError(radray::format("source '{}' could not be read", args->Source));
        return 1;
    }

    shader::Client client;
    if (!client.IsAvailable()) {
        PrintError("RadRay DXC compiler client is unavailable");
        return 1;
    }

    const shader::DiscoveryResult discovery = client.DiscoverSourceContract(
        args->Source,
        *source,
        shader::ShaderTarget::DXIL,
        args->IncludePaths);
    PrintDiagnostics(discovery.Diagnostics);
    if (!discovery.Succeeded()) {
        return 1;
    }

    vector<shader::KeywordAssignment> assignments = DefaultAssignments(*discovery.Value);
    if (assignments.size() != discovery.Value->KeywordGroups.size()) {
        PrintError("every keyword group must declare at least one value");
        return 1;
    }

    if (shader::HasDuplicateNames(args->Keywords)) {
        PrintError("duplicate --keyword name");
        return 1;
    }
    for (const auto& keyword : args->Keywords) {
        bool found = false;
        for (size_t index = 0; index < discovery.Value->KeywordGroups.size(); ++index) {
            const auto& group = discovery.Value->KeywordGroups[index];
            if (group.Name != keyword.Name) continue;
            found = std::find(group.Values.begin(), group.Values.end(), keyword.Value) != group.Values.end();
            if (found) assignments[index] = keyword;
            break;
        }
        if (!found) {
            PrintError(radray::format("unknown keyword name or value: {}={}", keyword.Name, keyword.Value));
            return 1;
        }
    }

    const shader::CompileVariantResult result = client.CompileVariant(shader::CompileVariantRequest{
                                                                          .SourceName = args->Source,
                                                                          .RootSource = *source,
                                                                          .Defines = {},
                                                                          .Assignments = assignments,
                                                                          .Targets = args->Targets,
                                                                          .Policy = {},
                                                                          .ExpectedContract = discovery.Value->Hash},
                                                                      args->IncludePaths);
    if (!result.Diagnostics.empty()) {
        PrintDiagnostics(result.Diagnostics);
    }
    if (!result.Succeeded()) {
        return 1;
    }

    JsonWriter manifest;
    auto root = manifest.RootObject();
    root.AddUint("version", 1);
    auto variant = root.AddArray("variants").AppendObject();
    variant.AddString("name", "Default");
    for (const shader::CompileTargetLane& lane : *result.Value) {
        const string suffix = radray::format(".{}.bin", TargetName(lane.Target));
        std::filesystem::path output = args->OutputPrefix;
        output += suffix;
        if (!args->Manifest.empty()) {
            shader::WireMetadataEnvelope envelope{};
            if (lane.Metadata.size() < sizeof(envelope)) return 1;
            shader::DecodeShaderMetadataEnvelope(std::span<const byte>{lane.Metadata}.first(152), envelope);
            std::error_code error;
            const auto relative = std::filesystem::relative(output, args->Manifest.parent_path().empty() ? "." : args->Manifest.parent_path(), error);
            if (error) {
                PrintError("cannot resolve manifest artifact path");
                return 1;
            }
            auto target = variant.AddObject(lane.Target == shader::ShaderTarget::DXIL ? "dxil" : "spirv");
            target.AddString("path", relative.generic_string());
            target.AddString("gpuArtifactHash", envelope.GpuArtifact.ToHex());
            target.AddUint("toolchainIdentity", envelope.ToolchainIdentity);
        }
    }
    if (!args->Manifest.empty()) {
        std::error_code error;
        std::filesystem::remove(args->Manifest, error);
        if (error) {
            PrintError("cannot invalidate previous compilation manifest");
            return 1;
        }
    }
    for (const shader::CompileTargetLane& lane : *result.Value) {
        std::filesystem::path output = args->OutputPrefix;
        output += radray::format(".{}.bin", TargetName(lane.Target));
        if (!WriteBinaryFile(output, lane.Metadata)) {
            PrintError(radray::format("failed to write '{}'", output.generic_string()));
            return 1;
        }
        std::fputs(
            radray::format(
                "radray_shader_compile: wrote {} ({} bytes)\n",
                output.generic_string(),
                lane.Metadata.size())
                .c_str(),
            stdout);
    }
    if (!args->Manifest.empty() && !manifest.WriteFile(args->Manifest)) {
        PrintError("failed to write compilation manifest");
        return 1;
    }
    return 0;
}
