#include <radray/runtime/upload_arena.h>

#include <algorithm>
#include <limits>

#include <radray/profiler.h>

namespace radray {

UploadArena::UploadArena(gpu::Device& device, uint64_t pageSize) noexcept
    : _device(device), _pageSize(pageSize) {}
UploadArena::~UploadArena() noexcept { Clear(); }

void UploadArena::Reset() noexcept {
    for (auto& page : _pages) {
        page.Cursor = page.DirtyBegin = page.DirtyEnd = 0;
        page.Reads = {};
    }
}

void UploadArena::Clear() noexcept {
    for (auto& page : _pages) page.Buffer->Unmap();
    _pages.clear();
}

std::optional<UploadAllocation> UploadArena::Allocate(uint64_t size, uint64_t alignment, uint64_t reservedSize, gpu::BufferStates reads) {
    return AllocatePageSlice(PageKind::Dynamic, size, alignment, reservedSize, reads);
}

std::optional<UploadAllocation> UploadArena::AllocateStaging(uint64_t size, uint64_t alignment) {
    return AllocatePageSlice(PageKind::Staging, size, alignment, size, gpu::BufferState::CopySource);
}

std::optional<UploadAllocation> UploadArena::AllocatePageSlice(PageKind kind, uint64_t size, uint64_t alignment, uint64_t reservedSize, gpu::BufferStates reads) {
    RADRAY_PROFILE_SCOPE_N("RadRay.UploadArena.Allocate");
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
        if (page.Kind != kind) continue;
        if (auto result = allocate(page)) return result;
    }
    const auto maximum = _device.GetCapabilities().Limits.MaxBufferSize;
    if (maximum && reservedSize > maximum) return std::nullopt;
    const auto capacity = std::max(maximum ? std::min(_pageSize, maximum) : _pageSize, reservedSize);
    if (!capacity || capacity > std::numeric_limits<size_t>::max()) return std::nullopt;
    constexpr auto kStagingUsage = gpu::BufferUse::MapWrite | gpu::BufferUse::CopySource;
    constexpr auto kDynamicUsage = kStagingUsage | gpu::BufferUse::Vertex | gpu::BufferUse::Index |
                                   gpu::BufferUse::CBuffer | gpu::BufferUse::Resource | gpu::BufferUse::Indirect;
    auto buffer = _device.CreateBuffer({.Size = capacity,
                                        .Memory = gpu::MemoryType::Upload,
                                        .Usage = kind == PageKind::Staging ? kStagingUsage : kDynamicUsage,
                                        .Hints = gpu::ResourceHint::PersistentMap});
    if (!buffer) return std::nullopt;
    auto* data = static_cast<byte*>(buffer->Map(0, capacity));
    if (!data) return std::nullopt;
    _pages.push_back({buffer.Release(), data, capacity, kind});
    return allocate(_pages.back());
}

bool UploadArena::Flush() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.UploadArena.Flush");
    for (auto& page : _pages) {
        if (page.DirtyEnd > page.Capacity) return false;
        if (page.DirtyEnd > page.DirtyBegin) page.Buffer->FlushMappedRange({page.DirtyBegin, page.DirtyEnd - page.DirtyBegin});
    }
    return true;
}

void UploadArena::CommitSubmittedStates() noexcept {
    for (auto& page : _pages) {
        if (page.Cursor) page.SubmittedState = page.Reads;
    }
}

gpu::Device& UploadArena::GetDevice() const noexcept { return _device; }

uint64_t UploadArena::GetCapacity() const noexcept {
    uint64_t result = 0;
    for (const auto& page : _pages) result += page.Capacity;
    return result;
}

}  // namespace radray
