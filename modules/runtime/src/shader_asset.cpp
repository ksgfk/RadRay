#include <radray/runtime/shader_asset.h>

#include <algorithm>
#include <radray/file.h>
#include <radray/json.h>
#include <radray/logger.h>

namespace radray {
namespace {
template <typename T>
bool AssetActive(const T& value, gpu::RenderBackend backend) noexcept {
    return backend == gpu::RenderBackend::D3D12 ? value.D3D12.has_value() : value.Vulkan.has_value();
}
}  // namespace

std::optional<ShaderAsset> ShaderAsset::Create(gpu::Device& device, const ShaderAssetDesc& desc) {
    string diagnostic;
    const auto backend = device.GetBackend();
    if ((backend != gpu::RenderBackend::D3D12 && backend != gpu::RenderBackend::Vulkan) ||
        desc.Family.Variants.empty() || !shader::ValidateShaderFamilyLayoutDesc(desc.Family, &diagnostic)) {
        RADRAY_ERR_LOG("Invalid shader asset family: {}", diagnostic);
        return std::nullopt;
    }
    vector<gpu::ShaderStages> stages(desc.Family.Variants.size());
    for (size_t i = 0; i < desc.Stages.size(); ++i) {
        const auto& entry = desc.Stages[i];
        if (entry.VariantIndex >= desc.Family.Variants.size() || entry.Entry.empty() || entry.Bytecode.empty() ||
            (entry.Stage != gpu::ShaderStage::Vertex && entry.Stage != gpu::ShaderStage::Pixel && entry.Stage != gpu::ShaderStage::Compute)) {
            RADRAY_ERR_LOG("Invalid shader stage record {}", i);
            return std::nullopt;
        }
        const auto target = shader::GetShaderTargetInterface(desc.Family.Variants[entry.VariantIndex], entry.Backend);
        if (!target || !target->Stages.HasFlag(entry.Stage)) return std::nullopt;
        for (size_t j = 0; j < i; ++j)
            if (desc.Stages[j].VariantIndex == entry.VariantIndex && desc.Stages[j].Backend == entry.Backend && desc.Stages[j].Stage == entry.Stage) {
                RADRAY_ERR_LOG("Duplicate shader stage record {}", i);
                return std::nullopt;
            }
        if (entry.Backend == backend) stages[entry.VariantIndex] |= entry.Stage;
    }
    bool available = false;
    for (size_t i = 0; i < stages.size(); ++i) {
        const auto target = shader::GetShaderTargetInterface(desc.Family.Variants[i], backend);
        if (!target) continue;
        const auto expected = target->Stages;
        if (stages[i] != expected ||
            (expected.HasFlag(gpu::ShaderStage::Compute) ? expected != gpu::ShaderStages{gpu::ShaderStage::Compute}
                                                            : !expected.HasFlag(gpu::ShaderStage::Vertex))) {
            RADRAY_ERR_LOG("Missing or invalid shader stages for variant {}", desc.Family.Variants[i].Name);
            return std::nullopt;
        }
        available = true;
    }
    if (!available) return std::nullopt;
    ShaderAsset result;
    result._family = desc.Family;
    result._backend = backend;
    for (const auto& entry : desc.Stages) {
        if (entry.Backend != backend) continue;
        auto shader = device.CreateShader({entry.Bytecode, backend == gpu::RenderBackend::D3D12 ? gpu::ShaderBlobCategory::DXIL : gpu::ShaderBlobCategory::SPIRV, entry.Stage});
        if (!shader) {
            RADRAY_ERR_LOG("Cannot create shader entry {}", entry.Entry);
            return std::nullopt;
        }
        result._stages.push_back({entry.VariantIndex, entry.Stage, entry.Entry, shader.Release()});
    }
    return result;
}

std::optional<ShaderAsset> ShaderAsset::Load(gpu::Device& device, const std::filesystem::path& indexPath) {
    const auto document = JsonDocument::ParseFile(indexPath);
    if (!document) {
        RADRAY_ERR_LOG("Cannot read shader index {}", indexPath.generic_string());
        return std::nullopt;
    }
    const auto root = document->Root();
    uint32_t version = 0;
    string layoutFile;
    if (!DeserializeJsonValue(root["version"], version) || version != 1 ||
        !DeserializeJsonValue(root["layoutBinary"], layoutFile) || layoutFile.empty() || !root["stages"].IsArray()) return std::nullopt;
    ShaderAssetDesc desc;
    const auto bytes = ReadBinaryFile(indexPath.parent_path() / layoutFile);
    if (!bytes || !shader::ReadShaderFamilyLayoutBinary(*bytes, desc.Family)) return std::nullopt;
    const auto backend = device.GetBackend();
    const auto records = root["stages"];
    vector<ShaderStageBytecode> inventory;
    for (size_t i = 0; i < records.Size(); ++i) {
        const auto record = records.At(i);
        string variant, target, stage, file, entry;
        if (!DeserializeJsonValue(record["variant"], variant) || !DeserializeJsonValue(record["backend"], target) ||
            !DeserializeJsonValue(record["stage"], stage) || !DeserializeJsonValue(record["file"], file) ||
            !DeserializeJsonValue(record["entry"], entry) || entry.empty() || file.empty()) return std::nullopt;
        const auto found = std::find_if(desc.Family.Variants.begin(), desc.Family.Variants.end(), [&](const auto& v) { return v.Name == variant; });
        if (found == desc.Family.Variants.end() || (target != "dxil" && target != "spirv") ||
            (stage != "vertex" && stage != "pixel" && stage != "compute")) return std::nullopt;
        ShaderStageBytecode code;
        code.VariantIndex = static_cast<uint32_t>(found - desc.Family.Variants.begin());
        code.Backend = target == "dxil" ? gpu::RenderBackend::D3D12 : gpu::RenderBackend::Vulkan;
        code.Stage = stage == "vertex" ? gpu::ShaderStage::Vertex : stage == "pixel" ? gpu::ShaderStage::Pixel
                                                                                        : gpu::ShaderStage::Compute;
        code.Entry = std::move(entry);
        const auto interface = shader::GetShaderTargetInterface(*found, code.Backend);
        if (!interface || !interface->Stages.HasFlag(code.Stage)) return std::nullopt;
        for (const auto& old : inventory)
            if (old.VariantIndex == code.VariantIndex && old.Backend == code.Backend && old.Stage == code.Stage) return std::nullopt;
        inventory.push_back(code);
        if (code.Backend != backend) continue;
        auto stageBytes = ReadBinaryFile(indexPath.parent_path() / file);
        if (!stageBytes) {
            RADRAY_ERR_LOG("Cannot read shader stage {}", file);
            return std::nullopt;
        }
        code.Bytecode = std::move(*stageBytes);
        desc.Stages.push_back(std::move(code));
    }
    return Create(device, desc);
}

const shader::ShaderPipelineLayoutDesc& ShaderAsset::GetLayoutDesc() const noexcept { return _family.SharedLayout; }
const shader::ShaderFamilyLayoutDesc& ShaderAsset::GetFamilyDesc() const noexcept { return _family; }
gpu::RenderBackend ShaderAsset::GetBackend() const noexcept { return _backend; }
Nullable<const shader::ShaderVariantTargetInterfaceDesc*> ShaderAsset::GetVariantInterface(uint32_t variant) const noexcept {
    return variant < _family.Variants.size() ? shader::GetShaderTargetInterface(_family.Variants[variant], _backend) : nullptr;
}
std::optional<gpu::ShaderEntry> ShaderAsset::GetStage(uint32_t variant, gpu::ShaderStage stage) const noexcept {
    for (const auto& entry : _stages)
        if (entry.VariantIndex == variant && entry.Stage == stage) return gpu::ShaderEntry{entry.Object.get(), entry.Entry};
    return std::nullopt;
}
std::optional<ShaderBindingLocation> ShaderAsset::FindBinding(std::string_view name) const noexcept {
    for (const auto& group : _family.SharedLayout.Groups)
        for (const auto& entry : group.Bindings)
            if (entry.Name == name)
                return ShaderBindingLocation{
                    .Reference = {group.GroupSlot, entry.Binding.BindingId}, .Kind = entry.Binding.Kind,
                    .Count = entry.Binding.Count, .TypeIndex = entry.TypeIndex, .BufferByteSize = entry.BufferByteSize,
                    .ImmutableSampler = entry.Binding.ImmutableSampler.has_value(), .ActiveOnDevice = AssetActive(entry.Binding, _backend)};
    return std::nullopt;
}
std::optional<ShaderPushConstantLocation> ShaderAsset::FindPushConstant(std::string_view name) const noexcept {
    for (const auto& entry : _family.SharedLayout.PushConstants)
        if (entry.Name == name) return ShaderPushConstantLocation{entry.Push.PushId, entry.Push.Size, entry.TypeIndex, AssetActive(entry.Push, _backend)};
    return std::nullopt;
}
std::optional<shader::ShaderResolvedMember> ShaderAsset::FindBufferMember(std::string_view block, std::string_view path) const {
    const auto binding = FindBinding(block);
    if (binding && binding->TypeIndex) return shader::FindShaderTypeMember(_family.SharedLayout.Types, *binding->TypeIndex, path);
    const auto push = FindPushConstant(block);
    if (push && push->TypeIndex) return shader::FindShaderTypeMember(_family.SharedLayout.Types, *push->TypeIndex, path);
    return std::nullopt;
}

ShaderAssetLoader::ShaderAssetLoader(gpu::Device& device, Nullable<const ShaderAsset*> fallback) noexcept
    : _device(device), _fallback(fallback) {}

task<std::optional<ShaderAsset>> ShaderAssetLoader::LoadAsync(AssetLoadContext& context) {
    co_return ShaderAsset::Load(_device, context.GetMeta().Path);
}

Nullable<const ShaderAsset*> ShaderAssetLoader::GetFallback() const noexcept { return _fallback; }

}  // namespace radray
