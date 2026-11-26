#pragma once

#include <cstdint>
#include <optional>

#include <radray/gpu/types.h>
#include <radray/runtime/mapped_buffer_pages.h>

namespace radray {

class StagingBufferPool final : public detail::MappedBufferPages {
public:
    explicit StagingBufferPool(gpu::Device& device, uint64_t pageSize = 1024 * 1024) noexcept;
    std::optional<UploadAllocation> Allocate(uint64_t size, uint64_t alignment);
};

}  // namespace radray
