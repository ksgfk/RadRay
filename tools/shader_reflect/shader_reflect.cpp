#include "shader_reflect.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <system_error>

#include <radray/file.h>
#include <radray/json.h>
#include <radray/logger.h>
#include <radray/shader/shader_reflector.h>

namespace radray::shader {
namespace {

struct OwnedVariant {
    string Name;
    std::optional<shader::DxilShaderArtifactView> DXIL;
    std::optional<shader::SpirvShaderArtifactView> SPIRV;
};

struct OutputFile {
    std::filesystem::path Path;
    vector<byte> Bytes;
};

bool Error(std::string_view message) {
    std::fputs(radray::format("radray_shader_reflect: {}\n", message).c_str(), stderr);
    return false;
}

bool ReadExpected(JsonValue value, shader::ShaderArtifactDecodeOptions& options, std::filesystem::path& path) {
    string name, hash;
    if (!value.IsObject() || !DeserializeJsonValue(value["path"], name) || name.empty() ||
        !DeserializeJsonValue(value["gpuArtifactHash"], hash) || hash.size() != 32 ||
        !DeserializeJsonValue(value["toolchainIdentity"], options.ExpectedToolchainIdentity) || !options.ExpectedToolchainIdentity) return false;
    for (size_t i = 0; i < 16; ++i) {
        unsigned byteValue = 0;
        const auto parsed = std::from_chars(hash.data() + i * 2, hash.data() + i * 2 + 2, byteValue, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != hash.data() + i * 2 + 2) return false;
        options.ExpectedGpuArtifact.Bytes[i] = static_cast<uint8_t>(byteValue);
    }
    path = name;
    return true;
}

bool ReadPolicy(JsonValue value, ShaderReflectionPolicy& policy) {
    if (!value.IsValid()) return true;
    if (!value.IsObject()) return false;
    if (value.Has("allowTargetOnlyBindings") && !DeserializeJsonValue(value["allowTargetOnlyBindings"], policy.AllowTargetOnlyBindings)) return false;
    if (value.Has("bindings")) {
        const auto list = value["bindings"];
        if (!list.IsArray()) return false;
        for (size_t i = 0; i < list.Size(); ++i) {
            const auto item = list.At(i);
            ShaderBindingOverride rule;
            if (!item.IsObject() || !DeserializeJsonValue(item["name"], rule.Name)) return false;
            if (item.Has("groupSlot")) {
                uint32_t slot;
                if (!DeserializeJsonValue(item["groupSlot"], slot)) return false;
                rule.GroupSlot = slot;
            }
            if (item.Has("bindingId")) {
                uint32_t id;
                if (!DeserializeJsonValue(item["bindingId"], id)) return false;
                rule.BindingId = id;
            }
            if (item.Has("groupName")) {
                string name;
                if (!DeserializeJsonValue(item["groupName"], name)) return false;
                rule.GroupName = std::move(name);
            }
            policy.Bindings.push_back(std::move(rule));
        }
    }
    if (value.Has("pushConstants")) {
        const auto list = value["pushConstants"];
        if (!list.IsArray()) return false;
        for (size_t i = 0; i < list.Size(); ++i) {
            const auto item = list.At(i);
            ShaderPushConstantOverride rule;
            if (!item.IsObject() || !DeserializeJsonValue(item["name"], rule.Name) || !DeserializeJsonValue(item["pushId"], rule.PushId)) return false;
            if (item.Has("promoteDxilCBufferToRootConstants") && !DeserializeJsonValue(item["promoteDxilCBufferToRootConstants"], rule.PromoteDxilCBufferToRootConstants)) return false;
            if (item.Has("allowDxilOnly") && !DeserializeJsonValue(item["allowDxilOnly"], rule.AllowDxilOnly)) return false;
            policy.PushConstants.push_back(std::move(rule));
        }
    }
    return true;
}

bool LoadVariants(const std::filesystem::path& manifest, vector<OwnedVariant>& output, ShaderReflectionPolicy& policy) {
    const auto document = JsonDocument::ParseFile(manifest);
    if (!document) return Error("cannot parse input manifest");
    const auto root = document->Root();
    uint32_t version = 0;
    if (!DeserializeJsonValue(root["version"], version) || version != 1 || !ReadPolicy(root["policy"], policy)) return Error("unsupported manifest version or invalid policy");
    const auto variants = root["variants"];
    if (!variants.IsArray() || variants.Size() == 0) return Error("manifest needs a nonempty variants array");
    for (size_t i = 0; i < variants.Size(); ++i) {
        const auto item = variants.At(i);
        OwnedVariant variant;
        if (!DeserializeJsonValue(item["name"], variant.Name) || variant.Name.empty()) return Error("variant name is required");
        for (bool dxil : {true, false}) {
            const auto target = item[dxil ? "dxil" : "spirv"];
            if (!target.IsValid()) continue;
            shader::ShaderArtifactDecodeOptions options{};
            options.Target = dxil ? shader::ShaderTarget::DXIL : shader::ShaderTarget::SPIRV;
            std::filesystem::path path;
            if (!ReadExpected(target, options, path)) return Error("target needs path, external gpuArtifactHash and toolchainIdentity");
            auto bytes = ReadBinaryFile(manifest.parent_path() / path);
            if (!bytes) return Error(radray::format("cannot read '{}'", path.generic_string()));
            if (dxil) {
                auto decoded = shader::DecodeDxilShaderArtifact(*bytes, options);
                if (!decoded.Succeeded()) return Error(decoded.Diagnostics);
                variant.DXIL = std::move(*decoded.Value);
            } else {
                auto decoded = shader::DecodeSpirvShaderArtifact(*bytes, options);
                if (!decoded.Succeeded()) return Error(decoded.Diagnostics);
                variant.SPIRV = std::move(*decoded.Value);
            }
        }
        output.push_back(std::move(variant));
    }
    return true;
}

std::string_view StageName(shader::ShaderStage stage) {
    switch (stage) {
        case shader::ShaderStage::Vertex: return "vertex";
        case shader::ShaderStage::Pixel: return "pixel";
        case shader::ShaderStage::Compute: return "compute";
    }
    return "invalid";
}

}  // namespace

int RunShaderReflect(int argc, char** argv) {
    std::filesystem::path manifest, outputPrefix;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option{argv[i]};
        if (option == "--help" || option == "-h") {
            std::fputs("usage: radray_shader_reflect --manifest <file> --output <prefix>\n", stdout);
            return 0;
        }
        if (i + 1 >= argc || (option != "--manifest" && option != "--output")) {
            Error("expected --manifest <file> --output <prefix>");
            return 1;
        }
        (option == "--manifest" ? manifest : outputPrefix) = argv[++i];
    }
    if (manifest.empty() || outputPrefix.empty()) {
        Error("--manifest and --output are required");
        return 1;
    }
    vector<OwnedVariant> owned;
    ShaderReflectionPolicy policy;
    if (!LoadVariants(manifest, owned, policy)) return 1;
    vector<ShaderVariantArtifacts> views;
    for (const auto& variant : owned) views.push_back({variant.Name,
                                                       variant.DXIL ? &*variant.DXIL : Nullable<const shader::DxilShaderArtifactView*>{nullptr},
                                                       variant.SPIRV ? &*variant.SPIRV : Nullable<const shader::SpirvShaderArtifactView*>{nullptr}});
    auto result = ReflectShaderFamily(views, policy);
    if (!result.Succeeded()) {
        Error(result.Diagnostics);
        return 1;
    }
    auto json = shader::WriteShaderFamilyLayoutJson(*result.Value);
    auto binary = shader::WriteShaderFamilyLayoutBinary(*result.Value);
    if (!json || !binary) {
        Error("cannot encode layout");
        return 1;
    }
    vector<OutputFile> files;
    files.push_back({outputPrefix.string() + ".layout.json", vector<byte>(reinterpret_cast<const byte*>(json->data()), reinterpret_cast<const byte*>(json->data() + json->size()))});
    files.push_back({outputPrefix.string() + ".layout.bin", std::move(*binary)});
    JsonWriter index;
    auto indexRoot = index.RootObject();
    indexRoot.AddUint("version", 1);
    indexRoot.AddString("layoutJson", files[0].Path.filename().generic_string());
    indexRoot.AddString("layoutBinary", files[1].Path.filename().generic_string());
    auto stages = indexRoot.AddArray("stages");
    // Family variant order is canonical, independent of manifest order.
    for (size_t i = 0; i < result.Value->Variants.size(); ++i) {
        const auto& name = result.Value->Variants[i].Name;
        const auto found = std::find_if(owned.begin(), owned.end(), [&](const auto& v) { return v.Name == name; });
        for (bool dxil : {true, false}) {
            Nullable<const shader::ShaderArtifactView*> artifact = dxil ? (found->DXIL ? &found->DXIL->Generic() : nullptr) : (found->SPIRV ? &found->SPIRV->Generic() : nullptr);
            if (!artifact) continue;
            for (const auto& entry : artifact->Entries()) {
                const auto stage = static_cast<shader::ShaderStage>(entry.Stage);
                const auto bytes = artifact->FindStageBytecode(stage);
                if (!bytes) {
                    Error("entry has no bytecode");
                    return 1;
                }
                const auto path = std::filesystem::path{radray::format("{}.{}.{}.{}.bin", outputPrefix.generic_string(), i, dxil ? "dxil" : "spirv", StageName(stage))};
                files.push_back({path, vector<byte>{bytes->begin(), bytes->end()}});
                auto record = stages.AppendObject();
                record.AddString("variant", name);
                record.AddString("backend", dxil ? "dxil" : "spirv");
                record.AddString("entry", *artifact->GetName(entry.Name));
                record.AddString("stage", StageName(stage));
                record.AddString("file", path.filename().generic_string());
            }
        }
    }
    // Never leave an old success index referring to a partially replaced output set.
    const std::filesystem::path indexPath{outputPrefix.string() + ".index.json"};
    std::error_code error;
    std::filesystem::remove(indexPath, error);
    if (error) {
        Error("cannot invalidate previous output index");
        return 1;
    }
    for (const auto& file : files)
        if (!WriteBinaryFile(file.Path, file.Bytes)) {
            Error(radray::format("cannot write '{}'", file.Path.generic_string()));
            return 1;
        }
    if (!index.WriteFile(indexPath)) {
        Error("cannot publish output index");
        return 1;
    }
    std::fputs(radray::format("radray_shader_reflect: wrote {} variants to {}\n", owned.size(), indexPath.generic_string()).c_str(), stdout);
    return 0;
}

}  // namespace radray::shader

#if !defined(RADRAY_SHADER_REFLECT_NO_MAIN)
int main(int argc, char** argv) { return radray::shader::RunShaderReflect(argc, argv); }
#endif
