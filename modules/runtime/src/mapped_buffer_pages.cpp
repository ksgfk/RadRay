#include <radray/runtime/mapped_buffer_pages.h>

#include <algorithm>
#include <limits>

#include <radray/profiler.h>
#include <radray/runtime/gpu_resource.h>

namespace radray::detail {

MappedBufferPages::MappedBufferPages(gpu::Device& device, uint64_t pageSize, gpu::BufferUses usage) noexcept
    : _device(device), _pageSize(pageSize), _usage(usage) {}

void MappedBufferPages::BeginFrame() noexcept {
    for (auto& page : _pages) {
        page.IdleFrames = page.Cursor ? 0 : static_cast<uint8_t>(page.IdleFrames + 1);
        page.Cursor = page.DirtyBegin = page.DirtyEnd = 0;
        page.Reads = {};
    }
    std::erase_if(_pages, [](const Page& page) { return page.IdleFrames >= 8; });
}

void MappedBufferPages::Clear() noexcept {
    _pages.clear();
}

std::optional<UploadAllocation> MappedBufferPages::AllocateInDomain(gpu::MemoryType memory, uint64_t size, uint64_t alignment,
                                                                    uint64_t reservedSize, gpu::BufferStates reads) {
    RADRAY_PROFILE_SCOPE_N("RadRay.MappedBufferPages.Allocate");
    if (!size) return UploadAllocation{};
    constexpr auto allowed = gpu::BufferState::CopySource | gpu::BufferState::Vertex | gpu::BufferState::Index |
                             gpu::BufferState::CBuffer | gpu::BufferState::ShaderRead | gpu::BufferState::Indirect;
    if (!alignment || reservedSize < size || reservedSize > std::numeric_limits<size_t>::max() || !reads || (reads & ~allowed)) return std::nullopt;
    const auto allocate = [&](Page& page) -> std::optional<UploadAllocation> {
        const auto padding = (alignment - page.Cursor % alignment) % alignment;
        const auto capacity = page.Capacity;
        if (padding > capacity - page.Cursor || reservedSize > capacity - page.Cursor - padding) return std::nullopt;
        const auto offset = page.Cursor + padding;
        if (!page.Cursor) page.DirtyBegin = offset;
        page.Cursor = offset + reservedSize;
        page.DirtyEnd = offset + size;
        page.Reads |= reads;
        return UploadAllocation{page.Buffer.get(), page.Data + offset, offset, size, reservedSize};
    };
    for (auto& page : _pages) {
        if (page.Memory != memory) continue;
        if (auto result = allocate(page)) return result;
    }
    const auto maximum = _device.GetCapabilities().Limits.MaxBufferSize;
    if (maximum && reservedSize > maximum) return std::nullopt;
    const auto capacity = std::max(maximum ? std::min(_pageSize, maximum) : _pageSize, reservedSize);
    if (!capacity || capacity > std::numeric_limits<size_t>::max()) return std::nullopt;
    if (memory == gpu::MemoryType::DeviceUpload && !CanAllocateDeviceUpload(_device, capacity)) return std::nullopt;
    auto buffer = _device.CreateBuffer({.Size = capacity, .Memory = memory, .Usage = _usage});
    if (!buffer) return std::nullopt;
    auto* data = static_cast<byte*>(buffer->GetMappedData().Get());
    if (!data) return std::nullopt;
    _pages.push_back({buffer.Release(), data, capacity, memory});
    return allocate(_pages.back());
}

bool MappedBufferPages::Flush() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.MappedBufferPages.Flush");
    for (auto& page : _pages) {
        if (page.DirtyEnd > page.Capacity) return false;
        if (page.DirtyEnd > page.DirtyBegin && !page.Buffer->FlushMappedRange({page.DirtyBegin, page.DirtyEnd - page.DirtyBegin})) return false;
    }
    return true;
}

void MappedBufferPages::CommitSubmittedStates() noexcept {
    for (auto& page : _pages) {
        if (page.Cursor) page.SubmittedState = page.Reads;
    }
}

gpu::Device& MappedBufferPages::GetDevice() const noexcept { return _device; }

uint64_t MappedBufferPages::GetCapacity() const noexcept {
    uint64_t result = 0;
    for (const auto& page : _pages) result += page.Capacity;
    return result;
}

}  // namespace radray::detail
