#pragma once

#include <string_view>
#include <unordered_map>

#include <radray/nullable.h>
#include <radray/render/common.h>

namespace radray::runtime {

struct PersistentTextureHandle {
    uint32_t Value{0};

    constexpr bool IsValid() const noexcept { return Value != 0; }
    static constexpr PersistentTextureHandle Invalid() noexcept { return PersistentTextureHandle{}; }
    friend auto operator<=>(const PersistentTextureHandle& lhs, const PersistentTextureHandle& rhs) noexcept = default;
};

struct PersistentTextureRecord {
    string Name{};
    render::TextureDescriptor Desc{};
    unique_ptr<render::Texture> TextureObject{};
    unique_ptr<render::TextureView> DefaultView{};
};

class PersistentResourceRegistry {
public:
    explicit PersistentResourceRegistry(render::Device* device = nullptr) noexcept;

    void Reset(render::Device* device) noexcept;

    PersistentTextureHandle RegisterTexture(std::string_view name, const render::TextureDescriptor& desc);

    bool ResizeTexture(PersistentTextureHandle handle, uint32_t width, uint32_t height) noexcept;

    Nullable<const PersistentTextureRecord*> ResolveTexture(PersistentTextureHandle handle) const noexcept;

    Nullable<PersistentTextureRecord*> ResolveTexture(PersistentTextureHandle handle) noexcept;

private:
    bool RecreateTexture(PersistentTextureRecord& record) noexcept;

    render::Device* _device{nullptr};
    vector<PersistentTextureRecord> _textures{};
    std::unordered_map<string, PersistentTextureHandle> _textureByName{};
};

}  // namespace radray::runtime
