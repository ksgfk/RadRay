#include <fmt/format.h>

#include <radray/runtime/persistent_resource_registry.h>

namespace radray::runtime {

PersistentResourceRegistry::PersistentResourceRegistry(render::Device* device) noexcept
    : _device(device) {}

void PersistentResourceRegistry::Reset(render::Device* device) noexcept {
    _device = device;
    _textures.clear();
    _textureByName.clear();
}

PersistentTextureHandle PersistentResourceRegistry::RegisterTexture(std::string_view name, const render::TextureDescriptor& desc) {
    const auto it = _textureByName.find(string{name});
    if (it != _textureByName.end()) {
        return it->second;
    }

    PersistentTextureRecord record{};
    record.Name = string{name};
    record.Desc = desc;
    const PersistentTextureHandle handle{static_cast<uint32_t>(_textures.size() + 1)};
    _textures.push_back(std::move(record));
    _textureByName.emplace(_textures.back().Name, handle);
    if (_device != nullptr) {
        this->RecreateTexture(_textures.back());
    }
    return handle;
}

bool PersistentResourceRegistry::ResizeTexture(PersistentTextureHandle handle, uint32_t width, uint32_t height) noexcept {
    auto record = this->ResolveTexture(handle);
    if (!record.HasValue()) {
        return false;
    }
    record.Get()->Desc.Width = width;
    record.Get()->Desc.Height = height;
    return this->RecreateTexture(*record.Get());
}

Nullable<const PersistentTextureRecord*> PersistentResourceRegistry::ResolveTexture(PersistentTextureHandle handle) const noexcept {
    if (!handle.IsValid()) {
        return nullptr;
    }
    const size_t index = static_cast<size_t>(handle.Value - 1);
    if (index >= _textures.size()) {
        return nullptr;
    }
    return &_textures[index];
}

Nullable<PersistentTextureRecord*> PersistentResourceRegistry::ResolveTexture(PersistentTextureHandle handle) noexcept {
    if (!handle.IsValid()) {
        return nullptr;
    }
    const size_t index = static_cast<size_t>(handle.Value - 1);
    if (index >= _textures.size()) {
        return nullptr;
    }
    return &_textures[index];
}

bool PersistentResourceRegistry::RecreateTexture(PersistentTextureRecord& record) noexcept {
    if (_device == nullptr) {
        return false;
    }

    auto textureOpt = _device->CreateTexture(record.Desc);
    if (!textureOpt.HasValue()) {
        return false;
    }
    auto texture = textureOpt.Release();
    texture->SetDebugName(record.Name);

    render::TextureViewDescriptor viewDesc{};
    viewDesc.Target = texture.get();
    viewDesc.Dim = record.Desc.Dim;
    viewDesc.Format = record.Desc.Format;
    viewDesc.Range = render::SubresourceRange{0, record.Desc.DepthOrArraySize, 0, record.Desc.MipLevels};
    viewDesc.Usage = static_cast<bool>(record.Desc.Usage & render::TextureUse::RenderTarget)
                         ? render::TextureViewUsage::RenderTarget
                         : render::TextureViewUsage::Resource;
    auto viewOpt = _device->CreateTextureView(viewDesc);
    if (!viewOpt.HasValue()) {
        return false;
    }
    auto view = viewOpt.Release();
    view->SetDebugName(fmt::format("{}_view", record.Name));

    record.TextureObject = std::move(texture);
    record.DefaultView = std::move(view);
    return true;
}

}  // namespace radray::runtime
