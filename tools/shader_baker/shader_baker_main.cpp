// radray_shader_baker: 极简 shader 烘焙 helper (供 tools/bake_shaders.py 调度)。
//
// 只做一件事: 编译单个 stage -> 输出字节码 blob + 反射 JSON sidecar。
// 反射提取依赖 DXC (DXIL) 与 SPIRV-Cross (SPIR-V), 只能在 C++ 侧完成, 故以独立可执行提供。
// 不创建 GPU Device, 仅用 Dxc + Reflect* 函数。
//
// 用法:
//   radray_shader_baker
//       --source <file.hlsl> --entry <name> --stage vertex|pixel|compute
//       --target dxil|spirv --sm SM60
//       --out-blob <path> --out-refl <path.json>
//       [--define NAME=1]... [--include <dir>]... [--optimize]

#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/logger.h>
#include <radray/file.h>
#include <radray/render/common.h>
#include <radray/render/shader/hlsl.h>
#include <radray/render/shader/spirv.h>
#include <radray/render/shader_compiler/dxc.h>

#ifdef RADRAY_ENABLE_SPIRV_CROSS
#include <radray/render/shader_compiler/spvc.h>
#endif

using namespace radray;
using namespace radray::render;

namespace {

struct Args {
    string Source;
    string Entry;
    string Stage;
    string Target;
    string Sm{"SM60"};
    string OutBlob;
    string OutRefl;
    vector<string> Defines;
    vector<string> Includes;
    bool Optimize{false};
};

bool ParseArgs(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                RADRAY_ERR_LOG("baker: missing value for {}", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--source") { const char* v = next("--source"); if (!v) return false; out.Source = v; }
        else if (a == "--entry") { const char* v = next("--entry"); if (!v) return false; out.Entry = v; }
        else if (a == "--stage") { const char* v = next("--stage"); if (!v) return false; out.Stage = v; }
        else if (a == "--target") { const char* v = next("--target"); if (!v) return false; out.Target = v; }
        else if (a == "--sm") { const char* v = next("--sm"); if (!v) return false; out.Sm = v; }
        else if (a == "--out-blob") { const char* v = next("--out-blob"); if (!v) return false; out.OutBlob = v; }
        else if (a == "--out-refl") { const char* v = next("--out-refl"); if (!v) return false; out.OutRefl = v; }
        else if (a == "--define") { const char* v = next("--define"); if (!v) return false; out.Defines.emplace_back(v); }
        else if (a == "--include") { const char* v = next("--include"); if (!v) return false; out.Includes.emplace_back(v); }
        else if (a == "--optimize") { out.Optimize = true; }
        else {
            RADRAY_ERR_LOG("baker: unknown argument '{}'", a);
            return false;
        }
    }
    if (out.Source.empty() || out.Entry.empty() || out.Stage.empty() ||
        out.Target.empty() || out.OutBlob.empty() || out.OutRefl.empty()) {
        RADRAY_ERR_LOG("baker: --source/--entry/--stage/--target/--out-blob/--out-refl are required");
        return false;
    }
    return true;
}

std::optional<ShaderStage> ParseStage(std::string_view s) {
    if (s == "vertex") return ShaderStage::Vertex;
    if (s == "pixel") return ShaderStage::Pixel;
    if (s == "compute") return ShaderStage::Compute;
    RADRAY_ERR_LOG("baker: unsupported stage '{}'", s);
    return std::nullopt;
}

std::optional<HlslShaderModel> ParseSm(std::string_view s) {
    if (s == "SM60") return HlslShaderModel::SM60;
    if (s == "SM61") return HlslShaderModel::SM61;
    if (s == "SM62") return HlslShaderModel::SM62;
    if (s == "SM63") return HlslShaderModel::SM63;
    if (s == "SM64") return HlslShaderModel::SM64;
    if (s == "SM65") return HlslShaderModel::SM65;
    if (s == "SM66") return HlslShaderModel::SM66;
    RADRAY_ERR_LOG("baker: unsupported shader model '{}'", s);
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
    Args args{};
    if (!ParseArgs(argc, argv, args)) {
        return 2;
    }

    auto stageOpt = ParseStage(args.Stage);
    auto smOpt = ParseSm(args.Sm);
    if (!stageOpt.has_value() || !smOpt.has_value()) {
        return 2;
    }
    const bool isSpirv = args.Target == "spirv";
    if (!isSpirv && args.Target != "dxil") {
        RADRAY_ERR_LOG("baker: unsupported target '{}' (dxil|spirv)", args.Target);
        return 2;
    }

    std::optional<string> code = ReadTextFile(args.Source);
    if (!code.has_value()) {
        RADRAY_ERR_LOG("baker: cannot read source '{}'", args.Source);
        return 1;
    }

    auto dxcOpt = CreateDxc();
    if (!dxcOpt.HasValue()) {
        RADRAY_ERR_LOG("baker: failed to create Dxc");
        return 1;
    }
    shared_ptr<Dxc> dxc = dxcOpt.Release();

    vector<std::string_view> defineViews;
    defineViews.reserve(args.Defines.size());
    for (const string& d : args.Defines) {
        defineViews.emplace_back(d);
    }
    vector<std::string_view> includeViews;
    includeViews.reserve(args.Includes.size());
    for (const string& inc : args.Includes) {
        includeViews.emplace_back(inc);
    }

    DxcCompileParams params{};
    params.Code = code.value();
    params.EntryPoint = args.Entry;
    params.Stage = stageOpt.value();
    params.SM = smOpt.value();
    params.Defines = defineViews;
    params.Includes = includeViews;
    params.IsOptimize = args.Optimize;
    params.IsSpirv = isSpirv;
    params.EnableUnbounded = false;

    auto outputOpt = dxc->Compile(params);
    if (!outputOpt.has_value()) {
        RADRAY_ERR_LOG("baker: DXC compile failed (entry '{}', target '{}')", args.Entry, args.Target);
        return 1;
    }
    DxcOutput output = std::move(outputOpt.value());

    // 提取反射 -> JSON
    std::optional<string> reflJson;
    if (isSpirv) {
#ifdef RADRAY_ENABLE_SPIRV_CROSS
        auto reflOpt = ReflectSpirv(SpirvBytecodeView{
            .Data = output.Data,
            .EntryPointName = args.Entry,
            .Stage = stageOpt.value(),
        });
        if (!reflOpt.has_value()) {
            RADRAY_ERR_LOG("baker: SPIR-V reflection failed (entry '{}')", args.Entry);
            return 1;
        }
        reflJson = SerializeSpirvShaderDesc(reflOpt.value());
#else
        RADRAY_ERR_LOG("baker: SPIR-V target requires SPIRV-Cross (RADRAY_ENABLE_SPVCROSS)");
        return 1;
#endif
    } else {
        auto reflOpt = dxc->GetShaderDescFromOutput(output.Refl);
        if (!reflOpt.has_value()) {
            RADRAY_ERR_LOG("baker: DXIL reflection failed (entry '{}')", args.Entry);
            return 1;
        }
        reflJson = SerializeHlslShaderDesc(reflOpt.value());
    }
    if (!reflJson.has_value()) {
        RADRAY_ERR_LOG("baker: reflection serialize failed (entry '{}')", args.Entry);
        return 1;
    }

    if (!WriteBinaryFile(args.OutBlob, std::span<const byte>{output.Data.data(), output.Data.size()})) {
        RADRAY_ERR_LOG("baker: failed to write blob '{}'", args.OutBlob);
        return 1;
    }
    if (!WriteTextFile(args.OutRefl, reflJson.value())) {
        RADRAY_ERR_LOG("baker: failed to write reflection '{}'", args.OutRefl);
        return 1;
    }

    RADRAY_INFO_LOG("baker: {} [{}] {} -> {}", args.Entry, args.Target, args.Stage, args.OutBlob);
    return 0;
}
