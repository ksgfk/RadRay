#pragma once

#include <cstdint>
#include <optional>

#include <radray/gpu/types.h>
#include <radray/runtime/mapped_buffer_pages.h>

namespace radray {

enum class DynamicBufferResidency : uint8_t { PreferDevice,
                                              HostOnly };

class DynamicBufferArena final : public detail::MappedBufferPages {
public:
    explicit DynamicBufferArena(gpu::Device& device, uint64_t pageSize = 1024 * 1024) noexcept;
    std::optional<UploadAllocation> Allocate(uint64_t size, uint64_t alignment, uint64_t reservedSize, gpu::BufferStates reads,
                                             DynamicBufferResidency residency = DynamicBufferResidency::PreferDevice);
};

}  // namespace radray
