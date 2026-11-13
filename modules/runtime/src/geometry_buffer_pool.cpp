#include <radray/runtime/geometry_buffer_pool.h>

#include <algorithm>
#include <limits>
#include <utility>

#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/mesh_draw_command.h>

namespace radray {
namespace {

struct GeometryPage {
    GeometryPage(IntrusivePtr<GpuBuffer> buffer, uint32_t capacity)
        : Buffer(std::move(buffer)), Allocator(capacity) {}
    IntrusivePtr<GpuBuffer> Buffer;
    BestFitAllocator Allocator;
    VertexInput Input;
};

struct GeometryPoolStorage {
    GeometryPoolStorage(render::Device& device, uint32_t stride, uint32_t capacity, render::BufferUses usage, std::string_view name)
        : Device(device), Stride(stride), PageCapacity(capacity), Usage(usage), Name(name) {}
    ~GeometryPoolStorage() noexcept {
        for (const auto& page : Pages) {
            RADRAY_ASSERT(page->Allocator.GetFreeCount() == page->Allocator.GetCapacity());
            (void)page;
        }
    }
    render::Device& Device;
    uint32_t Stride;
    uint32_t PageCapacity;
    render::BufferUses Usage;
    string Name;
    vector<unique_ptr<GeometryPage>> Pages;
};

bool ValidAdditionalUsage(render::BufferUses usage) noexcept {
    constexpr auto kDeviceUsage = render::BufferUse::Common | render::BufferUse::CopySource | render::BufferUse::CopyDestination |
                                  render::BufferUse::Vertex | render::BufferUse::Index | render::BufferUse::CBuffer |
                                  render::BufferUse::Resource | render::BufferUse::UnorderedAccess | render::BufferUse::Indirect;
    return !(usage & ~kDeviceUsage);
}

std::optional<std::pair<uint32_t, BestFitAllocator::Allocation>> AllocateRange(GeometryPoolStorage& storage, uint32_t count, Nullable<const VertexInput*> input) {
    if (!count) return std::nullopt;
    for (uint32_t index = 0; index < storage.Pages.size(); ++index) {
        if (const auto allocation = storage.Pages[index]->Allocator.Allocate(count)) return std::pair{index, *allocation};
    }
    const auto capacity = std::max(storage.PageCapacity, count);
    const uint64_t bytes = uint64_t{capacity} * storage.Stride;
    if ((input && bytes > std::numeric_limits<uint32_t>::max()) || storage.Pages.size() >= std::numeric_limits<uint32_t>::max()) return std::nullopt;
    auto buffer = GpuBuffer::Create(storage.Device, {.Size = bytes, .Usage = storage.Usage});
    if (!buffer) return std::nullopt;
    buffer->GetBuffer()->SetDebugName(radray::format("{}[{}]", storage.Name, storage.Pages.size()));
    auto page = make_unique<GeometryPage>(buffer.Release(), capacity);
    if (input) {
        page->Input = *input;
        const auto vertex = std::find_if(input->Layouts.begin(), input->Layouts.end(), [](const auto& layout) { return layout.StepMode == render::VertexStepMode::Vertex; });
        page->Input.Streams = {{page->Buffer->GetBuffer(), 0, bytes, vertex->Binding}};
        if (!ValidateVertexInput(page->Input, true)) return std::nullopt;
    }
    const auto allocation = page->Allocator.Allocate(count);
    const auto index = static_cast<uint32_t>(storage.Pages.size());
    storage.Pages.push_back(std::move(page));
    return std::pair{index, *allocation};
}

uint64_t FreeBytes(const GeometryPoolStorage& storage) noexcept {
    uint64_t bytes = 0;
    for (const auto& page : storage.Pages) bytes += uint64_t{page->Allocator.GetFreeCount()} * storage.Stride;
    return bytes;
}

void AddReadAccess(const GeometryPoolStorage& storage, RDGBuilder& graph, RDGRasterPassBuilder& pass, render::BufferStates state) {
    for (const auto& page : storage.Pages) {
        pass.ReadBuffer(ImportPersistentOwned(graph, page->Buffer), state);
    }
}

}  // namespace

class VertexBufferPool::Impl : public GeometryPoolStorage {
public:
    Impl(render::Device& device, const VertexBufferPoolDescriptor& desc)
        : GeometryPoolStorage(device, desc.VertexStride, desc.PageVertexCapacity, render::BufferUse::Vertex | render::BufferUse::CopyDestination | desc.AdditionalUsage, desc.DebugNamePrefix),
          Input{{desc.Layouts.begin(), desc.Layouts.end()}, {desc.Attributes.begin(), desc.Attributes.end()}, {}} {}
    VertexInput Input;
};

class IndexBufferPool::Impl : public GeometryPoolStorage {
public:
    Impl(render::Device& device, const IndexBufferPoolDescriptor& desc)
        : GeometryPoolStorage(device, desc.IndexStride, desc.PageIndexCapacity, render::BufferUse::Index | render::BufferUse::CopyDestination | desc.AdditionalUsage, desc.DebugNamePrefix) {}
};

Nullable<unique_ptr<VertexBufferPool>> VertexBufferPool::Create(render::Device& device, const VertexBufferPoolDescriptor& desc) {
    uint32_t vertices = 0, instances = 0;
    if (!desc.VertexStride || !desc.PageVertexCapacity || uint64_t{desc.VertexStride} * desc.PageVertexCapacity > std::numeric_limits<uint32_t>::max() || !ValidAdditionalUsage(desc.AdditionalUsage)) return nullptr;
    for (const auto& layout : desc.Layouts) {
        if (layout.StepMode == render::VertexStepMode::Vertex) {
            if (layout.Binding != desc.VertexStreamBinding || layout.ArrayStride != desc.VertexStride) return nullptr;
            ++vertices;
        } else if (layout.StepMode == render::VertexStepMode::Instance) {
            ++instances;
        } else {
            return nullptr;
        }
    }
    if (vertices != 1 || instances > 1) return nullptr;
    vector<render::VertexAttribute> attributes;
    attributes.reserve(desc.Attributes.size());
    for (const auto& attribute : desc.Attributes) {
        attributes.push_back({attribute.BufferBinding, attribute.Offset, attribute.Semantic, attribute.SemanticIndex, attribute.Format, attribute.Location});
    }
    if (!render::ValidateVertexInputState({desc.Layouts, attributes})) return nullptr;
    return unique_ptr<VertexBufferPool>{new VertexBufferPool{make_unique<Impl>(device, desc)}};
}

VertexBufferPool::VertexBufferPool(unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}
VertexBufferPool::~VertexBufferPool() noexcept = default;

std::optional<VertexBufferSubAllocation> VertexBufferPool::Allocate(uint32_t vertexCount) {
    RADRAY_PROFILE_SCOPE_N("RadRay.VertexBufferPool.Allocate");
    const auto range = AllocateRange(*_impl, vertexCount, &_impl->Input);
    if (!range) return std::nullopt;
    return VertexBufferSubAllocation{*this, range->first, range->second};
}

uint32_t VertexBufferPool::GetPageCount() const noexcept { return static_cast<uint32_t>(_impl->Pages.size()); }
uint64_t VertexBufferPool::GetFreeBytes() const noexcept { return FreeBytes(*_impl); }
void VertexBufferPool::AddPassReadAccess(RDGBuilder& graph, RDGRasterPassBuilder& pass, render::BufferStates state) const { AddReadAccess(*_impl, graph, pass, state); }

Nullable<unique_ptr<IndexBufferPool>> IndexBufferPool::Create(render::Device& device, const IndexBufferPoolDescriptor& desc) {
    if ((desc.IndexStride != 2 && desc.IndexStride != 4) || !desc.PageIndexCapacity || !ValidAdditionalUsage(desc.AdditionalUsage)) return nullptr;
    return unique_ptr<IndexBufferPool>{new IndexBufferPool{make_unique<Impl>(device, desc)}};
}

IndexBufferPool::IndexBufferPool(unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}
IndexBufferPool::~IndexBufferPool() noexcept = default;

std::optional<IndexBufferSubAllocation> IndexBufferPool::Allocate(uint32_t indexCount) {
    RADRAY_PROFILE_SCOPE_N("RadRay.IndexBufferPool.Allocate");
    const auto range = AllocateRange(*_impl, indexCount, nullptr);
    if (!range) return std::nullopt;
    return IndexBufferSubAllocation{*this, range->first, range->second};
}

uint32_t IndexBufferPool::GetPageCount() const noexcept { return static_cast<uint32_t>(_impl->Pages.size()); }
uint64_t IndexBufferPool::GetFreeBytes() const noexcept { return FreeBytes(*_impl); }
void IndexBufferPool::AddPassReadAccess(RDGBuilder& graph, RDGRasterPassBuilder& pass, render::BufferStates state) const { AddReadAccess(*_impl, graph, pass, state); }

VertexBufferSubAllocation::VertexBufferSubAllocation(VertexBufferPool& pool, uint32_t page, BestFitAllocator::Allocation allocation) noexcept
    : _pool(&pool), _page(page), _allocation(allocation) {}
VertexBufferSubAllocation::~VertexBufferSubAllocation() noexcept { Reset(); }
VertexBufferSubAllocation::VertexBufferSubAllocation(VertexBufferSubAllocation&& other) noexcept
    : _pool(std::exchange(other._pool, nullptr)), _page(other._page), _allocation(other._allocation) {}
VertexBufferSubAllocation& VertexBufferSubAllocation::operator=(VertexBufferSubAllocation&& other) noexcept {
    if (this != &other) {
        Reset();
        _pool = std::exchange(other._pool, nullptr);
        _page = other._page;
        _allocation = other._allocation;
    }
    return *this;
}
void VertexBufferSubAllocation::Reset() noexcept {
    if (_pool) _pool->_impl->Pages[_page]->Allocator.Destroy(_allocation);
    _pool = nullptr;
}
VertexBufferSubAllocation::operator bool() const noexcept { return bool{_pool}; }
const VertexInput& VertexBufferSubAllocation::GetVertexInput() const noexcept { return _pool->_impl->Pages[_page]->Input; }
render::Buffer* VertexBufferSubAllocation::GetBuffer() const noexcept { return _pool->_impl->Pages[_page]->Buffer->GetBuffer(); }
IntrusivePtr<GpuBuffer> VertexBufferSubAllocation::GetGpuBuffer() const noexcept { return _pool->_impl->Pages[_page]->Buffer; }
uint32_t VertexBufferSubAllocation::GetVertexStride() const noexcept { return _pool->_impl->Stride; }
uint32_t VertexBufferSubAllocation::GetFirstVertex() const noexcept { return _allocation.Offset; }
uint32_t VertexBufferSubAllocation::GetVertexCount() const noexcept { return _allocation.Count; }
uint64_t VertexBufferSubAllocation::GetByteOffset() const noexcept { return uint64_t{_allocation.Offset} * GetVertexStride(); }
uint64_t VertexBufferSubAllocation::GetByteSize() const noexcept { return uint64_t{_allocation.Count} * GetVertexStride(); }

IndexBufferSubAllocation::IndexBufferSubAllocation(IndexBufferPool& pool, uint32_t page, BestFitAllocator::Allocation allocation) noexcept
    : _pool(&pool), _page(page), _allocation(allocation) {}
IndexBufferSubAllocation::~IndexBufferSubAllocation() noexcept { Reset(); }
IndexBufferSubAllocation::IndexBufferSubAllocation(IndexBufferSubAllocation&& other) noexcept
    : _pool(std::exchange(other._pool, nullptr)), _page(other._page), _allocation(other._allocation) {}
IndexBufferSubAllocation& IndexBufferSubAllocation::operator=(IndexBufferSubAllocation&& other) noexcept {
    if (this != &other) {
        Reset();
        _pool = std::exchange(other._pool, nullptr);
        _page = other._page;
        _allocation = other._allocation;
    }
    return *this;
}
void IndexBufferSubAllocation::Reset() noexcept {
    if (_pool) _pool->_impl->Pages[_page]->Allocator.Destroy(_allocation);
    _pool = nullptr;
}
IndexBufferSubAllocation::operator bool() const noexcept { return bool{_pool}; }
render::Buffer* IndexBufferSubAllocation::GetBuffer() const noexcept { return _pool->_impl->Pages[_page]->Buffer->GetBuffer(); }
IntrusivePtr<GpuBuffer> IndexBufferSubAllocation::GetGpuBuffer() const noexcept { return _pool->_impl->Pages[_page]->Buffer; }
uint32_t IndexBufferSubAllocation::GetIndexStride() const noexcept { return _pool->_impl->Stride; }
uint32_t IndexBufferSubAllocation::GetFirstIndex() const noexcept { return _allocation.Offset; }
uint32_t IndexBufferSubAllocation::GetIndexCount() const noexcept { return _allocation.Count; }
uint64_t IndexBufferSubAllocation::GetByteOffset() const noexcept { return uint64_t{_allocation.Offset} * GetIndexStride(); }
uint64_t IndexBufferSubAllocation::GetByteSize() const noexcept { return uint64_t{_allocation.Count} * GetIndexStride(); }
render::IndexBufferView IndexBufferSubAllocation::GetIndexBufferView() const noexcept { return {GetBuffer(), 0, GetIndexStride()}; }

}  // namespace radray
