#include <radray/shader/shader_reflector.h>

#include "record_validation.h"
#include <radray/shader/shader_compiler_contract.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <radray/binary_io.h>
#include <radray/logger.h>

namespace radray::shader {
namespace {

shader::ShaderTypeRecord ConvertType(const shader::ShaderArtifactView& artifact, const shader::ShaderArtifactTypeRecord& source) {
    shader::ShaderTypeRecord result;
    result.Name = *artifact.GetName(source.Name);
    result.Kind = static_cast<shader::ShaderTypeKind>(source.Kind);
    result.ElementKind = static_cast<shader::ShaderTypeKind>(source.ElementKind);
    result.ElementCount = source.ElementCount;
    result.Offset = source.Offset;
    result.Size = source.Size;
    result.ElementSize = source.ElementSize;
    result.Stride = source.Stride;
    result.MatrixStride = source.MatrixStride;
    result.ScalarKind = static_cast<shader::ShaderScalarKind>(source.ScalarKind);
    result.RowCount = source.RowCount;
    result.ColumnCount = source.ColumnCount;
    result.RowMajor = (source.Flags & shader::kShaderTypeFlagRowMajor) != 0;
    return result;
}

void TypeKey(BinaryWriter& writer, const shader::ShaderTypeRecord& type, bool member) {
    if (member) writer.String(type.Name);
    writer.U32(static_cast<uint32_t>(type.Kind));
    writer.U32(static_cast<uint32_t>(type.ElementKind));
    writer.U32(type.ElementCount);
    writer.U32(type.Offset);
    writer.U32(type.Size);
    writer.U32(type.ElementSize);
    writer.U32(type.Stride);
    writer.U32(type.MatrixStride);
    writer.U32(static_cast<uint32_t>(type.ScalarKind));
    writer.U32(type.RowCount);
    writer.U32(type.ColumnCount);
    writer.Bool(type.RowMajor);
    writer.Bool(type.TypeIndex.has_value());
    if (type.TypeIndex) writer.U32(*type.TypeIndex);
}

class TypeInterner {
public:
    explicit TypeInterner(vector<shader::ShaderTypeRecord>& types) : _types(types) {}

    vector<std::optional<uint32_t>> Import(const shader::ShaderArtifactView& artifact) {
        const auto source = artifact.Types();
        vector<std::optional<uint32_t>> remap(source.size());
        vector<bool> reachable(source.size(), false);
        vector<uint32_t> pending;
        for (const auto& b : artifact.Bindings())
            if (b.TypeIndex != shader::kShaderNoType) pending.push_back(b.TypeIndex);
        for (const auto& p : artifact.RootConstants())
            if (p.TypeIndex != shader::kShaderNoType) pending.push_back(p.TypeIndex);
        while (!pending.empty()) {
            const auto root = pending.back();
            pending.pop_back();
            if (reachable[root]) continue;
            reachable[root] = true;
            for (const auto& child : source)
                if (child.ParentIndex == root && child.TypeIndex != shader::kShaderNoType) pending.push_back(child.TypeIndex);
        }
        struct Candidate {
            uint32_t Source;
            vector<shader::ShaderTypeRecord> Records;
            vector<byte> Key;
        };
        for (;;) {
            vector<Candidate> ready;
            for (uint32_t index = 0; index < source.size(); ++index) {
                if (!reachable[index] || remap[index]) continue;
                Candidate candidate{index, {ConvertType(artifact, source[index])}, {}};
                bool complete = true;
                for (const auto& child : source)
                    if (child.ParentIndex == index) {
                        auto converted = ConvertType(artifact, child);
                        if (child.TypeIndex != shader::kShaderNoType) {
                            if (!remap[child.TypeIndex]) {
                                complete = false;
                                break;
                            }
                            converted.TypeIndex = remap[child.TypeIndex];
                        }
                        candidate.Records.push_back(std::move(converted));
                    }
                if (!complete) continue;
                std::sort(candidate.Records.begin() + 1, candidate.Records.end(), [](const auto& a, const auto& b) { return a.Name < b.Name; });
                BinaryWriter writer;
                writer.Size32(candidate.Records.size());
                for (size_t i = 0; i < candidate.Records.size(); ++i) TypeKey(writer, candidate.Records[i], i != 0);
                candidate.Key = std::move(writer).TakeData();
                ready.push_back(std::move(candidate));
            }
            if (ready.empty()) break;
            std::sort(ready.begin(), ready.end(), [](const auto& a, const auto& b) { return a.Key < b.Key; });
            for (auto& candidate : ready) {
                const auto found = _roots.find(candidate.Key);
                if (found != _roots.end()) {
                    remap[candidate.Source] = found->second;
                    continue;
                }
                const auto root = static_cast<uint32_t>(_types.size());
                _roots.emplace(std::move(candidate.Key), root);
                remap[candidate.Source] = root;
                for (size_t i = 0; i < candidate.Records.size(); ++i) {
                    if (i) candidate.Records[i].ParentIndex = root;
                    _types.push_back(std::move(candidate.Records[i]));
                }
            }
        }
        return remap;
    }

private:
    vector<shader::ShaderTypeRecord>& _types;
    map<vector<byte>, uint32_t> _roots;
};

std::optional<gpu::SamplerDescriptor> Sampler(const shader::ShaderArtifactSamplerRecord& source) {
    // Direct casts below depend on these schema 10 codes.
    static_assert(static_cast<uint32_t>(gpu::FilterMode::Nearest) == 0 &&
                  static_cast<uint32_t>(gpu::FilterMode::Linear) == 1);
    static_assert(static_cast<uint32_t>(gpu::CompareFunction::Never) == 0 &&
                  static_cast<uint32_t>(gpu::CompareFunction::Less) == 1 &&
                  static_cast<uint32_t>(gpu::CompareFunction::Equal) == 2 &&
                  static_cast<uint32_t>(gpu::CompareFunction::LessEqual) == 3 &&
                  static_cast<uint32_t>(gpu::CompareFunction::Greater) == 4 &&
                  static_cast<uint32_t>(gpu::CompareFunction::NotEqual) == 5 &&
                  static_cast<uint32_t>(gpu::CompareFunction::GreaterEqual) == 6 &&
                  static_cast<uint32_t>(gpu::CompareFunction::Always) == 7);
    static_assert(static_cast<uint32_t>(gpu::SamplerBorderColor::FloatTransparentBlack) == 0 &&
                  static_cast<uint32_t>(gpu::SamplerBorderColor::IntTransparentBlack) == 1 &&
                  static_cast<uint32_t>(gpu::SamplerBorderColor::FloatOpaqueBlack) == 2 &&
                  static_cast<uint32_t>(gpu::SamplerBorderColor::IntOpaqueBlack) == 3 &&
                  static_cast<uint32_t>(gpu::SamplerBorderColor::FloatOpaqueWhite) == 4 &&
                  static_cast<uint32_t>(gpu::SamplerBorderColor::IntOpaqueWhite) == 5);
    static_assert(static_cast<uint32_t>(gpu::SamplerReductionMode::WeightedAverage) == 0 &&
                  static_cast<uint32_t>(gpu::SamplerReductionMode::Min) == 1 &&
                  static_cast<uint32_t>(gpu::SamplerReductionMode::Max) == 2);
    const array addresses{gpu::AddressMode::Repeat, gpu::AddressMode::Mirror, gpu::AddressMode::ClampToEdge,
                          gpu::AddressMode::ClampToBorder, gpu::AddressMode::MirrorClampToEdge};
    if (source.AnisotropyEnable && (source.MaxAnisotropy < 1 || source.MaxAnisotropy > 16 || std::trunc(source.MaxAnisotropy) != source.MaxAnisotropy)) return std::nullopt;
    gpu::SamplerDescriptor result;
    result.AddressS = addresses[source.AddressModeU];
    result.AddressT = addresses[source.AddressModeV];
    result.AddressR = addresses[source.AddressModeW];
    result.MinFilter = static_cast<gpu::FilterMode>(source.MinFilter);
    result.MagFilter = static_cast<gpu::FilterMode>(source.MagFilter);
    result.MipmapFilter = static_cast<gpu::FilterMode>(source.MipmapMode);
    result.LodMin = source.MinLod;
    result.LodMax = source.MaxLod;
    if (source.CompareEnable) result.Compare = static_cast<gpu::CompareFunction>(source.CompareOp);
    result.AnisotropyClamp = source.AnisotropyEnable ? static_cast<uint32_t>(source.MaxAnisotropy) : 0;
    result.MipLodBias = source.MipLodBias;
    result.BorderColor = static_cast<gpu::SamplerBorderColor>(source.BorderColor);
    result.Reduction = static_cast<gpu::SamplerReductionMode>(source.ReductionMode);
    result.UnnormalizedCoordinates = (source.Flags & kShaderSamplerFlagUnnormalizedCoordinates) != 0;
    return result;
}

struct Lane {
    map<string, ShaderBindingDesc> Bindings;
    map<string, ShaderPushConstantDesc> Pushes;
    unordered_set<string> StaticSamplers;
    ShaderVariantTargetInterfaceDesc Interface;
    vector<string> ActiveBindings;
    vector<string> ActivePushes;
};

bool ShouldPromoteDxilCBuffer(const ShaderReflectionPolicy& policy, std::string_view name) noexcept {
    return std::any_of(policy.PushConstants.begin(), policy.PushConstants.end(), [&](const auto& rule) {
        return rule.PromoteDxilCBufferToRootConstants && rule.Name == name;
    });
}

bool IsAllowedDxilOnlyPush(const ShaderReflectionPolicy& policy, std::string_view name) noexcept {
    return std::any_of(policy.PushConstants.begin(), policy.PushConstants.end(), [&](const auto& rule) {
        return rule.AllowDxilOnly && rule.Name == name;
    });
}

bool SamePushNames(const map<string, ShaderPushConstantDesc>& dxil,
                   const map<string, ShaderPushConstantDesc>& spirv,
                   const ShaderReflectionPolicy& policy) noexcept {
    for (const auto& [name, _] : spirv) {
        if (!dxil.contains(name)) return false;
    }
    for (const auto& [name, _] : dxil) {
        if (!spirv.contains(name) && !IsAllowedDxilOnlyPush(policy, name)) return false;
    }
    return true;
}

bool ReadLane(const shader::ShaderArtifactView& artifact, bool dxil, const ShaderReflectionPolicy& policy, TypeInterner& interner, Lane& lane, string& error) {
    const auto remap = interner.Import(artifact);
    lane.Interface.Stages = gpu::ShaderStages{uint32_t{artifact.Envelope().StageMask}};
    for (const auto& wire : artifact.VertexInputs()) lane.Interface.VertexInputs.push_back({string{*artifact.GetName(wire.Semantic)}, wire.SemanticIndex, wire.Location,
                                                                                            static_cast<shader::ShaderVertexComponentType>(wire.ComponentType), wire.ComponentCount});
    std::sort(lane.Interface.VertexInputs.begin(), lane.Interface.VertexInputs.end(), [](const auto& a, const auto& b) { return a.Location < b.Location; });
    for (const auto& wire : artifact.Bindings()) {
        const auto kind = *DecodeBindingKind(wire.Type);
        const string name{*artifact.GetName(wire.Name)};
        if (dxil && kind == gpu::BindingKind::CBuffer && ShouldPromoteDxilCBuffer(policy, name)) {
            if (wire.Count != 1 || !detail::IsRoot(artifact.Types(), wire.TypeIndex)) {
                error = radray::format("DXIL cbuffer '{}' cannot be promoted to root constants", name);
                return false;
            }
            const uint32_t actualSize = artifact.Types()[wire.TypeIndex].Size;
            if (actualSize == 0 || (actualSize % 4) != 0 || actualSize > 64 * 4) {
                error = radray::format("DXIL cbuffer '{}' root constant size {} is invalid", name, actualSize);
                return false;
            }
            ShaderPushConstantDesc entry;
            entry.Name = name;
            entry.Push.Size = actualSize;
            entry.Push.Stages = gpu::ShaderStages{wire.StageMask};
            entry.Push.D3D12 = gpu::PushConstantDescriptor::D3D12Location{wire.Binding, wire.Group};
            entry.TypeIndex = remap[wire.TypeIndex];
            if (wire.StageMask != 0) lane.ActivePushes.push_back(entry.Name);
            if (!lane.Pushes.emplace(entry.Name, std::move(entry)).second) {
                error = radray::format("duplicate push constant '{}'", name);
                return false;
            }
            continue;
        }
        ShaderBindingDesc entry;
        entry.Name = name;
        entry.Binding.Kind = kind;
        entry.Binding.Count = wire.Count;
        entry.Binding.Stages = gpu::ShaderStages{wire.StageMask};
        entry.BufferByteSize = wire.BufferByteSize;
        if (wire.TypeIndex != shader::kShaderNoType) entry.TypeIndex = remap[wire.TypeIndex];
        const bool root = wire.Placement == static_cast<uint32_t>(shader::ShaderBindingPlacement::RootDescriptor);
        if (dxil)
            entry.Binding.D3D12 = gpu::D3D12BindingDescriptor{
                gpu::GetBindingRegisterClass(entry.Binding.Kind), wire.Binding, wire.Group,
                root ? gpu::D3D12BufferPlacement::RootDescriptor : gpu::D3D12BufferPlacement::Table};
        else
            entry.Binding.Vulkan = gpu::VulkanBindingDescriptor{wire.Group, wire.Binding,
                                                                   root ? gpu::VulkanBufferDescriptorPlacement::Dynamic : gpu::VulkanBufferDescriptorPlacement::Regular};
        if ((dxil && wire.Placement == static_cast<uint32_t>(shader::ShaderBindingPlacement::StaticSampler)) ||
            (!dxil && wire.SamplerIndex != shader::kShaderNoSampler)) {
            lane.StaticSamplers.insert(entry.Name);
            if (!dxil) {
                if (wire.SamplerIndex >= artifact.Samplers().size()) {
                    error = "missing immutable sampler state";
                    return false;
                }
                entry.Binding.ImmutableSampler = Sampler(artifact.Samplers()[wire.SamplerIndex]);
                if (!entry.Binding.ImmutableSampler) {
                    error = "immutable sampler cannot be represented by the RHI";
                    return false;
                }
            }
        }
        lane.ActiveBindings.push_back(entry.Name);
        lane.Bindings.emplace(entry.Name, std::move(entry));
    }
    for (const auto& wire : artifact.RootConstants()) {
        ShaderPushConstantDesc entry;
        entry.Name = *artifact.GetName(wire.Name);
        entry.Push.Size = wire.Size;
        entry.Push.Stages = gpu::ShaderStages{wire.StageMask ? wire.StageMask : uint32_t{artifact.Envelope().StageMask}};
        if (wire.TypeIndex != shader::kShaderNoType) entry.TypeIndex = remap[wire.TypeIndex];
        if (dxil)
            entry.Push.D3D12 = gpu::PushConstantDescriptor::D3D12Location{wire.Register, wire.RegisterSpace};
        else
            entry.Push.Vulkan = gpu::PushConstantDescriptor::VulkanLocation{wire.Offset};
        if (wire.StageMask) lane.ActivePushes.push_back(entry.Name);
        lane.Pushes.emplace(entry.Name, std::move(entry));
    }
    return true;
}

template <typename T>
bool MergeMapping(std::optional<T>& target, const std::optional<T>& source) {
    if (target && source && *target != *source) return false;
    if (source) target = source;
    return true;
}

bool MergeBinding(ShaderBindingDesc& target, const ShaderBindingDesc& source) {
    auto& a = target.Binding;
    const auto& b = source.Binding;
    if (a.Kind != b.Kind || a.Count != b.Count || target.TypeIndex != source.TypeIndex || a.ImmutableSampler != b.ImmutableSampler ||
        !MergeMapping(a.D3D12, b.D3D12) || !MergeMapping(a.Vulkan, b.Vulkan)) return false;
    a.Stages |= b.Stages;
    target.BufferByteSize = std::max(target.BufferByteSize, source.BufferByteSize);
    return true;
}

bool MergePush(ShaderPushConstantDesc& target, const ShaderPushConstantDesc& source) {
    if (target.Push.Size != source.Push.Size || (target.TypeIndex && source.TypeIndex && target.TypeIndex != source.TypeIndex) ||
        !MergeMapping(target.Push.D3D12, source.Push.D3D12) || !MergeMapping(target.Push.Vulkan, source.Push.Vulkan)) return false;
    if (source.TypeIndex) target.TypeIndex = source.TypeIndex;
    target.Push.Stages |= source.Push.Stages;
    return true;
}

template <typename T>
bool SameNames(const map<string, T>& a, const map<string, T>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) { return x.first == y.first; });
}

struct PendingVariant {
    string Name;
    std::optional<Lane> DXIL;
    std::optional<Lane> SPIRV;
};

}  // namespace

shader::DiagnosticResult<ShaderFamilyLayoutDesc> ReflectShaderFamily(std::span<const ShaderVariantArtifacts> variants, const ShaderReflectionPolicy& policy) {
    if (variants.empty()) return {std::nullopt, "shader family has no variants"};
    ShaderFamilyLayoutDesc output;
    TypeInterner interner{output.SharedLayout.Types};
    vector<const ShaderVariantArtifacts*> ordered;
    for (const auto& variant : variants) ordered.push_back(&variant);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->Name < b->Name; });
    map<string, ShaderBindingDesc> bindings;
    map<string, ShaderPushConstantDesc> pushes;
    vector<PendingVariant> pending;
    bool hasAnyDxil = false;
    string error;
    for (const auto* input : ordered) {
        if (input->Name.empty() || (!input->DXIL && !input->SPIRV) || (!pending.empty() && pending.back().Name == input->Name))
            return {std::nullopt, "variant needs a unique name and at least one artifact"};
        PendingVariant variant{input->Name, std::nullopt, std::nullopt};
        if (input->DXIL) {
            hasAnyDxil = true;
            variant.DXIL.emplace();
            if (!ReadLane(input->DXIL.Get()->Generic(), true, policy, interner, *variant.DXIL, error)) return {std::nullopt, error};
        }
        if (input->SPIRV) {
            variant.SPIRV.emplace();
            if (!ReadLane(input->SPIRV.Get()->Generic(), false, policy, interner, *variant.SPIRV, error)) return {std::nullopt, error};
        }
        if (variant.DXIL && variant.SPIRV && !policy.AllowTargetOnlyBindings &&
            (!SameNames(variant.DXIL->Bindings, variant.SPIRV->Bindings) || !SamePushNames(variant.DXIL->Pushes, variant.SPIRV->Pushes, policy)))
            return {std::nullopt, radray::format("variant '{}' has target-only resources", input->Name)};
        if (variant.DXIL)
            for (const auto& name : variant.DXIL->StaticSamplers) {
                if (!variant.SPIRV || !variant.SPIRV->StaticSamplers.contains(name))
                    return {std::nullopt, radray::format("DXIL static sampler '{}' requires paired SPIR-V state", name)};
                variant.DXIL->Bindings.at(name).Binding.ImmutableSampler = variant.SPIRV->Bindings.at(name).Binding.ImmutableSampler;
            }
        if (variant.DXIL && variant.SPIRV)
            for (const auto& name : variant.SPIRV->StaticSamplers)
                if (variant.DXIL->Bindings.contains(name) && !variant.DXIL->StaticSamplers.contains(name))
                    return {std::nullopt, radray::format("sampler '{}' differs in immutability", name)};
        for (const auto* lane : {&variant.DXIL, &variant.SPIRV})
            if (*lane) {
                for (const auto& [name, entry] : (*lane)->Bindings) {
                    auto [position, inserted] = bindings.emplace(name, entry);
                    if (!inserted && !MergeBinding(position->second, entry))
                        return {std::nullopt, radray::format("variant '{}': incompatible binding '{}'", input->Name, name)};
                }
                for (const auto& [name, entry] : (*lane)->Pushes) {
                    auto [position, inserted] = pushes.emplace(name, entry);
                    if (!inserted && !MergePush(position->second, entry))
                        return {std::nullopt, radray::format("variant '{}': incompatible push constant '{}'", input->Name, name)};
                }
            }
        pending.push_back(std::move(variant));
    }
    map<string, const ShaderBindingOverride*> overrides;
    for (const auto& override : policy.Bindings)
        if (!bindings.contains(override.Name) || !overrides.emplace(override.Name, &override).second)
            return {std::nullopt, "binding override has unknown or duplicate name"};
    map<uint32_t, ShaderBindingGroupDesc> groups;
    map<string, ShaderBindingReference> locations;
    map<uint32_t, unordered_set<uint32_t>> reserved;
    for (const auto& [name, entry] : bindings) {
        uint32_t slot = entry.Binding.Vulkan ? entry.Binding.Vulkan->Set : entry.Binding.D3D12->Space;
        auto policyEntry = overrides.find(name);
        if (policyEntry != overrides.end() && policyEntry->second->GroupSlot) slot = *policyEntry->second->GroupSlot;
        locations.emplace(name, ShaderBindingReference{slot, 0});
        auto& group = groups[slot];
        group.GroupSlot = slot;
        if (policyEntry != overrides.end()) {
            const auto& override = *policyEntry->second;
            if (override.BindingId && !reserved[slot].insert(*override.BindingId).second) return {std::nullopt, "duplicate explicit binding ID"};
            if (override.GroupName) {
                if (override.GroupName->empty() || (!group.Name.empty() && group.Name != *override.GroupName)) return {std::nullopt, "inconsistent group names"};
                group.Name = *override.GroupName;
            }
        }
    }
    for (auto& [name, entry] : bindings) {
        auto& location = locations.at(name);
        const auto rule = overrides.find(name);
        uint32_t id = 0;
        if (rule != overrides.end() && rule->second->BindingId)
            id = *rule->second->BindingId;
        else {
            while (reserved[location.GroupSlot].contains(id)) {
                if (id == std::numeric_limits<uint32_t>::max()) return {std::nullopt, "binding ID space exhausted"};
                ++id;
            }
            reserved[location.GroupSlot].insert(id);
        }
        location.BindingId = entry.Binding.BindingId = id;
        groups[location.GroupSlot].Bindings.push_back(std::move(entry));
    }
    for (auto& [slot, group] : groups) {
        if (group.Name.empty()) group.Name = radray::format("Group{}", slot);
        std::sort(group.Bindings.begin(), group.Bindings.end(), [](const auto& a, const auto& b) { return a.Binding.BindingId < b.Binding.BindingId; });
        output.SharedLayout.Groups.push_back(std::move(group));
    }
    map<string, uint32_t> pushLocations;
    unordered_set<uint32_t> pushIds;
    for (const auto& rule : policy.PushConstants) {
        if (!pushLocations.emplace(rule.Name, rule.PushId).second || !pushIds.insert(rule.PushId).second)
            return {std::nullopt, "push override has unknown/duplicate name or ID"};
        if (!pushes.contains(rule.Name) && !(rule.AllowDxilOnly && !hasAnyDxil))
            return {std::nullopt, "push override has unknown/duplicate name or ID"};
    }
    for (auto& [name, entry] : pushes) {
        auto found = pushLocations.find(name);
        uint32_t id = 0;
        if (found != pushLocations.end())
            id = found->second;
        else {
            while (pushIds.contains(id)) {
                if (id == std::numeric_limits<uint32_t>::max()) return {std::nullopt, "push ID space exhausted"};
                ++id;
            }
            pushIds.insert(id);
            pushLocations.emplace(name, id);
        }
        entry.Push.PushId = id;
        output.SharedLayout.PushConstants.push_back(std::move(entry));
    }
    for (auto& variant : pending) {
        ShaderVariantInterfaceDesc program{.Name = std::move(variant.Name)};
        for (bool dxil : {true, false}) {
            auto& lane = dxil ? variant.DXIL : variant.SPIRV;
            if (!lane) continue;
            for (const auto& name : lane->ActiveBindings) lane->Interface.ActiveBindings.push_back(locations.at(name));
            for (const auto& name : lane->ActivePushes) lane->Interface.ActivePushConstants.push_back(pushLocations.at(name));
            auto& target = dxil ? program.DXIL : program.SPIRV;
            target = std::move(lane->Interface);
        }
        output.Variants.push_back(std::move(program));
    }
    if (!ValidateShaderFamilyLayoutDesc(output, &error)) return {std::nullopt, std::move(error)};
    return {std::move(output), {}};
}

shader::DiagnosticResult<ShaderFamilyLayoutDesc> ReflectShaderVariant(const ShaderVariantArtifacts& variant, const ShaderReflectionPolicy& policy) {
    return ReflectShaderFamily(std::span{&variant, size_t{1}}, policy);
}

}  // namespace radray::shader
