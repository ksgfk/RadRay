#include <radray/runtime/upload_arena.h>

#include <algorithm>
#include <limits>

#include <radray/profiler.h>

namespace radray {

UploadArena::UploadArena(render::Device& device, uint64_t pageSize) noexcept : _device(device), _pageSize(pageSize) {}
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

std::optional<UploadAllocation> UploadArena::Allocate(uint64_t size, uint64_t alignment, uint64_t reservedSize, render::BufferStates reads) {
    RADRAY_PROFILE_SCOPE_N("RadRay.UploadArena.Allocate");
    if (!size) return UploadAllocation{};
    constexpr auto allowed = render::BufferState::CopySource | render::BufferState::Vertex | render::BufferState::Index |
                             render::BufferState::CBuffer | render::BufferState::ShaderRead | render::BufferState::Indirect;
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
        if (auto result = allocate(page)) return result;
    }
    const auto maximum = _device.GetCapabilities().Limits.MaxBufferSize;
    if (maximum && reservedSize > maximum) return std::nullopt;
    const auto capacity = std::max(maximum ? std::min(_pageSize, maximum) : _pageSize, reservedSize);
    if (!capacity || capacity > std::numeric_limits<size_t>::max()) return std::nullopt;
    const auto usage = render::BufferUse::CopySource | render::BufferUse::Vertex | render::BufferUse::Index |
                       render::BufferUse::CBuffer | render::BufferUse::Resource | render::BufferUse::Indirect | render::BufferUse::MapWrite;
    auto buffer = _device.CreateBuffer({.Size = capacity, .Memory = render::MemoryType::Upload, .Usage = usage, .Hints = render::ResourceHint::PersistentMap});
    if (!buffer) return std::nullopt;
    auto* data = static_cast<byte*>(buffer->Map(0, capacity));
    if (!data) return std::nullopt;
    _pages.push_back({buffer.Release(), data, capacity});
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

uint64_t UploadArena::GetCapacity() const noexcept {
    uint64_t result = 0;
    for (const auto& page : _pages) result += page.Capacity;
    return result;
}

}  // namespace radray
