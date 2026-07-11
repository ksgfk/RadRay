#include <filesystem>
#include <algorithm>
#include <tuple>

#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/file.h>
#include <radray/render/common.h>
#include <radray/runtime/sampler_cache.h>
#include <radray/runtime/shader_variant_library.h>
#include <radray/render/shader/hlsl.h>
#include <radray/render/shader/spirv.h>

#ifdef RADRAY_ENABLE_DXC
#include <radray/render/shader_compiler/dxc.h>
#endif

#ifdef RADRAY_ENABLE_SPIRV_CROSS
#include <radray/render/shader_compiler/spvc.h>
#endif

namespace radray {

using namespace render;

std::optional<render::ShaderBindingLocation> FindShaderBindingLocation(
    const CompiledShaderVariant& variant,
    std::string_view name) noexcept {
    std::optional<render::ShaderBindingLocation> result;
    const render::Shader* shaders[] = {variant.VS, variant.PS, variant.CS};
    for (const render::Shader* shader : shaders) {
        if (shader == nullptr) {
            continue;
        }
        auto reflectionOpt = shader->GetReflection();
        if (!reflectionOpt.HasValue() || reflectionOpt.Get() == nullptr) {
            continue;
        }
        std::optional<render::ShaderBindingLocation> candidate;
        if (const auto* hlsl = std::get_if<render::HlslShaderDesc>(reflectionOpt.Get())) {
            const auto binding = std::ranges::find_if(
                hlsl->BoundResources,
                [name](const render::HlslInputBindDesc& resource) noexcept {
                    return resource.Name == name;
                });
            if (binding != hlsl->BoundResources.end()) {
                candidate = render::ShaderBindingLocation{
                    .Group = binding->VkSet.value_or(binding->Space),
                    .Binding = binding->VkBinding.value_or(binding->BindPoint)};
            }
        } else if (const auto* spirv = std::get_if<render::SpirvShaderDesc>(reflectionOpt.Get())) {
            const auto binding = std::ranges::find_if(
                spirv->ResourceBindings,
                [name](const render::SpirvResourceBinding& resource) noexcept {
                    return resource.Name == name;
                });
            if (binding != spirv->ResourceBindings.end()) {
                candidate = render::ShaderBindingLocation{
                    .Group = binding->Set,
                    .Binding = binding->Binding};
            }
        }
        if (!candidate.has_value()) {
            continue;
        }
        if (result.has_value() && result.value() != candidate.value()) {
            RADRAY_ERR_LOG("shader binding '{}' maps to inconsistent stage locations", name);
            return std::nullopt;
        }
        result = candidate;
    }
    if (!result.has_value()) {
        const auto declared = std::ranges::find_if(
            variant.DeclaredBindings,
            [name](const CompiledShaderVariant::DeclaredBinding& binding) noexcept {
                return binding.Name == name;
            });
        if (declared != variant.DeclaredBindings.end()) {
            result = declared->Location;
        }
    }
    return result;
}

bool ValidateShaderInterfaceSchema(
    const ShaderInterfaceSchema& schema,
    const render::PipelineLayout& layout,
    string* error) noexcept {
    const vector<render::BindingGroupLayout> groups = layout.GetBindingGroupLayouts();
    const vector<render::PushConstantRange> pushConstants = layout.GetPushConstantRanges();
    const auto fail = [error](string message) noexcept {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };
    const auto findBinding = [&groups](uint32_t group, uint32_t binding)
        -> const render::BindingGroupLayoutEntry* {
        for (const render::BindingGroupLayout& groupLayout : groups) {
            if (groupLayout.GroupIndex != group) {
                continue;
            }
            for (const render::BindingGroupLayoutEntry& entry : groupLayout.Entries) {
                if (entry.Binding == binding) {
                    return &entry;
                }
            }
        }
        return nullptr;
    };

    for (const ShaderInterfaceBinding& expected : schema.Bindings) {
        const render::BindingGroupLayoutEntry* actual = findBinding(expected.Group, expected.Binding);
        if (actual == nullptr) {
            if (expected.Required) {
                return fail(fmt::format(
                    "required binding '{}' at group {} binding {} is missing",
                    expected.Name, expected.Group, expected.Binding));
            }
            continue;
        }
        const render::ShaderParameterInfo& parameter = actual->Parameter;
        if ((!expected.Name.empty() && parameter.Name != expected.Name) ||
            parameter.Kind != expected.Kind ||
            (expected.Type != render::ResourceBindType::UNKNOWN && parameter.Type != expected.Type) ||
            parameter.Count != expected.Count ||
            (expected.Stages != render::ShaderStage::UNKNOWN && parameter.Stages != expected.Stages) ||
            actual->HasDynamicOffset != expected.HasDynamicOffset ||
            actual->IsStaticSampler != expected.IsStaticSampler) {
            return fail(fmt::format(
                "binding '{}' at group {} binding {} does not match the declared interface: "
                "expected(name='{}', kind={}, type={}, count={}, stages={}, dynamic={}, static={}), "
                "actual(name='{}', kind={}, type={}, count={}, stages={}, dynamic={}, static={})",
                expected.Name,
                expected.Group,
                expected.Binding,
                expected.Name,
                static_cast<uint32_t>(expected.Kind),
                static_cast<uint32_t>(expected.Type),
                expected.Count,
                expected.Stages.value(),
                expected.HasDynamicOffset,
                expected.IsStaticSampler,
                parameter.Name,
                static_cast<uint32_t>(parameter.Kind),
                static_cast<uint32_t>(parameter.Type),
                parameter.Count,
                parameter.Stages.value(),
                actual->HasDynamicOffset,
                actual->IsStaticSampler));
        }
    }
    if (!schema.AllowAdditionalBindings) {
        for (const render::BindingGroupLayout& group : groups) {
            for (const render::BindingGroupLayoutEntry& actual : group.Entries) {
                const bool declared = std::ranges::any_of(
                    schema.Bindings,
                    [&group, &actual](const ShaderInterfaceBinding& expected) noexcept {
                        return expected.Group == group.GroupIndex && expected.Binding == actual.Binding;
                    });
                if (!declared) {
                    return fail(fmt::format(
                        "undeclared binding '{}' at group {} binding {}",
                        actual.Parameter.Name, group.GroupIndex, actual.Binding));
                }
            }
        }
    }

    for (const ShaderInterfacePushConstant& expected : schema.PushConstants) {
        const auto actual = std::ranges::find_if(
            pushConstants,
            [&expected](const render::PushConstantRange& range) noexcept {
                return range.Group == expected.Group && range.Binding == expected.Binding;
            });
        if (actual == pushConstants.end()) {
            if (expected.Required) {
                return fail(fmt::format("required push constant '{}' is missing", expected.Name));
            }
            continue;
        }
        if ((!expected.Name.empty() && actual->Name != expected.Name) ||
            actual->Stages != expected.Stages || actual->Offset != expected.Offset ||
            actual->Size != expected.Size) {
            return fail(fmt::format("push constant '{}' does not match the declared interface", expected.Name));
        }
    }
    if (!schema.AllowAdditionalBindings) {
        for (const render::PushConstantRange& actual : pushConstants) {
            const bool declared = std::ranges::any_of(
                schema.PushConstants,
                [&actual](const ShaderInterfacePushConstant& expected) noexcept {
                    return expected.Group == actual.Group && expected.Binding == actual.Binding;
                });
            if (!declared) {
                return fail(fmt::format("undeclared push constant '{}'", actual.Name));
            }
        }
    }
    return true;
}

std::optional<ShaderVariantKey> BuildShaderVariantKey(
    const ShaderVariantDescriptor& desc,
    render::RenderBackend backend) noexcept {
    if (desc.ProgramId.IsEmpty()) {
        RADRAY_ERR_LOG("shader variant cache: ProgramId is empty (assign an identity before caching)");
        return std::nullopt;
    }
    ShaderVariantKey key{};
    key.ProgramId = desc.ProgramId;
    key.PassIndex = desc.PassIndex;
    key.Backend = static_cast<uint32_t>(backend);
    key.KeywordBitmask = desc.KeywordBitmask;
    key.ShaderModel = static_cast<uint32_t>(desc.SM);
    key.CompileOptions = desc.IsOptimize ? 1u : 0u;
    key.SourceVersion = desc.SourceVersion;
    if (key.SourceVersion == 0) {
        uint64_t version = 0xcbf29ce484222325ull;
        auto append = [&version](const void* data, size_t size) noexcept {
            const uint64_t part = HashData64(data, size);
            version ^= part + 0x9e3779b97f4a7c15ull + (version << 6u) + (version >> 2u);
        };
        for (const auto& stage : desc.Stages) {
            append(stage.Source.data(), stage.Source.size());
            append(stage.EntryPoint.data(), stage.EntryPoint.size());
            append(&stage.Stage, sizeof(stage.Stage));
        }
        for (const auto define : desc.Defines) {
            append(define.data(), define.size());
        }
        for (const auto include : desc.Includes) {
            append(include.data(), include.size());
        }
        vector<render::DynamicBufferBinding> dynamicBindings{
            desc.DynamicBufferBindings.begin(), desc.DynamicBufferBindings.end()};
        std::ranges::sort(dynamicBindings, [](const auto& lhs, const auto& rhs) noexcept {
            return std::tie(lhs.Group, lhs.Binding) < std::tie(rhs.Group, rhs.Binding);
        });
        for (const render::DynamicBufferBinding& binding : dynamicBindings) {
            append(&binding.Group, sizeof(binding.Group));
            append(&binding.Binding, sizeof(binding.Binding));
        }
        vector<render::PushConstantBinding> pushConstantBindings{
            desc.PushConstantBindings.begin(), desc.PushConstantBindings.end()};
        std::ranges::sort(pushConstantBindings, [](const auto& lhs, const auto& rhs) noexcept {
            return std::tie(lhs.Group, lhs.Binding) < std::tie(rhs.Group, rhs.Binding);
        });
        for (const render::PushConstantBinding& binding : pushConstantBindings) {
            append(&binding.Group, sizeof(binding.Group));
            append(&binding.Binding, sizeof(binding.Binding));
        }
        vector<const render::StaticSamplerDescriptor*> staticSamplers;
        staticSamplers.reserve(desc.StaticSamplers.size());
        for (const render::StaticSamplerDescriptor& sampler : desc.StaticSamplers) {
            staticSamplers.push_back(&sampler);
        }
        std::ranges::sort(staticSamplers, [](const auto* lhs, const auto* rhs) noexcept {
            return lhs->Name < rhs->Name;
        });
        for (const render::StaticSamplerDescriptor* sampler : staticSamplers) {
            append(sampler->Name.data(), sampler->Name.size());
            const SamplerKey samplerKey = BuildSamplerKey(sampler->Desc);
            append(&samplerKey, sizeof(samplerKey));
        }
        if (desc.InterfaceSchema != nullptr) {
            for (const ShaderInterfaceBinding& binding : desc.InterfaceSchema->Bindings) {
                append(binding.Name.data(), binding.Name.size());
                append(&binding.Group, sizeof(binding.Group));
                append(&binding.Binding, sizeof(binding.Binding));
                append(&binding.Kind, sizeof(binding.Kind));
                append(&binding.Type, sizeof(binding.Type));
                append(&binding.Count, sizeof(binding.Count));
                append(&binding.Stages, sizeof(binding.Stages));
                append(&binding.HasDynamicOffset, sizeof(binding.HasDynamicOffset));
                append(&binding.IsStaticSampler, sizeof(binding.IsStaticSampler));
                append(&binding.Required, sizeof(binding.Required));
            }
            for (const ShaderInterfacePushConstant& range : desc.InterfaceSchema->PushConstants) {
                append(range.Name.data(), range.Name.size());
                append(&range.Group, sizeof(range.Group));
                append(&range.Binding, sizeof(range.Binding));
                append(&range.Stages, sizeof(range.Stages));
                append(&range.Offset, sizeof(range.Offset));
                append(&range.Size, sizeof(range.Size));
                append(&range.Required, sizeof(range.Required));
            }
            append(
                &desc.InterfaceSchema->AllowAdditionalBindings,
                sizeof(desc.InterfaceSchema->AllowAdditionalBindings));
        }
        key.SourceVersion = version;
    }
    return key;
}

PipelineLayoutLibrary::PipelineLayoutLibrary(render::Device* device) noexcept
    : _device(device) {}

PipelineLayoutLibrary::~PipelineLayoutLibrary() noexcept {
    Clear();
}

string PipelineLayoutLibrary::BuildCanonicalKey(
    render::PipelineLayout& layout,
    const render::PipelineLayoutDescriptor& desc) noexcept {
    vector<string> entries;
    for (const render::BindingGroupLayout& group : layout.GetBindingGroupLayouts()) {
        for (const render::BindingGroupLayoutEntry& binding : group.Entries) {
            const render::ShaderParameterInfo& parameter = binding.Parameter;
            entries.push_back(fmt::format(
                "b:{:08x}:{:08x}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
                group.GroupIndex,
                binding.Binding,
                static_cast<uint32_t>(parameter.Kind),
                static_cast<uint32_t>(parameter.Type),
                parameter.Count,
                parameter.ByteSize,
                parameter.Stages.value(),
                parameter.IsReadOnly ? 1u : 0u,
                parameter.IsBindless ? 1u : 0u,
                binding.HasDynamicOffset ? 1u : 0u,
                binding.IsStaticSampler ? 1u : 0u));

            if (!binding.IsStaticSampler) {
                continue;
            }
            const auto samplerIt = std::ranges::find_if(
                desc.StaticSamplers,
                [&parameter](const render::StaticSamplerDescriptor& sampler) noexcept {
                    return sampler.Name == parameter.Name;
                });
            if (samplerIt == desc.StaticSamplers.end()) {
                continue;
            }
            const auto& value = samplerIt->Desc;
            entries.push_back(fmt::format(
                "s:{:08x}:{:08x}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
                group.GroupIndex,
                binding.Binding,
                static_cast<uint32_t>(value.AddressS),
                static_cast<uint32_t>(value.AddressT),
                static_cast<uint32_t>(value.AddressR),
                static_cast<uint32_t>(value.MinFilter),
                static_cast<uint32_t>(value.MagFilter),
                static_cast<uint32_t>(value.MipmapFilter),
                value.LodMin,
                value.LodMax,
                value.Compare.has_value() ? static_cast<uint32_t>(value.Compare.value()) + 1u : 0u,
                value.AnisotropyClamp));
        }
    }
    for (const render::PushConstantRange& range : layout.GetPushConstantRanges()) {
        entries.push_back(fmt::format(
            "p:{:08x}:{:08x}:{}:{}:{}",
            range.Group,
            range.Binding,
            range.Stages.value(),
            range.Offset,
            range.Size));
    }
    std::ranges::sort(entries);
    string key;
    for (const auto& entry : entries) {
        key.append(entry);
        key.push_back(';');
    }
    return key;
}

std::optional<string> PipelineLayoutLibrary::BuildBindingGroupCanonicalKey(
    const render::BindingGroupLayout& group,
    const render::PipelineLayoutDescriptor& desc) noexcept {
    vector<string> entries;
    entries.reserve(group.Entries.size() * 2);
    for (const render::BindingGroupLayoutEntry& binding : group.Entries) {
        const render::ShaderParameterInfo& parameter = binding.Parameter;
        const bool hasDynamicOffset = binding.HasDynamicOffset || std::ranges::any_of(
            desc.DynamicBufferBindings,
            [&group, &binding](const render::DynamicBufferBinding& dynamicBinding) noexcept {
                return dynamicBinding.Group == group.GroupIndex &&
                       dynamicBinding.Binding == binding.Binding;
            });
        entries.push_back(fmt::format(
            "b:{:08x}:{:08x}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
            group.GroupIndex,
            binding.Binding,
            static_cast<uint32_t>(parameter.Kind),
            static_cast<uint32_t>(parameter.Type),
            parameter.Count,
            parameter.ByteSize,
            parameter.Stages.value(),
            parameter.IsReadOnly ? 1u : 0u,
            parameter.IsBindless ? 1u : 0u,
            hasDynamicOffset ? 1u : 0u,
            binding.IsStaticSampler ? 1u : 0u));

        if (!binding.IsStaticSampler) {
            continue;
        }
        const auto samplerIt = std::ranges::find_if(
            desc.StaticSamplers,
            [&parameter](const render::StaticSamplerDescriptor& sampler) noexcept {
                return sampler.Name == parameter.Name;
            });
        if (samplerIt == desc.StaticSamplers.end()) {
            return std::nullopt;
        }
        const auto& value = samplerIt->Desc;
        entries.push_back(fmt::format(
            "s:{:08x}:{:08x}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
            group.GroupIndex,
            binding.Binding,
            static_cast<uint32_t>(value.AddressS),
            static_cast<uint32_t>(value.AddressT),
            static_cast<uint32_t>(value.AddressR),
            static_cast<uint32_t>(value.MinFilter),
            static_cast<uint32_t>(value.MagFilter),
            static_cast<uint32_t>(value.MipmapFilter),
            value.LodMin,
            value.LodMax,
            value.Compare.has_value() ? static_cast<uint32_t>(value.Compare.value()) + 1u : 0u,
            value.AnisotropyClamp));
    }
    std::ranges::sort(entries);
    string key;
    for (const string& entry : entries) {
        key.append(entry);
        key.push_back(';');
    }
    return key;
}

Nullable<render::PipelineLayout*> PipelineLayoutLibrary::GetOrCreate(
    const render::PipelineLayoutDescriptor& desc,
    const ShaderInterfaceSchema* schema) noexcept {
    if (_device == nullptr || desc.Shaders.empty()) {
        return nullptr;
    }

    vector<render::BindingGroupLayoutReuse> layoutReuses{
        desc.BindingGroupLayoutReuses.begin(), desc.BindingGroupLayoutReuses.end()};
    if (schema != nullptr && !schema->AllowAdditionalBindings) {
        for (const render::BindingGroupLayout& group : desc.BindingGroupLayouts) {
            const bool alreadyDeclared = std::ranges::any_of(
                layoutReuses,
                [&group](const render::BindingGroupLayoutReuse& reuse) noexcept {
                    return reuse.Group == group.GroupIndex;
                });
            if (alreadyDeclared) {
                continue;
            }
            auto groupKey = BuildBindingGroupCanonicalKey(group, desc);
            if (!groupKey.has_value()) {
                continue;
            }
            const auto source = std::ranges::find_if(
                _bindingGroupLayouts,
                [&groupKey](const BindingGroupLayoutEntry& entry) noexcept {
                    return entry.Key == groupKey.value();
                });
            if (source != _bindingGroupLayouts.end()) {
                layoutReuses.push_back(render::BindingGroupLayoutReuse{
                    .Group = group.GroupIndex,
                    .Source = source->Layout,
                    .SourceGroup = source->Group});
            }
        }
    }

    render::PipelineLayoutDescriptor createDesc = desc;
    createDesc.BindingGroupLayoutReuses = layoutReuses;
    auto candidateOpt = _device->CreatePipelineLayout(createDesc);
    if (!candidateOpt.HasValue()) {
        return nullptr;
    }
    auto candidate = candidateOpt.Release();
    if (schema != nullptr) {
        string error;
        if (!ValidateShaderInterfaceSchema(*schema, *candidate, &error)) {
            RADRAY_ERR_LOG("shader interface validation failed: {}", error);
            candidate->Destroy();
            return nullptr;
        }
    }
    string key = BuildCanonicalKey(*candidate, desc);
    for (auto& entry : _entries) {
        if (entry.Key == key) {
            ++_hits;
            candidate->Destroy();
            return entry.Layout.get();
        }
    }
    ++_misses;
    candidate->SetGuid(Guid::NewGuid());
    auto* result = candidate.get();
    _entries.push_back(Entry{.Key = std::move(key), .Layout = std::move(candidate)});
    if (schema != nullptr && !schema->AllowAdditionalBindings) {
        for (const render::BindingGroupLayout& group : result->GetBindingGroupLayouts()) {
            auto groupKey = BuildBindingGroupCanonicalKey(group, desc);
            if (!groupKey.has_value()) {
                continue;
            }
            const bool exists = std::ranges::any_of(
                _bindingGroupLayouts,
                [&groupKey](const BindingGroupLayoutEntry& entry) noexcept {
                    return entry.Key == groupKey.value();
                });
            if (!exists) {
                _bindingGroupLayouts.push_back(BindingGroupLayoutEntry{
                    .Key = std::move(groupKey.value()),
                    .Layout = result,
                    .Group = group.GroupIndex});
            }
        }
    }
    return result;
}

void PipelineLayoutLibrary::Clear() noexcept {
    _bindingGroupLayouts.clear();
    for (auto entry = _entries.rbegin(); entry != _entries.rend(); ++entry) {
        if (entry->Layout != nullptr) {
            entry->Layout->Destroy();
        }
    }
    _entries.clear();
}

namespace {

// 变体来源策略: 封装"如何得到一个 stage 的字节码 Shader"这一唯一差异。
//
// 变体库机制 (按 key 去重、reflection -> CreateShader、走 PipelineLayoutLibrary 得 layout)
// 由统一实现承担, 与来源无关。来源只负责产出单个 stage 的 Shader:
//   - DXC JIT:      运行时用 Dxc 编译 HLSL 源 + 反射 (DxcVariantSource)
//   - 预编译磁盘:   从烘焙产物读字节码 + 反序列化反射 (PrecompiledVariantSource)
//
struct ShaderStageArtifact {
    vector<byte> Bytecode;
    ShaderBlobCategory Category{ShaderBlobCategory::DXIL};
    ShaderReflectionDesc Reflection;
};

class ShaderVariantSource {
public:
    virtual ~ShaderVariantSource() noexcept = default;

    virtual std::optional<ShaderStageArtifact> CompileStage(
        const ShaderVariantStageDesc& stage,
        const ShaderVariantDescriptor& desc) noexcept = 0;
};

// 后端无关、来源无关的统一变体缓存机制。
// miss 时逐 stage 向 ShaderVariantSource 索取 Shader -> 走 PipelineLayoutLibrary 得 layout。
class ShaderVariantLibraryImpl final : public ShaderVariantLibrary {
public:
    ShaderVariantLibraryImpl(
        Device* device,
        PipelineLayoutLibrary* layoutLibrary,
        unique_ptr<ShaderVariantSource> source) noexcept
        : _device(device), _layoutLibrary(layoutLibrary), _source(std::move(source)) {}
    ~ShaderVariantLibraryImpl() noexcept override { Clear(); }

    Nullable<const CompiledShaderVariant*> Find(const ShaderVariantKey& key) const noexcept override {
        if (auto it = _cache.find(key); it != _cache.end()) {
            return &it->second.Variant;
        }
        return nullptr;
    }

    Nullable<const CompiledShaderVariant*> GetOrCreate(const ShaderVariantDescriptor& desc) noexcept override {
        auto keyOpt = BuildShaderVariantKey(desc, _device->GetBackend());
        if (!keyOpt.has_value()) {
            return nullptr;
        }
        const ShaderVariantKey& key = keyOpt.value();
        if (auto cached = Find(key); cached.HasValue()) {
            ++_stats.VariantHits;
            return cached;
        }
        ++_stats.VariantMisses;
        if (desc.Stages.empty()) {
            RADRAY_ERR_LOG("shader variant cache: descriptor has no stages");
            return nullptr;
        }

        Entry entry{};
        vector<Shader*> shaders;
        shaders.reserve(desc.Stages.size());

        for (const auto& stage : desc.Stages) {
            auto artifactOpt = _source->CompileStage(stage, desc);
            if (!artifactOpt.has_value()) {
                return nullptr;
            }
            auto artifact = std::move(artifactOpt.value());
            ModuleKey moduleKey{};
            moduleKey.Backend = static_cast<uint32_t>(_device->GetBackend());
            moduleKey.Stage = static_cast<uint32_t>(stage.Stage);
            moduleKey.BytecodeHash = HashData64(artifact.Bytecode.data(), artifact.Bytecode.size());
            Shader* raw = nullptr;
            if (auto moduleIt = _modules.find(moduleKey); moduleIt != _modules.end() &&
                moduleIt->second.Bytecode == artifact.Bytecode) {
                ++_stats.ModuleHits;
                raw = moduleIt->second.Object.get();
            } else {
                ++_stats.ModuleMisses;
                ShaderDescriptor shaderDesc{};
                shaderDesc.Source = artifact.Bytecode;
                shaderDesc.Category = artifact.Category;
                shaderDesc.Stages = stage.Stage;
                shaderDesc.Reflection = std::move(artifact.Reflection);
                auto shaderOpt = _device->CreateShader(shaderDesc);
                if (!shaderOpt.HasValue()) {
                    return nullptr;
                }
                auto shader = shaderOpt.Release();
                shader->SetGuid(Guid::NewGuid());
                raw = shader.get();
                _modules.emplace(
                    moduleKey,
                    ModuleEntry{.Bytecode = std::move(artifact.Bytecode), .Object = std::move(shader)});
            }
            switch (stage.Stage) {
                case ShaderStage::Vertex: entry.Variant.VS = raw; break;
                case ShaderStage::Pixel: entry.Variant.PS = raw; break;
                case ShaderStage::Compute: entry.Variant.CS = raw; break;
                default:
                    RADRAY_ERR_LOG("shader variant cache: unsupported stage {}", stage.Stage);
                    return nullptr;
            }
            shaders.push_back(raw);
        }

        PipelineLayoutDescriptor layoutDesc{};
        layoutDesc.Shaders = shaders;
        layoutDesc.StaticSamplers = desc.StaticSamplers;
        layoutDesc.DynamicBufferBindings = desc.DynamicBufferBindings;
        vector<BindingGroupLayout> explicitGroups;
        if (desc.InterfaceSchema != nullptr) {
            for (const ShaderInterfaceBinding& binding : desc.InterfaceSchema->Bindings) {
                const auto reflectedLocation = FindShaderBindingLocation(entry.Variant, binding.Name);
                const bool reflectedAtDeclaredLocation = reflectedLocation.has_value() &&
                                                         reflectedLocation->Group == binding.Group &&
                                                         reflectedLocation->Binding == binding.Binding;
                if (!binding.Required && !reflectedAtDeclaredLocation) {
                    continue;
                }
                auto group = std::ranges::find_if(
                    explicitGroups,
                    [&binding](const BindingGroupLayout& candidate) noexcept {
                        return candidate.GroupIndex == binding.Group;
                    });
                if (group == explicitGroups.end()) {
                    explicitGroups.push_back(BindingGroupLayout{.GroupIndex = binding.Group});
                    group = std::prev(explicitGroups.end());
                }
                const bool readOnly = binding.Type != ResourceBindType::RWBuffer &&
                                      binding.Type != ResourceBindType::RWTexelBuffer &&
                                      binding.Type != ResourceBindType::RWTexture;
                group->Entries.push_back(BindingGroupLayoutEntry{
                    .Parameter = ShaderParameterInfo{
                        .Name = binding.Name,
                        .Kind = binding.Kind,
                        .Stages = binding.Stages,
                        .Type = binding.Type,
                        .Count = binding.Count,
                        .IsReadOnly = readOnly,
                        .IsBindless = binding.Kind == ShaderParameterKind::BindlessArray},
                    .Binding = binding.Binding,
                    .HasDynamicOffset = binding.HasDynamicOffset,
                    .IsStaticSampler = binding.IsStaticSampler});
                entry.Variant.DeclaredBindings.push_back(CompiledShaderVariant::DeclaredBinding{
                    .Name = binding.Name,
                    .Location = ShaderBindingLocation{
                        .Group = binding.Group,
                        .Binding = binding.Binding}});
            }
            std::ranges::sort(
                explicitGroups,
                [](const BindingGroupLayout& lhs, const BindingGroupLayout& rhs) noexcept {
                    return lhs.GroupIndex < rhs.GroupIndex;
                });
            for (BindingGroupLayout& group : explicitGroups) {
                std::ranges::sort(
                    group.Entries,
                    [](const BindingGroupLayoutEntry& lhs, const BindingGroupLayoutEntry& rhs) noexcept {
                        return lhs.Binding < rhs.Binding;
                    });
            }
        }
        layoutDesc.BindingGroupLayouts = explicitGroups;
        layoutDesc.PushConstantBindings = desc.PushConstantBindings;
        const uint64_t oldLayoutHits = _layoutLibrary->GetHitCount();
        const uint64_t oldLayoutMisses = _layoutLibrary->GetMissCount();
        auto layoutOpt = _layoutLibrary->GetOrCreate(layoutDesc, desc.InterfaceSchema);
        if (!layoutOpt.HasValue()) {
            RADRAY_ERR_LOG("shader variant cache: GetOrCreate binding layout failed");
            return nullptr;
        }
        entry.Variant.Layout = layoutOpt.Get();
        entry.Variant.Key = key;
        _stats.LayoutHits += _layoutLibrary->GetHitCount() - oldLayoutHits;
        _stats.LayoutMisses += _layoutLibrary->GetMissCount() - oldLayoutMisses;

        auto [it, _] = _cache.emplace(key, std::move(entry));
        return &it->second.Variant;
    }

    void Clear() noexcept override {
        _cache.clear();
        for (auto& [_, module] : _modules) {
            if (module.Object != nullptr) {
                module.Object->Destroy();
            }
        }
        _modules.clear();
    }

    uint32_t Count() const noexcept override { return static_cast<uint32_t>(_cache.size()); }

    ShaderVariantLibraryStats GetStats() const noexcept override { return _stats; }

private:
    struct Entry {
        CompiledShaderVariant Variant{};
    };

    struct ModuleKey {
        uint64_t BytecodeHash{0};
        uint32_t Backend{0};
        uint32_t Stage{0};
    };
    static_assert(std::is_trivially_copyable_v<ModuleKey>);

    struct ModuleEntry {
        vector<byte> Bytecode;
        unique_ptr<Shader> Object;
    };

    Device* _device;
    PipelineLayoutLibrary* _layoutLibrary;
    unique_ptr<ShaderVariantSource> _source;
    unordered_map<ShaderVariantKey, Entry, PodHasher<ShaderVariantKey>, PodEqual<ShaderVariantKey>> _cache;
    unordered_map<ModuleKey, ModuleEntry, PodHasher<ModuleKey>, PodEqual<ModuleKey>> _modules;
    ShaderVariantLibraryStats _stats{};
};

// 用给定来源组装统一缓存。source 的所有权转移给缓存。
Nullable<unique_ptr<ShaderVariantLibrary>> MakeShaderVariantLibrary(
    Device* device,
    PipelineLayoutLibrary* layoutLibrary,
    unique_ptr<ShaderVariantSource> source) noexcept {
    if (device == nullptr || layoutLibrary == nullptr || source == nullptr) {
        RADRAY_ERR_LOG("MakeShaderVariantLibrary: device, layout library or source is null");
        return nullptr;
    }
    return unique_ptr<ShaderVariantLibrary>{
        make_unique<ShaderVariantLibraryImpl>(device, layoutLibrary, std::move(source)).release()};
}

// 预编译磁盘来源: 从烘焙产物读字节码 + 反序列化反射, 不依赖 DXC。
//
// 磁盘布局:
//   <bakeRoot>/<LogicalName>/pass<N>_mask<HEX16>/<target>/{vs,ps,cs}.{dxil|spv}
//                                                <target>/{vs,ps,cs}.refl.json
// target: D3D12 -> "dxil", Vulkan -> "spirv"。
class PrecompiledVariantSource final : public ShaderVariantSource {
public:
    PrecompiledVariantSource(Device* device, std::string_view bakeRoot) noexcept
        : _device(device), _bakeRoot(bakeRoot) {
        const RenderBackend backend = _device->GetBackend();
        if (backend == RenderBackend::Vulkan) {
            _target = "spirv";
            _blobExt = ".spv";
            _category = ShaderBlobCategory::SPIRV;
            _isSpirv = true;
        } else {
            _target = "dxil";
            _blobExt = ".dxil";
            _category = ShaderBlobCategory::DXIL;
            _isSpirv = false;
        }
    }

    std::optional<ShaderStageArtifact> CompileStage(
        const ShaderVariantStageDesc& stage,
        const ShaderVariantDescriptor& desc) noexcept override {
        if (desc.LogicalName.empty()) {
            RADRAY_ERR_LOG("precompiled shader cache: descriptor has empty LogicalName; cannot locate baked variant");
            return std::nullopt;
        }
        const char* prefix = StagePrefix(stage.Stage);
        if (prefix == nullptr) {
            RADRAY_ERR_LOG("precompiled shader cache: unsupported stage {}", stage.Stage);
            return std::nullopt;
        }

        const std::filesystem::path variantDir = MakeVariantDir(desc);
        const std::filesystem::path blobPath = variantDir / (string{prefix} + _blobExt);
        const std::filesystem::path reflPath = variantDir / (string{prefix} + ".refl.json");

        auto blobOpt = ReadBinaryFile(blobPath);
        if (!blobOpt.has_value()) {
            RADRAY_ERR_LOG("precompiled shader cache: cannot read bytecode '{}' (variant not baked for target '{}'?)",
                           blobPath.string(), _target);
            return std::nullopt;
        }
        auto reflJson = ReadTextFile(reflPath);
        if (!reflJson.has_value()) {
            RADRAY_ERR_LOG("precompiled shader cache: cannot read reflection '{}'", reflPath.string());
            return std::nullopt;
        }

        ShaderReflectionDesc reflection{};
        if (_isSpirv) {
            auto reflOpt = DeserializeSpirvShaderDesc(reflJson.value());
            if (!reflOpt.has_value()) {
                RADRAY_ERR_LOG("precompiled shader cache: SPIR-V reflection deserialize failed '{}'", reflPath.string());
                return std::nullopt;
            }
            reflection = std::move(reflOpt.value());
        } else {
            auto reflOpt = DeserializeHlslShaderDesc(reflJson.value());
            if (!reflOpt.has_value()) {
                RADRAY_ERR_LOG("precompiled shader cache: DXIL reflection deserialize failed '{}'", reflPath.string());
                return std::nullopt;
            }
            reflection = std::move(reflOpt.value());
        }

        return ShaderStageArtifact{
            .Bytecode = std::move(blobOpt.value()),
            .Category = _category,
            .Reflection = std::move(reflection)};
    }

private:
    // stage -> 文件名前缀 (vs / ps / cs)。
    static const char* StagePrefix(ShaderStage stage) noexcept {
        switch (stage) {
            case ShaderStage::Vertex: return "vs";
            case ShaderStage::Pixel: return "ps";
            case ShaderStage::Compute: return "cs";
            default: return nullptr;
        }
    }

    std::filesystem::path MakeVariantDir(const ShaderVariantDescriptor& desc) const {
        // pass<N>_mask<16位大写HEX>
        char maskBuf[32];
        std::snprintf(maskBuf, sizeof(maskBuf), "pass%u_mask%016llX",
                      desc.PassIndex, static_cast<unsigned long long>(desc.KeywordBitmask));
        std::filesystem::path dir{_bakeRoot};
        dir /= std::filesystem::path{string{desc.LogicalName}};
        dir /= maskBuf;
        dir /= _target;  // <target> 子目录
        return dir;
    }

    Device* _device;
    string _bakeRoot;
    string _target;
    string _blobExt;
    ShaderBlobCategory _category{ShaderBlobCategory::DXIL};
    bool _isSpirv{false};
};

#ifdef RADRAY_ENABLE_DXC

// DXC JIT 来源: 运行时用 Dxc 编译 HLSL 源 (Vulkan 走 SPIR-V) + 反射, 产出单个 stage 的 Shader。
class DxcVariantSource final : public ShaderVariantSource {
public:
    DxcVariantSource(Device* device, Dxc* dxc) noexcept
        : _device(device), _dxc(dxc) {}

    std::optional<ShaderStageArtifact> CompileStage(
        const ShaderVariantStageDesc& stage,
        const ShaderVariantDescriptor& desc) noexcept override {
        const bool isSpirv = _device->GetBackend() == RenderBackend::Vulkan;

        DxcCompileParams params{};
        params.Code = stage.Source;
        params.EntryPoint = stage.EntryPoint;
        params.Stage = stage.Stage;
        params.SM = desc.SM;
        params.Defines = desc.Defines;
        params.Includes = desc.Includes;
        params.IsOptimize = desc.IsOptimize;
        params.IsSpirv = isSpirv;
        params.EnableUnbounded = false;
        auto outputOpt = _dxc->Compile(params);
        if (!outputOpt.has_value()) {
            RADRAY_ERR_LOG("shader variant cache: DXC compile failed for entry '{}'", stage.EntryPoint);
            return std::nullopt;
        }
        auto output = std::move(outputOpt.value());

        ShaderReflectionDesc reflection{};
        ShaderBlobCategory category = ShaderBlobCategory::DXIL;
        if (isSpirv) {
#ifdef RADRAY_ENABLE_SPIRV_CROSS
            auto reflOpt = ReflectSpirv(SpirvBytecodeView{
                .Data = output.Data,
                .EntryPointName = stage.EntryPoint,
                .Stage = stage.Stage,
            });
            if (!reflOpt.has_value()) {
                RADRAY_ERR_LOG("shader variant cache: SPIR-V reflection failed for entry '{}'", stage.EntryPoint);
                return std::nullopt;
            }
            reflection = std::move(reflOpt.value());
            category = ShaderBlobCategory::SPIRV;
#else
            RADRAY_ERR_LOG("shader variant cache: SPIR-V Cross is not enabled for this build");
            return std::nullopt;
#endif
        } else {
            auto reflOpt = _dxc->GetShaderDescFromOutput(output.Refl);
            if (!reflOpt.has_value()) {
                RADRAY_ERR_LOG("shader variant cache: DXIL reflection failed for entry '{}'", stage.EntryPoint);
                return std::nullopt;
            }
            reflection = std::move(reflOpt.value());
            category = ShaderBlobCategory::DXIL;
        }

        return ShaderStageArtifact{
            .Bytecode = std::move(output.Data),
            .Category = category,
            .Reflection = std::move(reflection)};
    }

private:
    Device* _device;
    Dxc* _dxc;
};

#endif

}  // namespace

// 预编译 shader 变体缓存 (不依赖 DXC), 从磁盘烘焙产物加载。见头文件说明。
Nullable<unique_ptr<ShaderVariantLibrary>> CreatePrecompiledShaderVariantLibrary(
    Device* device,
    PipelineLayoutLibrary* layoutLibrary,
    std::string_view bakeRoot) noexcept {
    if (device == nullptr || layoutLibrary == nullptr) {
        RADRAY_ERR_LOG("CreatePrecompiledShaderVariantLibrary: device or layout library is null");
        return nullptr;
    }
    if (bakeRoot.empty()) {
        RADRAY_ERR_LOG("CreatePrecompiledShaderVariantLibrary: bakeRoot is empty");
        return nullptr;
    }
    return MakeShaderVariantLibrary(
        device, layoutLibrary,
        make_unique<PrecompiledVariantSource>(device, bakeRoot));
}

#ifdef RADRAY_ENABLE_DXC

// DXC JIT shader 变体缓存, 运行时编译。见头文件说明。
Nullable<unique_ptr<ShaderVariantLibrary>> CreateShaderVariantLibrary(
    Device* device,
    Dxc* dxc,
    PipelineLayoutLibrary* layoutLibrary) noexcept {
    if (device == nullptr || dxc == nullptr || layoutLibrary == nullptr) {
        RADRAY_ERR_LOG("CreateShaderVariantLibrary: device, dxc or layout library is null");
        return nullptr;
    }
    return MakeShaderVariantLibrary(device, layoutLibrary, make_unique<DxcVariantSource>(device, dxc));
}

#endif

}  // namespace radray
