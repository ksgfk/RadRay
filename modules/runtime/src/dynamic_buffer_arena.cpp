#include <radray/runtime/dynamic_buffer_arena.h>

namespace radray {

DynamicBufferArena::DynamicBufferArena(gpu::Device& device, uint64_t pageSize) noexcept
    : MappedBufferPages(device, pageSize, gpu::BufferUse::MapWrite | gpu::BufferUse::CopySource | gpu::BufferUse::Vertex | gpu::BufferUse::Index | gpu::BufferUse::CBuffer | gpu::BufferUse::Resource | gpu::BufferUse::Indirect) {}

std::optional<UploadAllocation> DynamicBufferArena::Allocate(uint64_t size, uint64_t alignment, uint64_t reservedSize, gpu::BufferStates reads,
                                                             DynamicBufferResidency residency) {
    if (residency == DynamicBufferResidency::PreferDevice) {
        if (auto slice = AllocateInDomain(gpu::MemoryType::DeviceUpload, size, alignment, reservedSize, reads)) return slice;
    }
    return AllocateInDomain(gpu::MemoryType::HostUpload, size, alignment, reservedSize, reads);
}

}  // namespace radray
