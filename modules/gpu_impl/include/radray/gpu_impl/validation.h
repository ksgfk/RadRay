#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include <radray/types.h>
#include <radray/gpu/types.h>

namespace radray::gpu {

// Attachment and resolve views must belong to the recording device.
std::optional<AttachmentSignature> ValidateRenderScope(const RenderPassBeginDescriptor& desc, Device& device) noexcept;
bool ValidateVertexInputState(const VertexInputState& state) noexcept;
std::pair<bool, std::string_view> ValidateTextureUploadRegion(const TextureDescriptor& destination, const TextureRegion2D& target);
std::pair<bool, std::string_view> ValidateBufferTextureCopyRegion(
    const BufferDescriptor& source, const TextureDescriptor& destination,
    const BufferTextureCopyRegion& r, const DeviceDetail& detail);
std::pair<bool, std::string_view> ValidateTextureReadbackRegion(const TextureDescriptor& source, const TextureRegion2D& region);
std::pair<bool, std::string_view> ValidateTextureReadbackRegion(
    const TextureDescriptor& source, const BufferDescriptor& destination,
    const TextureBufferCopyRegion& region, const DeviceDetail& detail);
bool IsValidTextureSupportQuery(const TextureSupportQuery& query) noexcept;
bool BufferRangeFits(uint64_t bufferSize, BufferRange range) noexcept;
bool IsBufferMemoryUsageValid(const BufferDescriptor& desc) noexcept;

std::pair<bool, string> ValidateTextureDescriptor(
    const TextureDescriptor& desc, const GpuDeviceCapabilities& capabilities, const TextureSupport& support);
std::pair<bool, string> ValidateTextureDescriptor(const TextureDescriptor& desc, const Device& device);

}  // namespace radray::gpu
