#include <radray/runtime/staging_buffer_pool.h>

namespace radray {

StagingBufferPool::StagingBufferPool(gpu::Device& device, uint64_t pageSize) noexcept
    : MappedBufferPages(device, pageSize, gpu::BufferUse::MapWrite | gpu::BufferUse::CopySource) {}

std::optional<UploadAllocation> StagingBufferPool::Allocate(uint64_t size, uint64_t alignment) {
    return AllocateInDomain(gpu::MemoryType::HostUpload, size, alignment, size, gpu::BufferState::CopySource);
}

}  // namespace radray
