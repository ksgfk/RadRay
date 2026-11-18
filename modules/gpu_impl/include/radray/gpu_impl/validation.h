#pragma once

#include <optional>
#include <utility>

#include <radray/types.h>
#include <radray/gpu/types.h>

namespace radray::gpu {

std::optional<AttachmentSignature> ValidateRenderScope(const RenderPassBeginDescriptor& desc, Device& device) noexcept;
bool ValidateVertexInputState(const VertexInputState& state) noexcept;
std::pair<bool, string> ValidateBufferTextureCopyRegion(
    const BufferDescriptor& source, const TextureDescriptor& destination,
    const BufferTextureCopyRegion& r, const DeviceDetail& detail);
bool IsValidTextureSupportQuery(const TextureSupportQuery& query) noexcept;
std::pair<bool, string> ValidateTextureDescriptor(
    const TextureDescriptor& desc, const RenderDeviceCapabilities& capabilities, const TextureSupport& support);
std::pair<bool, string> ValidateTextureDescriptor(const TextureDescriptor& desc, const Device& device);

}  // namespace radray::gpu
