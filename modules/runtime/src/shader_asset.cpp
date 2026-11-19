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
    ShaderAsset result{device};
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

shader::DiagnosticResult<ShaderAsset> ShaderAsset::Compile(
    gpu::Device& device, const shader::Client& client, const ShaderCompileDesc& desc) {
    const Uri& uri = desc.SourceUri;
    string diagnostics;
    const auto failure = [&](std::string_view reason) -> shader::DiagnosticResult<ShaderAsset> {
        shader::AppendDiagnostic(diagnostics, reason);
        return {std::nullopt, radray::format("{}: {}", uri.AsString(), diagnostics)};
    };
    if (uri.Scheme() != "virtual" || uri.Host().empty() || uri.Authority() != uri.Host() ||
        uri.AbsolutePath().empty() || uri.AbsolutePath().ends_with('/') || !uri.Query().empty() || !uri.Fragment().empty())
        return failure("expected virtual://host/path with a root source filename");
    string sourceName{uri.Host()};
    sourceName += uri.AbsolutePath();
    if (!shader::IsLogicalSourceName(sourceName)) return failure("invalid compiler source name");
    if (!client.IsAvailable()) return failure("shader compiler unavailable");
    const auto backend = device.GetBackend();
    if (backend != gpu::RenderBackend::D3D12 && backend != gpu::RenderBackend::Vulkan) return failure("unsupported backend");
    const bool dxil = backend == gpu::RenderBackend::D3D12;
    const auto target = dxil ? shader::ShaderTarget::DXIL : shader::ShaderTarget::SPIRV;
    shader::SourceContractRequest source{
        sourceName, {desc.Source.begin(), desc.Source.end()}, desc.Defines,
        dxil ? shader::ShaderTargetMask::DXIL : shader::ShaderTargetMask::SPIRV, desc.Policy};
    const auto discovery = client.DiscoverSourceContract(source, desc.IncludePaths);
    shader::AppendDiagnostic(diagnostics, discovery.Diagnostics);
    if (!discovery.Succeeded()) return failure("source contract discovery failed");
    shader::CompileVariantRequest request{
        std::move(source.SourceName), std::move(source.RootSource), std::move(source.Defines), {},
        source.Targets, source.Policy, discovery.Value->Hash};

    const ShaderVariantCompileDesc defaultVariant{};
    const auto variants = desc.Variants.empty() ? std::span{&defaultVariant, 1} : std::span<const ShaderVariantCompileDesc>{desc.Variants};
    vector<shader::DxilShaderArtifactView> dxArtifacts;
    vector<shader::SpirvShaderArtifactView> vkArtifacts;
    if (dxil) dxArtifacts.reserve(variants.size());
    else vkArtifacts.reserve(variants.size());
    vector<shader::ShaderVariantArtifacts> family;
    ShaderAssetDesc asset;
    for (const auto& variant : variants) {
        request.Assignments = variant.Keywords;
        auto compiled = client.CompileVariant(request, desc.IncludePaths);
        shader::AppendDiagnostic(diagnostics, compiled.Diagnostics);
        if (!compiled.Succeeded() || compiled.Value->size() != 1) return failure(radray::format("compiling variant {}", variant.Name));
        auto& lane = compiled.Value->front();
        shader::WireMetadataEnvelope envelope{};
        const std::span<const byte> metadata{lane.Metadata};
        if (metadata.size() < sizeof(envelope) || !shader::DecodeShaderMetadataEnvelope(metadata.first(sizeof(envelope)), envelope))
            return failure("invalid compiler metadata envelope");
        shader::ShaderArtifactDecodeOptions options{target, envelope.GpuArtifact, shader::kShaderToolchainIdentity};
        shader::ShaderVariantArtifacts reflected{variant.Name};
        if (dxil) {
            auto result = shader::DecodeDxilShaderArtifact(metadata, options);
            if (!result.Succeeded()) return failure(result.Diagnostics);
            reflected.DXIL = &dxArtifacts.emplace_back(std::move(*result.Value));
        } else {
            auto result = shader::DecodeSpirvShaderArtifact(metadata, options);
            if (!result.Succeeded()) return failure(result.Diagnostics);
            reflected.SPIRV = &vkArtifacts.emplace_back(std::move(*result.Value));
        }
        for (auto& stage : lane.Stages) {
            const auto gpuStage = stage.Stage == shader::ShaderStage::Vertex ? gpu::ShaderStage::Vertex :
                                  stage.Stage == shader::ShaderStage::Pixel ? gpu::ShaderStage::Pixel : gpu::ShaderStage::Compute;
            asset.Stages.push_back({static_cast<uint32_t>(family.size()), backend, gpuStage, std::move(stage.EntryPoint), std::move(stage.Bytecode)});
        }
        family.push_back(std::move(reflected));
    }
    auto layout = shader::ReflectShaderFamily(family, desc.ReflectionPolicy);
    shader::AppendDiagnostic(diagnostics, layout.Diagnostics);
    if (!layout.Succeeded()) return failure("shader family reflection failed");
    asset.Family.SharedLayout = std::move(layout.Value->SharedLayout);
    // The reflector orders variants by name. Public indices follow the compile descriptor.
    for (const auto& variant : variants) {
        const auto found = std::find_if(layout.Value->Variants.begin(), layout.Value->Variants.end(),
                                        [&](const auto& item) { return item.Name == variant.Name; });
        if (found == layout.Value->Variants.end()) return failure("missing reflected variant");
        asset.Family.Variants.push_back(std::move(*found));
    }
    auto value = Create(device, asset);
    if (!value) return failure("GPU shader creation failed");
    return {std::move(value), std::move(diagnostics)};
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

AssetReloadDecision ShaderAsset::ClassifyReload(const ShaderAsset& candidate) const {
    if (_device != candidate._device) return {AssetReloadKind::Rejected, "shader device changed"};
    const auto& before = GetLayoutDesc();
    const auto& after = candidate.GetLayoutDesc();
    if (before.Groups.size() != after.Groups.size()) return {AssetReloadKind::Breaking, "binding group count changed"};
    for (size_t i = 0; i < before.Groups.size(); ++i)
        if (before.Groups[i] != after.Groups[i]) return {AssetReloadKind::Breaking, radray::format("binding group {} changed", before.Groups[i].GroupSlot)};
    if (before.PushConstants != after.PushConstants) return {AssetReloadKind::Breaking, "push constant layout changed"};
    if (before.Types.size() != after.Types.size()) return {AssetReloadKind::Breaking, "shader type count changed"};
    for (size_t i = 0; i < before.Types.size(); ++i)
        if (before.Types[i] != after.Types[i]) return {AssetReloadKind::Breaking, radray::format("shader type/member {} changed", before.Types[i].Name)};
    if (_family.Variants.size() != candidate._family.Variants.size()) return {AssetReloadKind::Breaking, "variant count changed"};
    for (uint32_t i = 0; i < _family.Variants.size(); ++i) {
        const auto& name = _family.Variants[i].Name;
        if (name != candidate._family.Variants[i].Name) return {AssetReloadKind::Breaking, radray::format("variant {} changed at index {}", name, i)};
        const auto a = GetVariantInterface(i), b = candidate.GetVariantInterface(i);
        if (a.HasValue() != b.HasValue()) return {AssetReloadKind::Breaking, radray::format("{} target availability changed", name)};
        if (!a) continue;
        if (a->Stages != b->Stages) return {AssetReloadKind::Breaking, radray::format("{} stages changed", name)};
        if (a->ActiveBindings != b->ActiveBindings) return {AssetReloadKind::Breaking, radray::format("{} active bindings changed", name)};
        if (a->ActivePushConstants != b->ActivePushConstants) return {AssetReloadKind::Breaking, radray::format("{} active push constants changed", name)};
        for (auto stage : {gpu::ShaderStage::Vertex, gpu::ShaderStage::Pixel, gpu::ShaderStage::Compute}) {
            if (a->Stages.HasFlag(stage) && GetStage(i, stage)->EntryPoint != candidate.GetStage(i, stage)->EntryPoint)
                return {AssetReloadKind::Breaking, radray::format("{} stage entry changed", name)};
        }
    }
    return {};
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

ShaderAsset::ShaderAsset(gpu::Device& device) noexcept : _device(&device) {}

}  // namespace radray
