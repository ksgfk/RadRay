#include <radray/runtime/material_instance.h>

#include <algorithm>

namespace radray {

MaterialInstance::MaterialInstance(Material* material) noexcept : _material(material) {
    if (_material == nullptr) {
        return;
    }
    // 克隆默认值存储模板,得到 per-instance 的可写参数副本。
    const std::optional<StructuredBufferStorage>& tmpl = _material->GetStorageTemplate();
    if (tmpl.has_value()) {
        _storage = tmpl;  // 值语义拷贝。
        _rootName = string{_material->GetParameterLayout().GetConstantBufferName()};
        BuildFieldHandleCache();
    }
}

void MaterialInstance::BuildFieldHandleCache() noexcept {
    _fieldHandles.clear();
    if (!_storage.has_value()) {
        return;
    }
    StructuredBufferReadOnlyView root = _storage->GetVar(_rootName);
    if (!root.IsValid()) {
        RADRAY_ERR_LOG("MaterialInstance: storage template has no root cbuffer '{}'", _rootName);
        return;
    }
    // 一次性解析根 cbuffer 的所有字段名 → 稳定 globalId,缓存供热路径复用。
    for (const StructuredBufferVariable& member : root.GetType().GetMembers()) {
        _fieldHandles.emplace(string{member.GetName()}, member.GetGlobalId());
    }
}

std::span<const byte> MaterialInstance::GetConstantData() const noexcept {
    if (!_storage.has_value()) {
        return {};
    }
    return _storage->GetData();
}

bool MaterialInstance::SetTexture(std::string_view name, StreamingAssetRefAny texture) noexcept {
    if (_material == nullptr) {
        return false;
    }
    // 仅接受参数布局中声明的贴图槽。
    const MaterialParameterLayout& layout = _material->GetParameterLayout();
    bool isKnownSlot = false;
    for (const auto& slot : layout.GetResourceSlots()) {
        if (slot.Kind == MaterialParameterLayout::ResourceKind::Texture && slot.Name == name) {
            isKnownSlot = true;
            break;
        }
    }
    if (!isKnownSlot) {
        return false;
    }
    auto it = std::find_if(_textures.begin(), _textures.end(), [&](const auto& kv) {
        return kv.first == name;
    });
    if (it != _textures.end()) {
        it->second = std::move(texture);
    } else {
        _textures.emplace_back(string{name}, std::move(texture));
    }
    return true;
}

StreamingAssetRefAny MaterialInstance::GetTexture(std::string_view name) const noexcept {
    auto it = std::find_if(_textures.begin(), _textures.end(), [&](const auto& kv) {
        return kv.first == name;
    });
    return it != _textures.end() ? it->second : StreamingAssetRefAny{};
}

}  // namespace radray
