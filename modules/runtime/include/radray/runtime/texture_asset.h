#pragma once

#include <optional>

#include <radray/intrusive_ptr.h>
#include <radray/gpu/types.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_resource.h>

namespace radray {

class TextureAsset final {
public:
    static std::optional<TextureAsset> Create(IntrusivePtr<GpuTexture> texture);
    AssetReloadDecision ClassifyReload(const TextureAsset& candidate) const;
    const IntrusivePtr<GpuTexture>& GetTexture() const noexcept;
    gpu::TextureView* GetDefaultView() const noexcept;

private:
    explicit TextureAsset(IntrusivePtr<GpuTexture> texture) noexcept;
    IntrusivePtr<GpuTexture> _texture;
};

}  // namespace radray
