#include <radray/runtime/render_framework/material_property_block.h>

#include <cstring>
#include <utility>

namespace radray {

void MaterialPropertyBlock::Clear() noexcept {
    if (!_overrides.empty()) {
        _overrides.clear();
        ++_version;
    }
}

void MaterialPropertyBlock::SetFloat(std::string_view name, float value) noexcept {
    _overrides[string{name}] = value;
    ++_version;
}

void MaterialPropertyBlock::SetVector(std::string_view name, const Eigen::Vector4f& value) noexcept {
    _overrides[string{name}] = value;
    ++_version;
}

void MaterialPropertyBlock::SetConstantBlock(std::string_view name, const void* data, size_t size) noexcept {
    vector<byte> bytes(size);
    if (data != nullptr && size > 0) {
        std::memcpy(bytes.data(), data, size);
    }
    _overrides[string{name}] = std::move(bytes);
    ++_version;
}

void MaterialPropertyBlock::SetTexture(std::string_view name, StreamingAssetRef<TextureAsset> texture) noexcept {
    _overrides[string{name}] = std::move(texture);
    ++_version;
}

void MaterialPropertyBlock::SetTexture(std::string_view name, StreamingAssetRef<TextureAsset> texture, const TextureSubViewDesc& sub) noexcept {
    _overrides[string{name}] = TextureSubViewRef{std::move(texture), sub};
    ++_version;
}

void MaterialPropertyBlock::SetSampler(std::string_view name, const render::SamplerDescriptor& desc) noexcept {
    _overrides[string{name}] = desc;
    ++_version;
}

void MaterialPropertyBlock::ClearProperty(std::string_view name) noexcept {
    auto it = _overrides.find(string{name});
    if (it != _overrides.end()) {
        _overrides.erase(it);
        ++_version;
    }
}

std::optional<MaterialPropertyValue> MaterialPropertyBlock::GetOverride(std::string_view name) const noexcept {
    auto it = _overrides.find(string{name});
    if (it == _overrides.end()) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace radray
