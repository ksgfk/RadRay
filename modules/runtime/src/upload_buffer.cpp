#include <radray/runtime/upload_buffer.h>

#include <radray/logger.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

#include <radray/profiler.h>

namespace radray {

Nullable<unique_ptr<MappedUploadBuffer>> MappedUploadBuffer::Create(render::Device& device, uint64_t capacity, render::BufferUses usage) {
    RADRAY_PROFILE_SCOPE_N("RadRay.MappedUploadBuffer.Create");
    if (!capacity || capacity > std::numeric_limits<size_t>::max()) return nullptr;
    auto buffer = device.CreateBuffer({.Size = capacity, .Memory = render::MemoryType::Upload, .Usage = usage | render::BufferUse::MapWrite, .Hints = render::ResourceHint::PersistentMap});
    if (!buffer) return nullptr;
    auto* data = static_cast<byte*>(buffer->Map(0, capacity));
    if (!data) return nullptr;
    return unique_ptr<MappedUploadBuffer>{new MappedUploadBuffer{device, buffer.Release(), data, capacity, usage}};
}

MappedUploadBuffer::MappedUploadBuffer(render::Device& device, unique_ptr<render::Buffer> buffer, byte* data, uint64_t capacity, render::BufferUses usage) noexcept
    : _device(device), _buffer(std::move(buffer)), _data(data), _capacity(capacity), _usage(usage) {}
MappedUploadBuffer::~MappedUploadBuffer() noexcept { _buffer->Unmap(); }

bool MappedUploadBuffer::Reserve(render::Device& device, uint64_t minCapacity) {
    if (&device != &_device) return false;
    if (minCapacity <= _capacity) return true;
    if (minCapacity > (uint64_t{1} << 63)) return false;
    RADRAY_PROFILE_SCOPE_N("RadRay.MappedUploadBuffer.Reserve");
    auto replacement = Create(device, std::bit_ceil(minCapacity), _usage);
    if (!replacement) return false;
    _buffer.swap(replacement->_buffer);
    std::swap(_data, replacement->_data);
    std::swap(_capacity, replacement->_capacity);
    return true;
}

bool MappedUploadBuffer::Flush(uint64_t offset, uint64_t byteSize) noexcept {
    if (offset > _capacity || byteSize > _capacity - offset) return false;
    if (byteSize) _buffer->FlushMappedRange({offset, byteSize});
    return true;
}
render::Buffer* MappedUploadBuffer::Get() const noexcept { return _buffer.get(); }
byte* MappedUploadBuffer::GetData() const noexcept { return _data; }
uint64_t MappedUploadBuffer::GetCapacity() const noexcept { return _capacity; }

Nullable<unique_ptr<BatchedBufferUpload>> BatchedBufferUpload::Create(render::Device& device, uint64_t defaultPageSize) {
    if (!defaultPageSize || defaultPageSize > (uint64_t{1} << 63) || defaultPageSize > std::numeric_limits<size_t>::max()) return nullptr;
    return unique_ptr<BatchedBufferUpload>{new BatchedBufferUpload{device, defaultPageSize}};
}
BatchedBufferUpload::BatchedBufferUpload(render::Device& device, uint64_t defaultPageSize) noexcept
    : _device(device), _defaultPageSize(defaultPageSize) {}
BatchedBufferUpload::~BatchedBufferUpload() noexcept = default;

Nullable<byte*> BatchedBufferUpload::Acquire(render::Buffer* dstBuffer, uint64_t dstOffset, uint64_t byteSize, uint64_t alignment) {
    RADRAY_PROFILE_SCOPE_N("RadRay.BatchedBufferUpload.Acquire");
    if (_committed || !byteSize || !std::has_single_bit(alignment) || byteSize > (uint64_t{1} << 63) || !dstBuffer->IsValid()) return nullptr;
    const auto desc = dstBuffer->GetDesc();
    if (desc.Memory != render::MemoryType::Device || !desc.Usage.HasFlag(render::BufferUse::CopyDestination) || dstOffset > desc.Size || byteSize > desc.Size - dstOffset) return nullptr;
    std::optional<uint64_t> offset;
    for (size_t visited = 0; visited < _pages.size(); ++visited) {
        const auto index = static_cast<uint32_t>((size_t{_activePage} + visited) % _pages.size());
        const auto& page = _pages[index];
        if (page.Cursor > std::numeric_limits<uint64_t>::max() - (alignment - 1)) continue;
        const auto aligned = (page.Cursor + alignment - 1) & ~(alignment - 1);
        if (aligned <= page.Buffer->GetCapacity() && byteSize <= page.Buffer->GetCapacity() - aligned) {
            _activePage = index;
            offset = aligned;
            break;
        }
    }
    if (!offset) {
        if (_pages.size() >= std::numeric_limits<uint32_t>::max()) return nullptr;
        auto buffer = MappedUploadBuffer::Create(_device, std::max(_defaultPageSize, std::bit_ceil(byteSize)));
        if (!buffer) return nullptr;
        buffer->Get()->SetDebugName(radray::format("BatchedBufferUpload[{}]", _pages.size()));
        _activePage = static_cast<uint32_t>(_pages.size());
        _pages.push_back({buffer.Release(), 0});
        offset = 0;
    }
    auto& page = _pages[_activePage];
    _regions.push_back({_activePage, *offset, dstBuffer, dstOffset, byteSize});
    page.Cursor = *offset + byteSize;
    return page.Buffer->GetData() + *offset;
}

bool BatchedBufferUpload::Stage(render::Buffer* dstBuffer, uint64_t dstOffset, const void* data, uint64_t byteSize, uint64_t alignment) {
    auto destination = Acquire(dstBuffer, dstOffset, byteSize, alignment);
    if (!destination) return false;
    std::memcpy(destination.Get(), data, static_cast<size_t>(byteSize));
    return true;
}

bool BatchedBufferUpload::CommitToGraph(RDGBuilder& graph, std::string_view passName) {
    RADRAY_PROFILE_SCOPE_N("RadRay.BatchedBufferUpload.CommitToGraph");
    if (_committed) return false;
    _committed = true;
    if (_regions.empty()) return true;
    vector<RDGBufferHandle> pages(_pages.size());
    for (size_t index = 0; index < _pages.size(); ++index) {
        auto& page = _pages[index];
        if (!page.Cursor) continue;
        if (!page.Buffer->Flush(0, page.Cursor)) return false;
        pages[index] = graph.ImportBuffer(page.Buffer->Get(), render::BufferState::HostWrite, render::BufferState::HostWrite);
    }
    unordered_map<render::Buffer*, RDGBufferHandle> destinations;
    struct CopyRegion {
        RDGBufferHandle Src, Dst;
        uint64_t SrcOffset, DstOffset, ByteSize;
    };
    vector<CopyRegion> copies;
    copies.reserve(_regions.size());
    for (const auto& region : _regions) {
        auto [found, inserted] = destinations.try_emplace(region.DstBuffer);
        if (inserted) {
            found->second = graph.ImportBuffer(region.DstBuffer, render::BufferState::Common, render::BufferState::Common);
            graph.MarkOutput(found->second);
        }
        copies.push_back({pages[region.PageIndex], found->second, region.SrcOffset, region.DstOffset, region.ByteSize});
    }
    graph.AddCopyPass(passName, [&](RDGCopyPassBuilder& pass) {
        for (const auto page : pages) if (page.IsValid()) pass.ReadBuffer(page);
        for (const auto& [buffer, handle] : destinations) {
            (void)buffer;
            pass.WriteBuffer(handle);
        } }, [copies = std::move(copies)](RDGCopyPassContext& context) {
        for (const auto& copy : copies) context.GetCommandBuffer().CopyBufferToBuffer(&context.Resolve(copy.Dst), copy.DstOffset, &context.Resolve(copy.Src), copy.SrcOffset, copy.ByteSize);
        return true; });
    return true;
}

void BatchedBufferUpload::Reset() noexcept {
    _regions.clear();
    for (auto& page : _pages) page.Cursor = 0;
    _activePage = 0;
    _committed = false;
}
uint32_t BatchedBufferUpload::GetPageCount() const noexcept { return static_cast<uint32_t>(_pages.size()); }
uint64_t BatchedBufferUpload::GetCapacity() const noexcept {
    uint64_t capacity = 0;
    for (const auto& page : _pages) capacity += page.Buffer->GetCapacity();
    return capacity;
}
uint64_t BatchedBufferUpload::GetStagedByteSize() const noexcept {
    uint64_t bytes = 0;
    for (const auto& region : _regions) bytes += region.ByteSize;
    return bytes;
}

}  // namespace radray
