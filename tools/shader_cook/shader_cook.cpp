// radray_shader_cook —— 把 *.shader.json 烘成 AOT 产物 (index.json + <category>/<key>.bin)。
// 构建期步骤, 消费已人工收敛的 manifest (生成模板的是 radray_shader_gen, 方向相反)。
//
// 用法与排查见 docs/guide/shader-authoring.md;
// 为何刻意不提供 --output 见 docs/adr/0004-content-addressed-shader-artifacts.md。

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include <fmt/format.h>

#include <radray/enum_flags.h>
#include <radray/shader/dxc.h>
#include <radray/shader/shader_manifest.h>
#include <radray/types.h>

namespace {

using namespace radray;

constexpr std::string_view kManifestSuffix = ".shader.json";

constexpr std::string_view kUsage =
    "usage: radray_shader_cook --shader-root <dir>\n"
    "                          [--manifest <path>]... [--discover]\n"
    "                          [--category <dxil|spirv>]...\n"
    "                          [--no-validate-reflection] [--no-incremental]\n"
    "                          [--clean] [--quiet]\n"
    "\n"
    "Cooks every requested manifest into '<manifest without suffix>/index.json' plus\n"
    "'<category>/<key>.bin'. Manifest paths may be absolute or relative to the shader\n"
    "root. With --discover every '*.shader.json' under the shader root is cooked.\n"
    "\n"
    "The default category set follows the backends compiled into this build.\n";

struct Arguments {
    std::filesystem::path ShaderRoot;
    vector<std::filesystem::path> Manifests;
    vector<render::ShaderBlobCategory> Categories;
    bool Discover{false};
    bool ValidateReflection{true};
    bool Incremental{true};
    /// 先删掉整个产物目录再烘。增量从不删除任何东西, 故删过 bake 规则后需要它来清掉
    /// 上一轮遗留的 blob (不影响正确性, 但会一直占着发布包)。
    bool Clean{false};
    bool Quiet{false};
};

void PrintError(std::string_view message) {
    std::fputs(fmt::format("radray_shader_cook: {}\n", message).c_str(), stderr);
}

void PrintInfo(std::string_view message) {
    std::fputs(fmt::format("radray_shader_cook: {}\n", message).c_str(), stdout);
}

bool TakeValue(int argc, char** argv, int& index, std::string_view option, std::string_view& out) {
    if (index + 1 >= argc) {
        PrintError(fmt::format("option '{}' requires a value", option));
        return false;
    }
    out = argv[++index];
    return true;
}

/// "dxil" / "spirv" -> category。只认这两个 —— MSL / METALLIB 不是 cook 的输出
/// (CookShaderAsset 会显式拒绝), 在参数层就挡住比让它走到诊断里更直接。
std::optional<render::ShaderBlobCategory> ParseCategory(std::string_view text) {
    string normalized{text};
    for (char& c : normalized) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (normalized == "DXIL") {
        return render::ShaderBlobCategory::DXIL;
    }
    if (normalized == "SPIRV") {
        return render::ShaderBlobCategory::SPIRV;
    }
    return std::nullopt;
}

/// 本次构建编入了哪些后端就烘哪些字节码。
///
/// 【为何不无条件烘全部】: SPIRV 的反射校验要 spirv-cross (见 shader_asset.cpp 里
/// ValidateCompiled 的 #else 分支), 没有它 ValidateReflection 必然失败。而 Vulkan 后端
/// 与 spirv-cross 的依赖关系已由根 CMakeLists 钉住, 故"编了 Vulkan"即"能校验 SPIRV"。
vector<render::ShaderBlobCategory> DefaultCategories() {
    vector<render::ShaderBlobCategory> result;
#if defined(RADRAY_ENABLE_D3D12)
    result.push_back(render::ShaderBlobCategory::DXIL);
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    result.push_back(render::ShaderBlobCategory::SPIRV);
#endif
    return result;
}

bool IsManifestFile(const std::filesystem::path& path) {
    const string name = path.filename().generic_string();
    return name.size() > kManifestSuffix.size() &&
           std::string_view{name}.substr(name.size() - kManifestSuffix.size()) == kManifestSuffix;
}

/// 递归收集 shader root 下的全部 manifest。结果按路径排序, 使输出可复现。
std::optional<vector<std::filesystem::path>> DiscoverManifests(
    const std::filesystem::path& shaderRoot) {
    vector<std::filesystem::path> result;
    std::error_code error;
    std::filesystem::recursive_directory_iterator it{shaderRoot, error};
    if (error) {
        PrintError(fmt::format(
            "failed to walk '{}': {}", shaderRoot.generic_string(), error.message()));
        return std::nullopt;
    }
    const std::filesystem::recursive_directory_iterator end{};
    for (; it != end; it.increment(error)) {
        if (error) {
            PrintError(fmt::format("failed to walk '{}': {}", shaderRoot.generic_string(), error.message()));
            return std::nullopt;
        }
        if (!it->is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        if (IsManifestFile(it->path())) {
            result.push_back(it->path());
        }
    }
    std::ranges::sort(result);
    return result;
}

std::optional<Arguments> ParseArguments(int argc, char** argv) {
    Arguments args{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view option{argv[i]};
        std::string_view value{};
        if (option == "--shader-root") {
            if (!TakeValue(argc, argv, i, option, value)) return std::nullopt;
            args.ShaderRoot = std::filesystem::path{value};
        } else if (option == "--manifest") {
            if (!TakeValue(argc, argv, i, option, value)) return std::nullopt;
            args.Manifests.emplace_back(value);
        } else if (option == "--category") {
            if (!TakeValue(argc, argv, i, option, value)) return std::nullopt;
            const std::optional<render::ShaderBlobCategory> category = ParseCategory(value);
            if (!category.has_value()) {
                PrintError(fmt::format("unknown category '{}'; expected dxil or spirv", value));
                return std::nullopt;
            }
            if (std::ranges::find(args.Categories, category.value()) == args.Categories.end()) {
                args.Categories.push_back(category.value());
            }
        } else if (option == "--discover") {
            args.Discover = true;
        } else if (option == "--no-validate-reflection") {
            args.ValidateReflection = false;
        } else if (option == "--no-incremental") {
            args.Incremental = false;
        } else if (option == "--clean") {
            args.Clean = true;
        } else if (option == "--quiet") {
            args.Quiet = true;
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
    if (!std::filesystem::is_directory(args.ShaderRoot)) {
        PrintError(fmt::format("'{}' is not a directory", args.ShaderRoot.generic_string()));
        return std::nullopt;
    }
    if (!args.Discover && args.Manifests.empty()) {
        PrintError("either --manifest or --discover is required");
        return std::nullopt;
    }
    if (args.Categories.empty()) {
        args.Categories = DefaultCategories();
        if (args.Categories.empty()) {
            PrintError("this build has no render backend, so there is nothing to cook for; "
                       "pass --category explicitly");
            return std::nullopt;
        }
    }
    return args;
}

/// 相对路径按 shader root 解析 —— manifest 的 Source 字段本来就是这个口径。
std::filesystem::path ResolveManifestPath(
    const std::filesystem::path& shaderRoot,
    const std::filesystem::path& manifest) {
    return manifest.is_absolute() ? manifest : shaderRoot / manifest;
}

string JoinCategories(std::span<const render::ShaderBlobCategory> categories) {
    string text;
    for (const render::ShaderBlobCategory category : categories) {
        if (!text.empty()) {
            text += ", ";
        }
        text += fmt::format("{}", category);
    }
    return text;
}

/// 烘一份 manifest。返回 false 表示失败, 诊断已打印。
bool CookOne(
    render::Dxc& dxc,
    const Arguments& args,
    const std::filesystem::path& manifestPath) {
    if (!std::filesystem::is_regular_file(manifestPath)) {
        PrintError(fmt::format("'{}' is not a file", manifestPath.generic_string()));
        return false;
    }

    if (args.Clean) {
        const std::filesystem::path artifactDir = GetShaderArtifactDirectory(manifestPath);
        std::error_code error;
        std::filesystem::remove_all(artifactDir, error);
        if (error) {
            PrintError(fmt::format(
                "failed to clean '{}': {}", artifactDir.generic_string(), error.message()));
            return false;
        }
    }

    const ShaderCookOptions options{
        .ShaderRoot = args.ShaderRoot,
        .ManifestPath = manifestPath,
        .Categories = args.Categories,
        .ValidateReflection = args.ValidateReflection,
        .Incremental = args.Incremental};
    const ShaderCookResult result = CookShaderAssetFile(dxc, options);

    // 诊断非空即失败 —— ShaderCookResult 没有"仅告警"这一档 (Succeeded() 就是
    // Diagnostics.empty()), 所以每条都当错误报。
    for (const ShaderAssetDiagnostic& diagnostic : result.Diagnostics) {
        PrintError(fmt::format("{}: {}", manifestPath.generic_string(), diagnostic.ToString()));
    }
    if (!result.Succeeded()) {
        return false;
    }

    if (!args.Quiet) {
        PrintInfo(fmt::format(
            "{} -> {} entr{} (compiled {}, reused {}, deduplicated {})",
            manifestPath.generic_string(),
            result.Index.Entries.size(),
            result.Index.Entries.size() == 1 ? "y" : "ies",
            result.Stats.Compiled,
            result.Stats.Reused,
            result.Stats.Deduplicated));
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#if !defined(RADRAY_ENABLE_SHADER_JIT)
    PrintError("this build has no DXC; cooking is unavailable");
    return 1;
#else
    std::optional<Arguments> args = ParseArguments(argc, argv);
    if (!args.has_value()) {
        return 1;
    }

    vector<std::filesystem::path> manifests;
    for (const std::filesystem::path& manifest : args->Manifests) {
        manifests.push_back(ResolveManifestPath(args->ShaderRoot, manifest));
    }
    if (args->Discover) {
        std::optional<vector<std::filesystem::path>> discovered = DiscoverManifests(args->ShaderRoot);
        if (!discovered.has_value()) {
            return 1;
        }
        for (std::filesystem::path& manifest : discovered.value()) {
            if (std::ranges::find(manifests, manifest) == manifests.end()) {
                manifests.push_back(std::move(manifest));
            }
        }
    }
    if (manifests.empty()) {
        // 【为何是错误而不是静默成功】: --discover 一个都没找到, 几乎总是 --shader-root
        // 指错了或部署步骤没跑。静默返回 0 会让构建"成功"却不产出任何 index.json,
        // 而那正是这个工具存在的唯一目的。
        PrintError(fmt::format(
            "no '*{}' found under '{}'", kManifestSuffix, args->ShaderRoot.generic_string()));
        return 1;
    }

    Nullable<shared_ptr<render::Dxc>> dxcResult = render::CreateDxc();
    if (!dxcResult.HasValue()) {
        PrintError("failed to create DXC");
        return 1;
    }
    const shared_ptr<render::Dxc> dxc = dxcResult.Release();

    if (!args->Quiet) {
        PrintInfo(fmt::format(
            "cooking {} manifest(s) for [{}] from '{}'",
            manifests.size(),
            JoinCategories(args->Categories),
            args->ShaderRoot.generic_string()));
    }

    // 一份失败也继续烘剩下的: 一次构建里把所有 manifest 的问题一次报全, 比每次只暴露
    // 第一个更省往返。退出码仍然反映"有失败"。
    uint32_t failures = 0;
    for (const std::filesystem::path& manifest : manifests) {
        if (!CookOne(*dxc, args.value(), manifest)) {
            ++failures;
        }
    }
    if (failures != 0) {
        PrintError(fmt::format("{} of {} manifest(s) failed", failures, manifests.size()));
        return 1;
    }
    return 0;
#endif
}
