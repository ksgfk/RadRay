#include <radray/runtime/shader_parameter_set.h>

#include <algorithm>

namespace radray {

void ShaderParameterSet::SetConstant(
    std::string_view name,
    std::span<const byte> bytes) {
    auto it = std::ranges::find(_constants, name, &ConstantEntry::Name);
    if (it != _constants.end()) {
        if (std::ranges::equal(it->Bytes, bytes)) {
            return;
        }
        it->Bytes.assign(bytes.begin(), bytes.end());
    } else {
        _constants.push_back(ConstantEntry{
            .Name = string{name},
            .Bytes = vector<byte>{bytes.begin(), bytes.end()}});
    }
    ++_revision;
}

void ShaderParameterSet::SetResource(
    std::string_view name,
    render::ResourceView* resource) noexcept {
    auto it = std::ranges::find(_resources, name, &ResourceEntry::Name);
    if (it != _resources.end()) {
        if (it->Value == resource) {
            return;
        }
        it->Value = resource;
    } else {
        _resources.push_back(ResourceEntry{.Name = string{name}, .Value = resource});
    }
    ++_revision;
}

void ShaderParameterSet::SetSampler(
    std::string_view name,
    render::Sampler* sampler) noexcept {
    auto it = std::ranges::find(_samplers, name, &SamplerEntry::Name);
    if (it != _samplers.end()) {
        if (it->Value == sampler) {
            return;
        }
        it->Value = sampler;
    } else {
        _samplers.push_back(SamplerEntry{.Name = string{name}, .Value = sampler});
    }
    ++_revision;
}

void ShaderParameterSet::ClearConstant(std::string_view name) noexcept {
    const auto oldSize = _constants.size();
    std::erase_if(_constants, [&](const ConstantEntry& entry) { return entry.Name == name; });
    if (_constants.size() != oldSize) {
        ++_revision;
    }
}

void ShaderParameterSet::ClearResource(std::string_view name) noexcept {
    const auto oldSize = _resources.size();
    std::erase_if(_resources, [&](const ResourceEntry& entry) { return entry.Name == name; });
    if (_resources.size() != oldSize) {
        ++_revision;
    }
}

void ShaderParameterSet::ClearSampler(std::string_view name) noexcept {
    const auto oldSize = _samplers.size();
    std::erase_if(_samplers, [&](const SamplerEntry& entry) { return entry.Name == name; });
    if (_samplers.size() != oldSize) {
        ++_revision;
    }
}

void ShaderParameterSet::Clear() noexcept {
    if (_constants.empty() && _resources.empty() && _samplers.empty()) {
        return;
    }
    _constants.clear();
    _resources.clear();
    _samplers.clear();
    ++_revision;
}

Nullable<const ShaderParameterSet::ConstantEntry*> ShaderParameterSet::FindConstant(
    std::string_view name) const noexcept {
    auto it = std::ranges::find(_constants, name, &ConstantEntry::Name);
    return it != _constants.end() ? &*it : nullptr;
}

Nullable<render::ResourceView*> ShaderParameterSet::FindResource(
    std::string_view name) const noexcept {
    auto it = std::ranges::find(_resources, name, &ResourceEntry::Name);
    return it != _resources.end() ? it->Value : nullptr;
}

Nullable<render::Sampler*> ShaderParameterSet::FindSampler(
    std::string_view name) const noexcept {
    auto it = std::ranges::find(_samplers, name, &SamplerEntry::Name);
    return it != _samplers.end() ? it->Value : nullptr;
}

}  // namespace radray
