#pragma once

#include <optional>
#include <utility>

#include <radray/types.h>
#include <radray/gpu/types.h>

namespace radray::gpu {

std::optional<AttachmentSignature> ValidateRenderScope(const RenderPassBeginDescriptor& desc, Device& device) noexcept;
bool ValidateVertexInputState(const VertexInputState& state) noexcept;
std::pair<bool, string> ValidateTextureUploadRegion(const TextureDescriptor& destination, const TextureRegion2D& target);
std::pair<bool, string> ValidateBufferTextureCopyRegion(
    const BufferDescriptor& source, const TextureDescriptor& destination,
    const BufferTextureCopyRegion& r, const DeviceDetail& detail);
std::pair<bool, string> ValidateTextureReadbackRegion(const TextureDescriptor& source, const TextureRegion2D& region);
std::pair<bool, string> ValidateTextureReadbackRegion(
    const TextureDescriptor& source, const BufferDescriptor& destination,
    const TextureBufferCopyRegion& region, const DeviceDetail& detail);
bool IsValidTextureSupportQuery(const TextureSupportQuery& query) noexcept;
std::pair<bool, string> ValidateTextureDescriptor(
    const TextureDescriptor& desc, const GpuDeviceCapabilities& capabilities, const TextureSupport& support);
std::pair<bool, string> ValidateTextureDescriptor(const TextureDescriptor& desc, const Device& device);

}  // namespace radray::gpu
