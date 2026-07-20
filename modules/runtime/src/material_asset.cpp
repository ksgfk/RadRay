#include <radray/runtime/material_asset.h>

#include <algorithm>

namespace radray {
namespace {

bool GroupContainsDefine(
    const shader::ShaderKeywordGroupDesc& group,
    std::string_view define) noexcept {
    return std::ranges::find(group.Alternatives, define) != group.Alternatives.end();
}

bool ShaderDeclaresLocalDefine(const ShaderAsset& shaderAsset, std::string_view define) noexcept {
    return std::ranges::any_of(shaderAsset.GetPasses(), [define](const shader::ShaderPassDesc& pass) {
        return std::ranges::any_of(
            pass.VariantDomain.KeywordGroups,
            [define](const shader::ShaderKeywordGroupDesc& group) {
                return group.Scope == shader::ShaderKeywordScope::Local && GroupContainsDefine(group, define);
            });
    });
}

vector<ShaderProgramInterfaceRecord> CollectBakedInterfaces(
    const shader::ShaderBinary& binary) {
    vector<ShaderProgramInterfaceRecord> records;
    vector<uint32_t> indices;
    for (const shader::ShaderProgramVariantArtifact& program : binary.ProgramVariants) {
        if (std::ranges::find(indices, program.InterfaceIndex) != indices.end()) continue;
        indices.emplace_back(program.InterfaceIndex);
        records.emplace_back(ShaderProgramInterfaceRecord{
            .Interface = binary.ProgramInterfaces[program.InterfaceIndex],
            .Context = shader::ShaderDiagnosticContext{
                .Target = program.Target,
                .PassIndex = program.PassIndex,
                .VariantDefines = program.Defines}});
    }
    return records;
}

bool HasProgramIdentity(const ShaderProgramInterfaceRecord& record) noexcept {
    return record.Context.PassIndex.has_value();
}

bool SameProgramIdentity(
    const ShaderProgramInterfaceRecord& lhs,
    const ShaderProgramInterfaceRecord& rhs) noexcept {
    return HasProgramIdentity(lhs) && HasProgramIdentity(rhs) &&
           lhs.Context.PassIndex == rhs.Context.PassIndex &&
           lhs.Context.VariantDefines == rhs.Context.VariantDefines;
}

ShaderParameterLayoutBuildResult BuildMaterialLayout(
    const ShaderAsset& shaderAsset,
    const PipelineBindingPolicy& policy,
    std::span<const ShaderProgramInterfaceRecord> resolvedInterfaces) {
    vector<ShaderProgramInterfaceRecord> interfaces = CollectBakedInterfaces(shaderAsset.GetBinary());
    interfaces.insert(
        interfaces.end(),
        resolvedInterfaces.begin(),
        resolvedInterfaces.end());
    return BuildShaderParameterLayout(
        interfaces,
        policy,
        shader::ShaderProgramKind::Graphics);
}

}  // namespace

MaterialAsset::MaterialAsset(
    StreamingAssetRef<ShaderAsset> shaderRef,
    PipelineBindingPolicy policy) noexcept
    : _shader(std::move(shaderRef)), _policy(std::move(policy)) {
    RefreshShaderLayout();
}

MaterialAsset::~MaterialAsset() noexcept = default;

void MaterialAsset::OnUnload(IRenderResourceRecycler& /*recycler*/) {
    _shader.Reset();
    _policy = {};
    _parameters = {};
    _layoutDiagnostics.clear();
    _resolvedInterfaces.clear();
    _resolvedSourceIdentities.clear();
    _localKeywords.clear();
    _layoutShader = nullptr;
    _layoutInitialized = false;
    ++_revision;
}

AssetTypeId MaterialAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<MaterialAsset>;
}

bool MaterialAsset::SetShader(
    StreamingAssetRef<ShaderAsset> shaderRef,
    PipelineBindingPolicy policy) noexcept {
    const bool same = _shader.GetAssetId() == shaderRef.GetAssetId() &&
                      _shader.GetHandle() == shaderRef.GetHandle() && _policy == policy;
    if (!same) {
        _shader = std::move(shaderRef);
        _policy = std::move(policy);
        _parameters = {};
        _layoutDiagnostics.clear();
        _resolvedInterfaces.clear();
        _resolvedSourceIdentities.clear();
        _localKeywords.clear();
        _layoutShader = nullptr;
        _layoutInitialized = false;
        ++_revision;
    }
    return RefreshShaderLayout();
}

bool MaterialAsset::InstallLayout(
    ShaderParameterLayoutBuildResult result,
    const ShaderAsset* source) noexcept {
    _layoutDiagnostics = std::move(result.Diagnostics);
    if (!result.Succeeded() || !_parameters.Reset(*result.Layout, true)) {
        return false;
    }
    _layoutInitialized = true;
    _layoutShader = source;
    ++_revision;
    return true;
}

bool MaterialAsset::RefreshShaderLayout() noexcept {
    ShaderAsset* shaderAsset = _shader.Get();
    if (shaderAsset == nullptr || !shaderAsset->IsValid()) return false;
    if (_layoutInitialized && _layoutShader == shaderAsset) return true;
    if (_layoutShader != shaderAsset) {
        _resolvedInterfaces.clear();
        _resolvedSourceIdentities.clear();
    }
    return InstallLayout(
        BuildMaterialLayout(*shaderAsset, _policy, _resolvedInterfaces),
        shaderAsset);
}

bool MaterialAsset::ApplyResolvedInterfaces(
    std::span<const ShaderProgramInterfaceRecord> interfaces) noexcept {
    ShaderAsset* shaderAsset = _shader.Get();
    if (shaderAsset == nullptr || !shaderAsset->IsValid()) return false;
    vector<ShaderProgramInterfaceRecord> candidate =
        _layoutShader == shaderAsset ? _resolvedInterfaces : vector<ShaderProgramInterfaceRecord>{};
    bool changed = _layoutShader != shaderAsset;
    for (ShaderProgramInterfaceRecord incoming : interfaces) {
        shader::NormalizeShaderDefines(incoming.Context.VariantDefines);
        incoming.Context.Stage = shader::ShaderStage::UNKNOWN;
        if (incoming.Interface.Kind != shader::ShaderProgramKind::Graphics ||
            !shader::IsShaderInterfaceValid(incoming.Interface)) {
            _layoutDiagnostics = {ShaderBindingDiagnostic{
                .Code = ShaderBindingDiagnosticCode::InvalidInterface,
                .Message = "MaterialAsset only accepts valid graphics program interfaces",
                .Context = std::move(incoming.Context)}};
            return false;
        }
        if (incoming.Context.PassIndex.has_value()) {
            const uint32_t passIndex = *incoming.Context.PassIndex;
            if (passIndex >= shaderAsset->GetPasses().size() ||
                !shader::IsShaderVariantInDomain(
                    shaderAsset->GetPasses()[passIndex],
                    incoming.Context.VariantDefines)) {
                _layoutDiagnostics = {ShaderBindingDiagnostic{
                    .Code = ShaderBindingDiagnosticCode::InvalidInterface,
                    .Message = "resolved interface context is outside the ShaderAsset variant domain",
                    .Context = std::move(incoming.Context)}};
                return false;
            }
            for (const shader::ShaderProgramVariantArtifact& baked :
                 shaderAsset->GetBinary().ProgramVariants) {
                if (baked.PassIndex != passIndex ||
                    baked.Defines != incoming.Context.VariantDefines) {
                    continue;
                }
                const shader::ShaderInterfaceDesc& bakedInterface =
                    shaderAsset->GetBinary().ProgramInterfaces[baked.InterfaceIndex];
                if (bakedInterface != incoming.Interface) {
                    _layoutDiagnostics = {ShaderBindingDiagnostic{
                        .Code = ShaderBindingDiagnosticCode::InterfaceMismatch,
                        .Message = "resolved interface is incompatible with the baked interface for the same program",
                        .Context = std::move(incoming.Context),
                        .RelatedContext = shader::ShaderDiagnosticContext{
                            .Target = baked.Target,
                            .PassIndex = baked.PassIndex,
                            .VariantDefines = baked.Defines}}};
                    return false;
                }
                break;
            }
        }

        auto existing = std::ranges::find_if(candidate, [&](const ShaderProgramInterfaceRecord& value) {
            return SameProgramIdentity(value, incoming);
        });
        if (existing != candidate.end()) {
            if (existing->Interface != incoming.Interface) {
                _layoutDiagnostics = {ShaderBindingDiagnostic{
                    .Code = ShaderBindingDiagnosticCode::InterfaceMismatch,
                    .Message = "the same pass and variant resolved to incompatible canonical interfaces",
                    .Context = std::move(incoming.Context),
                    .RelatedContext = existing->Context}};
                return false;
            }
            continue;
        }
        if (!HasProgramIdentity(incoming) &&
            std::ranges::any_of(candidate, [&](const ShaderProgramInterfaceRecord& value) {
                return !HasProgramIdentity(value) && value.Interface == incoming.Interface;
            })) {
            continue;
        }
        candidate.emplace_back(std::move(incoming));
        changed = true;
    }

    if (!changed && _layoutInitialized && _layoutShader == shaderAsset) return true;
    ShaderParameterLayoutBuildResult layout = BuildMaterialLayout(
        *shaderAsset,
        _policy,
        candidate);
    if (!InstallLayout(std::move(layout), shaderAsset)) return false;
    _resolvedInterfaces = std::move(candidate);
    return true;
}

bool MaterialAsset::ApplyResolvedPrograms(
    std::span<const ShaderResolvedProgram> programs) noexcept {
    ShaderAsset* shaderAsset = _shader.Get();
    if (shaderAsset == nullptr || !shaderAsset->IsValid()) return false;
    unordered_map<uint32_t, shader::ShaderHash> sourceIdentities =
        _layoutShader == shaderAsset
            ? _resolvedSourceIdentities
            : unordered_map<uint32_t, shader::ShaderHash>{};
    vector<ShaderProgramInterfaceRecord> interfaces;
    interfaces.reserve(programs.size());
    for (const ShaderResolvedProgram& program : programs) {
        vector<string> normalizedDefines = program.Defines;
        shader::NormalizeShaderDefines(normalizedDefines);
        const bool knownTarget = program.Target == shader::ShaderTarget::DXIL ||
                                 program.Target == shader::ShaderTarget::SPIRV;
        if (!knownTarget || program.PassIndex >= shaderAsset->GetPasses().size() ||
            normalizedDefines != program.Defines ||
            !shader::IsShaderVariantInDomain(
                shaderAsset->GetPasses()[program.PassIndex],
                program.Defines) ||
            ComputeShaderProgramIdentity(
                shaderAsset->GetPasses()[program.PassIndex],
                program.PassIndex,
                program.Defines,
                program.SourceIdentity) != program.ProgramIdentity ||
            (shaderAsset->GetPasses()[program.PassIndex].SourceIdentity != shader::ShaderHash{} &&
             shaderAsset->GetPasses()[program.PassIndex].SourceIdentity != program.SourceIdentity)) {
            _layoutDiagnostics = {ShaderBindingDiagnostic{
                .Code = ShaderBindingDiagnosticCode::InterfaceMismatch,
                .Message = "resolved program provenance does not match this immutable ShaderAsset",
                .Context = shader::ShaderDiagnosticContext{
                    .Target = program.Target,
                    .PassIndex = program.PassIndex,
                    .VariantDefines = program.Defines}}};
            return false;
        }

        const auto existing = sourceIdentities.find(program.PassIndex);
        if (existing != sourceIdentities.end() && existing->second != program.SourceIdentity) {
            _layoutDiagnostics = {ShaderBindingDiagnostic{
                .Code = ShaderBindingDiagnosticCode::InterfaceMismatch,
                .Message = "resolved programs come from different immutable shader source identities",
                .Context = shader::ShaderDiagnosticContext{
                    .Target = program.Target,
                    .PassIndex = program.PassIndex,
                    .VariantDefines = program.Defines}}};
            return false;
        }
        if (program.SourceIdentity != shader::ShaderHash{}) {
            sourceIdentities.insert_or_assign(program.PassIndex, program.SourceIdentity);
        }
        interfaces.emplace_back(ShaderProgramInterfaceRecord{
            .Interface = program.Interface,
            .Context = shader::ShaderDiagnosticContext{
                .Target = program.Target,
                .PassIndex = program.PassIndex,
                .VariantDefines = program.Defines}});
    }
    if (!ApplyResolvedInterfaces(interfaces)) return false;
    _resolvedSourceIdentities = std::move(sourceIdentities);
    return true;
}

bool MaterialAsset::IsReady() const noexcept {
    ShaderAsset* shaderAsset = _shader.Get();
    return shaderAsset != nullptr && shaderAsset->IsValid() && _layoutInitialized &&
           _layoutShader == shaderAsset;
}

bool MaterialAsset::HasCompleteParametersFor(
    const shader::ShaderInterfaceDesc& interface,
    const shader::ShaderDiagnosticContext& context) const noexcept {
    if (!IsReady()) return false;
    ShaderBindingResolutionResult resolution = ResolveShaderBindings(interface, _policy, context);
    return resolution.Succeeded() && _parameters.IsCompleteFor(*resolution.Plan);
}

bool MaterialAsset::EnableLocalKeyword(std::string_view define) noexcept {
    ShaderAsset* shaderAsset = _shader.Get();
    if (define.empty() || shaderAsset == nullptr || !ShaderDeclaresLocalDefine(*shaderAsset, define)) return false;
    vector<string> next = _localKeywords;
    for (const shader::ShaderPassDesc& pass : shaderAsset->GetPasses()) {
        for (const shader::ShaderKeywordGroupDesc& group : pass.VariantDomain.KeywordGroups) {
            if (group.Scope != shader::ShaderKeywordScope::Local || !GroupContainsDefine(group, define)) continue;
            std::erase_if(next, [&](const string& enabled) {
                return enabled != define && GroupContainsDefine(group, enabled);
            });
        }
    }
    next.emplace_back(define);
    shader::NormalizeShaderDefines(next);
    if (next != _localKeywords) {
        _localKeywords = std::move(next);
        ++_revision;
    }
    return true;
}

bool MaterialAsset::DisableLocalKeyword(std::string_view define) noexcept {
    ShaderAsset* shaderAsset = _shader.Get();
    if (define.empty() || shaderAsset == nullptr || !ShaderDeclaresLocalDefine(*shaderAsset, define)) return false;
    if (std::erase(_localKeywords, define) != 0) ++_revision;
    return true;
}

bool MaterialAsset::IsLocalKeywordEnabled(std::string_view define) const noexcept {
    return std::ranges::find(_localKeywords, define) != _localKeywords.end();
}

std::optional<vector<string>> MaterialAsset::ResolveVariantDefines(
    uint32_t passIndex,
    std::span<const std::string_view> pipelineGlobalDefines) const noexcept {
    try {
        ShaderAsset* shaderAsset = _shader.Get();
        if (shaderAsset == nullptr || passIndex >= shaderAsset->GetPasses().size()) return std::nullopt;
        const shader::ShaderPassDesc& pass = shaderAsset->GetPasses()[passIndex];
        vector<string> result;
        for (const string& define : _localKeywords) {
            const bool used = std::ranges::any_of(
                pass.VariantDomain.KeywordGroups,
                [&](const auto& group) {
                    return group.Scope == shader::ShaderKeywordScope::Local && GroupContainsDefine(group, define);
                });
            if (used) result.emplace_back(define);
        }
        for (const std::string_view define : pipelineGlobalDefines) {
            bool global = false;
            bool local = false;
            for (const shader::ShaderKeywordGroupDesc& group : pass.VariantDomain.KeywordGroups) {
                if (!GroupContainsDefine(group, define)) continue;
                global = global || group.Scope == shader::ShaderKeywordScope::Global;
                local = local || group.Scope == shader::ShaderKeywordScope::Local;
            }
            if (local) return std::nullopt;
            if (global) result.emplace_back(define);
        }
        shader::NormalizeShaderDefines(result);
        return shader::IsShaderVariantInDomain(pass, result)
                   ? std::optional<vector<string>>{std::move(result)}
                   : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

bool MaterialAsset::SetFloat(std::string_view name, float value, uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetFloat(name, value, arrayIndex);
    });
}

bool MaterialAsset::SetFloat(
    ShaderParameterLocation location,
    std::string_view field,
    float value,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetFloat(location, field, value, arrayIndex);
    });
}

bool MaterialAsset::SetInt(std::string_view name, int32_t value, uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetInt(name, value, arrayIndex);
    });
}

bool MaterialAsset::SetInt(
    ShaderParameterLocation location,
    std::string_view field,
    int32_t value,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetInt(location, field, value, arrayIndex);
    });
}

bool MaterialAsset::SetUInt(std::string_view name, uint32_t value, uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetUInt(name, value, arrayIndex);
    });
}

bool MaterialAsset::SetUInt(
    ShaderParameterLocation location,
    std::string_view field,
    uint32_t value,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetUInt(location, field, value, arrayIndex);
    });
}

bool MaterialAsset::SetBool(std::string_view name, bool value, uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetBool(name, value, arrayIndex);
    });
}

bool MaterialAsset::SetBool(
    ShaderParameterLocation location,
    std::string_view field,
    bool value,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetBool(location, field, value, arrayIndex);
    });
}

bool MaterialAsset::SetVector(
    std::string_view name,
    const Eigen::Vector4f& value,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetVector(name, value, arrayIndex);
    });
}

bool MaterialAsset::SetVector(
    ShaderParameterLocation location,
    std::string_view field,
    const Eigen::Vector4f& value,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetVector(location, field, value, arrayIndex);
    });
}

bool MaterialAsset::SetValue(
    std::string_view name,
    shader::ShaderScalarType scalar,
    uint32_t columns,
    std::span<const byte> data,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetValue(name, scalar, columns, data, arrayIndex);
    });
}

bool MaterialAsset::SetValue(
    ShaderParameterLocation location,
    std::string_view field,
    shader::ShaderScalarType scalar,
    uint32_t columns,
    std::span<const byte> data,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetValue(location, field, scalar, columns, data, arrayIndex);
    });
}

bool MaterialAsset::SetMatrix(
    std::string_view name,
    std::span<const float> rowMajorValues,
    uint32_t rows,
    uint32_t columns,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetMatrix(name, rowMajorValues, rows, columns, arrayIndex);
    });
}

bool MaterialAsset::SetMatrix(
    ShaderParameterLocation location,
    std::string_view field,
    std::span<const float> rowMajorValues,
    uint32_t rows,
    uint32_t columns,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetMatrix(
            location,
            field,
            rowMajorValues,
            rows,
            columns,
            arrayIndex);
    });
}

bool MaterialAsset::SetConstantBuffer(std::string_view name, std::span<const byte> data) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetConstantBuffer(name, data);
    });
}

bool MaterialAsset::SetConstantBuffer(
    ShaderParameterLocation location,
    std::span<const byte> data) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetConstantBuffer(location, data);
    });
}

bool MaterialAsset::SetTexture(
    std::string_view name,
    StreamingAssetRef<TextureAsset> texture,
    const TextureSubViewDesc& view,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetTexture(name, std::move(texture), view, arrayIndex);
    });
}

bool MaterialAsset::SetTexture(
    ShaderParameterLocation location,
    StreamingAssetRef<TextureAsset> texture,
    const TextureSubViewDesc& view,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetTexture(location, std::move(texture), view, arrayIndex);
    });
}

bool MaterialAsset::SetSampler(
    std::string_view name,
    const render::SamplerDescriptor& sampler,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetSampler(name, sampler, arrayIndex);
    });
}

bool MaterialAsset::SetSampler(
    ShaderParameterLocation location,
    const render::SamplerDescriptor& sampler,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetSampler(location, sampler, arrayIndex);
    });
}

bool MaterialAsset::SetBuffer(
    std::string_view name,
    Nullable<render::Buffer*> buffer,
    render::BufferRange range,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetBuffer(name, buffer, range, arrayIndex);
    });
}

bool MaterialAsset::SetBuffer(
    ShaderParameterLocation location,
    Nullable<render::Buffer*> buffer,
    render::BufferRange range,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.SetBuffer(location, buffer, range, arrayIndex);
    });
}

bool MaterialAsset::ClearResource(std::string_view name, uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.ClearResource(name, arrayIndex);
    });
}

bool MaterialAsset::ClearResource(
    ShaderParameterLocation location,
    uint32_t arrayIndex) noexcept {
    return MutateParameters([&](ShaderParameterSet& parameters) {
        return parameters.ClearResource(location, arrayIndex);
    });
}

}  // namespace radray
