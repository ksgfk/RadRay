#include <radray/runtime/material_asset.h>

#include <algorithm>
#include <cstring>

namespace radray {
namespace {

bool SameAssetRef(
    const StreamingAssetRef<TextureAsset>& lhs,
    const StreamingAssetRef<TextureAsset>& rhs) noexcept {
    return lhs.GetAssetId() == rhs.GetAssetId() && lhs.GetHandle() == rhs.GetHandle();
}

bool GroupContainsDefine(
    const shader::ShaderKeywordGroupDesc& group,
    std::string_view define) noexcept {
    return std::ranges::find(group.Alternatives, define) != group.Alternatives.end();
}

bool ShaderDeclaresLocalDefine(const ShaderAsset& shaderAsset, std::string_view define) noexcept {
    return std::ranges::any_of(shaderAsset.GetPasses(), [define](const shader::ShaderPassDesc& pass) {
        return std::ranges::any_of(pass.KeywordGroups, [define](const shader::ShaderKeywordGroupDesc& group) {
            return group.Scope == shader::ShaderKeywordScope::Local && GroupContainsDefine(group, define);
        });
    });
}

}  // namespace

bool MaterialParameterStorage::Reset(const MaterialLayout& layout) noexcept {
    if (!layout.IsValid() || std::ranges::any_of(layout.Bindings, [](const auto& binding) {
            return binding.Count != 1;
        })) {
        return false;
    }
    try {
        vector<MaterialConstantBinding> constants;
        vector<MaterialTextureBinding> textures;
        vector<MaterialSamplerBinding> samplers;
        for (const MaterialBindingDesc& binding : layout.Bindings) {
            switch (binding.Kind) {
                case MaterialBindingKind::ConstantBuffer:
                    constants.emplace_back(MaterialConstantBinding{
                        .Name = binding.Name,
                        .Binding = binding.Binding,
                        .Data = vector<byte>(binding.ByteSize)});
                    break;
                case MaterialBindingKind::Texture:
                    textures.emplace_back(MaterialTextureBinding{
                        .Name = binding.Name,
                        .Binding = binding.Binding,
                        .Texture = {},
                        .View = {},
                        .IsSet = false});
                    break;
                case MaterialBindingKind::Sampler:
                    samplers.emplace_back(MaterialSamplerBinding{
                        .Name = binding.Name,
                        .Binding = binding.Binding,
                        .Sampler = {}});
                    break;
            }
        }
        _layout = layout;
        _constants = std::move(constants);
        _textures = std::move(textures);
        _samplers = std::move(samplers);
        ++_revision;
        return true;
    } catch (...) {
        return false;
    }
}

MaterialParameterStorage::ConstantFieldTarget MaterialParameterStorage::FindConstantField(
    std::string_view name) noexcept {
    const size_t separator = name.find('.');
    const std::string_view requestedBlock =
        separator == std::string_view::npos ? std::string_view{} : name.substr(0, separator);
    const std::string_view requestedField =
        separator == std::string_view::npos ? name : name.substr(separator + 1);

    ConstantFieldTarget result;
    for (MaterialConstantBinding& block : _constants) {
        if (!requestedBlock.empty() && block.Name != requestedBlock) {
            continue;
        }
        const auto layoutBinding = _layout.FindBinding(block.Binding);
        if (!layoutBinding.HasValue()) {
            return {};
        }
        const auto field = std::ranges::find_if(
            layoutBinding.Get()->Fields,
            [requestedField](const MaterialFieldDesc& value) {
                return value.Name == requestedField;
            });
        if (field == layoutBinding.Get()->Fields.end()) {
            continue;
        }
        if (result.Block != nullptr) {
            return {};
        }
        result = ConstantFieldTarget{.Block = &block, .Field = &*field};
    }
    return result;
}

bool MaterialParameterStorage::SetConstantField(
    std::string_view name,
    std::span<const byte> data) noexcept {
    const ConstantFieldTarget target = FindConstantField(name);
    if (target.Block == nullptr || target.Field == nullptr || target.Field->Size != data.size()) {
        return false;
    }
    std::span<byte> destination{
        target.Block->Data.data() + target.Field->Offset,
        target.Field->Size};
    if (!std::ranges::equal(destination, data)) {
        std::ranges::copy(data, destination.begin());
        ++_revision;
    }
    return true;
}

bool MaterialParameterStorage::SetFloat(std::string_view name, float value) noexcept {
    return SetConstantField(name, std::as_bytes(std::span{&value, 1}));
}

bool MaterialParameterStorage::SetVector(
    std::string_view name,
    const Eigen::Vector4f& value) noexcept {
    return SetConstantField(name, std::as_bytes(std::span<const float, 4>{value.data(), 4}));
}

bool MaterialParameterStorage::SetConstantBuffer(
    std::string_view name,
    std::span<const byte> data) noexcept {
    const auto it = std::ranges::find_if(_constants, [name](const MaterialConstantBinding& value) {
        return value.Name == name;
    });
    if (it == _constants.end() || it->Data.size() != data.size()) {
        return false;
    }
    if (!std::ranges::equal(it->Data, data)) {
        std::ranges::copy(data, it->Data.begin());
        ++_revision;
    }
    return true;
}

bool MaterialParameterStorage::SetTexture(
    std::string_view name,
    StreamingAssetRef<TextureAsset> texture,
    const TextureSubViewDesc& view) noexcept {
    const auto it = std::ranges::find_if(_textures, [name](const MaterialTextureBinding& value) {
        return value.Name == name;
    });
    if (it == _textures.end()) {
        return false;
    }
    if (!it->IsSet || !SameAssetRef(it->Texture, texture) || it->View != view) {
        it->Texture = std::move(texture);
        it->View = view;
        it->IsSet = true;
        ++_revision;
    }
    return true;
}

bool MaterialParameterStorage::ClearTexture(std::string_view name) noexcept {
    const auto it = std::ranges::find_if(_textures, [name](const MaterialTextureBinding& value) {
        return value.Name == name;
    });
    if (it == _textures.end()) {
        return false;
    }
    if (it->IsSet) {
        it->Texture.Reset();
        it->View = {};
        it->IsSet = false;
        ++_revision;
    }
    return true;
}

bool MaterialParameterStorage::SetSampler(
    std::string_view name,
    const render::SamplerDescriptor& sampler) noexcept {
    const auto it = std::ranges::find_if(_samplers, [name](const MaterialSamplerBinding& value) {
        return value.Name == name;
    });
    if (it == _samplers.end()) {
        return false;
    }
    if (!it->Sampler.has_value() || *it->Sampler != sampler) {
        it->Sampler = sampler;
        ++_revision;
    }
    return true;
}

bool MaterialParameterStorage::ClearSampler(std::string_view name) noexcept {
    const auto it = std::ranges::find_if(_samplers, [name](const MaterialSamplerBinding& value) {
        return value.Name == name;
    });
    if (it == _samplers.end()) {
        return false;
    }
    if (it->Sampler.has_value()) {
        it->Sampler.reset();
        ++_revision;
    }
    return true;
}

MaterialAsset::MaterialAsset(
    StreamingAssetRef<ShaderAsset> shaderRef,
    uint32_t bindingGroup) noexcept
    : _shader(std::move(shaderRef)), _bindingGroup(bindingGroup) {
    RefreshShaderLayout();
}

MaterialAsset::~MaterialAsset() noexcept = default;

void MaterialAsset::OnUnload(IRenderResourceRecycler& /*recycler*/) {
    _shader.Reset();
    _bindingGroup.reset();
    _localKeywords.clear();
    _parameters.Reset(MaterialLayout{});
    ++_revision;
}

AssetTypeId MaterialAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<MaterialAsset>;
}

bool MaterialAsset::SetShader(
    StreamingAssetRef<ShaderAsset> shaderRef,
    uint32_t bindingGroup) noexcept {
    const bool same = _shader.GetAssetId() == shaderRef.GetAssetId() &&
                      _shader.GetHandle() == shaderRef.GetHandle() &&
                      _bindingGroup == bindingGroup;
    if (!same) {
        _shader = std::move(shaderRef);
        _bindingGroup = bindingGroup;
        _localKeywords.clear();
        _parameters.Reset(MaterialLayout{});
        ++_revision;
    }
    return RefreshShaderLayout();
}

bool MaterialAsset::RefreshShaderLayout() noexcept {
    ShaderAsset* shaderAsset = _shader.Get();
    if (shaderAsset == nullptr || !shaderAsset->IsValid() || !_bindingGroup.has_value()) {
        return false;
    }
    const auto layout = BuildMaterialLayout(
        shaderAsset->GetBinary().Stages,
        *_bindingGroup);
    if (!layout.has_value()) {
        return false;
    }
    if (_parameters.GetLayout() == *layout) {
        return true;
    }
    if (!_parameters.Reset(*layout)) {
        return false;
    }
    ++_revision;
    return true;
}

bool MaterialAsset::IsReady() const noexcept {
    ShaderAsset* shaderAsset = _shader.Get();
    if (shaderAsset == nullptr || !shaderAsset->IsValid() || !_bindingGroup.has_value()) {
        return false;
    }
    const auto layout = BuildMaterialLayout(
        shaderAsset->GetBinary().Stages,
        *_bindingGroup);
    return layout.has_value() && _parameters.GetLayout() == *layout;
}

bool MaterialAsset::EnableLocalKeyword(std::string_view define) noexcept {
    ShaderAsset* shaderAsset = _shader.Get();
    if (define.empty() || shaderAsset == nullptr || !ShaderDeclaresLocalDefine(*shaderAsset, define)) {
        return false;
    }
    vector<string> next = _localKeywords;
    for (const shader::ShaderPassDesc& pass : shaderAsset->GetPasses()) {
        for (const shader::ShaderKeywordGroupDesc& group : pass.KeywordGroups) {
            if (group.Scope != shader::ShaderKeywordScope::Local || !GroupContainsDefine(group, define)) {
                continue;
            }
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
    if (define.empty() || shaderAsset == nullptr || !ShaderDeclaresLocalDefine(*shaderAsset, define)) {
        return false;
    }
    const size_t erased = std::erase(_localKeywords, define);
    if (erased != 0) {
        ++_revision;
    }
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
        if (shaderAsset == nullptr || passIndex >= shaderAsset->GetPasses().size()) {
            return std::nullopt;
        }
        const shader::ShaderPassDesc& pass = shaderAsset->GetPasses()[passIndex];
        vector<string> result;
        for (const string& define : _localKeywords) {
            const bool used = std::ranges::any_of(pass.KeywordGroups, [&](const auto& group) {
                return group.Scope == shader::ShaderKeywordScope::Local && GroupContainsDefine(group, define);
            });
            if (used) {
                result.emplace_back(define);
            }
        }
        for (const std::string_view define : pipelineGlobalDefines) {
            bool global = false;
            bool local = false;
            for (const shader::ShaderKeywordGroupDesc& group : pass.KeywordGroups) {
                if (!GroupContainsDefine(group, define)) {
                    continue;
                }
                global = global || group.Scope == shader::ShaderKeywordScope::Global;
                local = local || group.Scope == shader::ShaderKeywordScope::Local;
            }
            if (local) {
                return std::nullopt;
            }
            if (global) {
                result.emplace_back(define);
            }
        }
        shader::NormalizeShaderDefines(result);
        if (!shader::AreShaderDefinesValid(pass, shader::ShaderStage::UNKNOWN, result) ||
            !shader::IsDeclaredShaderVariant(pass, result)) {
            return std::nullopt;
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

bool MaterialAsset::SetFloat(std::string_view name, float value) noexcept {
    return MutateParameters([&](MaterialParameterStorage& storage) { return storage.SetFloat(name, value); });
}

bool MaterialAsset::SetVector(std::string_view name, const Eigen::Vector4f& value) noexcept {
    return MutateParameters([&](MaterialParameterStorage& storage) { return storage.SetVector(name, value); });
}

bool MaterialAsset::SetConstantBuffer(std::string_view name, std::span<const byte> data) noexcept {
    return MutateParameters([&](MaterialParameterStorage& storage) { return storage.SetConstantBuffer(name, data); });
}

bool MaterialAsset::SetTexture(
    std::string_view name,
    StreamingAssetRef<TextureAsset> texture,
    const TextureSubViewDesc& view) noexcept {
    return MutateParameters([&](MaterialParameterStorage& storage) {
        return storage.SetTexture(name, std::move(texture), view);
    });
}

bool MaterialAsset::ClearTexture(std::string_view name) noexcept {
    return MutateParameters([&](MaterialParameterStorage& storage) { return storage.ClearTexture(name); });
}

bool MaterialAsset::SetSampler(
    std::string_view name,
    const render::SamplerDescriptor& sampler) noexcept {
    return MutateParameters([&](MaterialParameterStorage& storage) { return storage.SetSampler(name, sampler); });
}

bool MaterialAsset::ClearSampler(std::string_view name) noexcept {
    return MutateParameters([&](MaterialParameterStorage& storage) { return storage.ClearSampler(name); });
}

}  // namespace radray
