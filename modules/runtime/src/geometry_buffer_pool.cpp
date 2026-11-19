#include <radray/runtime/geometry_buffer_pool.h>

#include <radray/runtime/rdg.h>

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
    GeometryPoolStorage(gpu::Device& device, uint32_t stride, uint32_t capacity, gpu::BufferUses usage, std::string_view name)
        : Device(device), Stride(stride), PageCapacity(capacity), Usage(usage), Name(name) {}
    ~GeometryPoolStorage() noexcept {
#if RADRAY_IS_DEBUG
        for (const auto& page : Pages) {
            if (page) RADRAY_ASSERT(page->Allocator.GetFreeCount() == page->Allocator.GetCapacity());
        }
        ValidateTotals();
        RADRAY_ASSERT(TotalFreeElements == TotalCapacityElements);
#endif
    }
#if RADRAY_IS_DEBUG
    void ValidateTotals() const noexcept {
        uint32_t pages = 0;
        uint64_t capacity = 0, free = 0;
        for (const auto& page : Pages) {
            if (!page) continue;
            ++pages;
            capacity += page->Allocator.GetCapacity();
            free += page->Allocator.GetFreeCount();
        }
        RADRAY_ASSERT(pages == CommittedPageCount && capacity == TotalCapacityElements && free == TotalFreeElements);
    }
#endif
    gpu::Device& Device;
    uint32_t Stride;
    uint32_t PageCapacity;
    gpu::BufferUses Usage;
    string Name;
    vector<unique_ptr<GeometryPage>> Pages;
    uint32_t CommittedPageCount{0};
    uint64_t TotalCapacityElements{0};
    uint64_t TotalFreeElements{0};
};

bool ValidAdditionalUsage(gpu::BufferUses usage) noexcept {
    constexpr auto kDeviceUsage = gpu::BufferUse::Common | gpu::BufferUse::CopySource | gpu::BufferUse::CopyDestination |
                                  gpu::BufferUse::Vertex | gpu::BufferUse::Index | gpu::BufferUse::CBuffer |
                                  gpu::BufferUse::Resource | gpu::BufferUse::UnorderedAccess | gpu::BufferUse::Indirect;
    return !(usage & ~kDeviceUsage);
}

Nullable<unique_ptr<GeometryPage>> CreateCommittedPage(const GeometryPoolStorage& storage, uint32_t slot, uint32_t capacity, Nullable<const VertexInput*> input) {
    const uint64_t bytes = uint64_t{capacity} * storage.Stride;
    if (input && bytes > std::numeric_limits<uint32_t>::max()) return nullptr;
    auto buffer = GpuBuffer::Create(storage.Device, {.Size = bytes, .Usage = storage.Usage});
    if (!buffer) return nullptr;
    buffer->GetBuffer()->SetDebugName(radray::format("{}[{}]", storage.Name, slot));
    auto page = make_unique<GeometryPage>(buffer.Release(), capacity);
    if (input) {
        page->Input = *input;
        const auto vertex = std::find_if(input->Layouts.begin(), input->Layouts.end(), [](const auto& layout) { return layout.StepMode == gpu::VertexStepMode::Vertex; });
        page->Input.Streams = {{page->Buffer->GetBuffer(), 0, bytes, vertex->Binding}};
        if (!ValidateVertexInput(page->Input, true)) return nullptr;
    }
    return page;
}

std::optional<std::pair<uint32_t, BestFitAllocator::Allocation>> AllocateRange(GeometryPoolStorage& storage, uint32_t count, Nullable<const VertexInput*> input) {
    if (!count) return std::nullopt;
    size_t slot = storage.Pages.size();
    for (size_t i = 0; i < storage.Pages.size(); ++i) {
        const auto& page = storage.Pages[i];
        if (!page) {
            if (slot == storage.Pages.size()) slot = i;
            continue;
        }
        if (const auto allocation = page->Allocator.Allocate(count)) {
            RADRAY_ASSERT(storage.TotalFreeElements >= count);
            storage.TotalFreeElements -= count;
            return std::pair{static_cast<uint32_t>(i), *allocation};
        }
    }
    if (slot >= std::numeric_limits<uint32_t>::max()) return std::nullopt;
    const auto index = static_cast<uint32_t>(slot);
    const auto capacity = std::max(storage.PageCapacity, count);
    auto page = CreateCommittedPage(storage, index, capacity, input);
    if (!page) return std::nullopt;
    const auto allocation = page->Allocator.Allocate(count);
    RADRAY_ASSERT(allocation.has_value());
    if (slot == storage.Pages.size())
        storage.Pages.push_back(page.Release());
    else
        storage.Pages[slot] = page.Release();
    ++storage.CommittedPageCount;
    storage.TotalCapacityElements += capacity;
    storage.TotalFreeElements += capacity - count;
    return std::pair{index, *allocation};
}

uint32_t TrimPages(GeometryPoolStorage& storage) noexcept {
    const auto before = storage.CommittedPageCount;
    for (auto& page : storage.Pages) {
        if (!page || page->Allocator.GetFreeCount() != page->Allocator.GetCapacity()) continue;
        const auto capacity = page->Allocator.GetCapacity();
        storage.TotalFreeElements -= capacity;
        storage.TotalCapacityElements -= capacity;
        --storage.CommittedPageCount;
        page.reset();
    }
    while (!storage.Pages.empty() && !storage.Pages.back()) storage.Pages.pop_back();
    return before - storage.CommittedPageCount;
}

uint64_t FreeBytes(const GeometryPoolStorage& storage) noexcept {
#if RADRAY_IS_DEBUG
    storage.ValidateTotals();
#endif
    return storage.TotalFreeElements * storage.Stride;
}

void AddReadAccess(const GeometryPoolStorage& storage, RDGBuilder& graph, RDGRasterPassBuilder& pass, gpu::BufferStates state) {
    for (const auto& page : storage.Pages) {
        if (!page) continue;
        pass.ReadBuffer(ImportPersistentOwned(graph, page->Buffer), state);
    }
}

}  // namespace

class VertexBufferPool::Impl : public GeometryPoolStorage {
public:
    Impl(gpu::Device& device, const VertexBufferPoolDescriptor& desc)
        : GeometryPoolStorage(device, desc.VertexStride, desc.PageVertexCapacity, gpu::BufferUse::Vertex | gpu::BufferUse::CopyDestination | desc.AdditionalUsage, desc.DebugNamePrefix),
          Input{{desc.Layouts.begin(), desc.Layouts.end()}, {desc.Attributes.begin(), desc.Attributes.end()}, {}} {}
    VertexInput Input;
};

class IndexBufferPool::Impl : public GeometryPoolStorage {
public:
    Impl(gpu::Device& device, const IndexBufferPoolDescriptor& desc)
        : GeometryPoolStorage(device, desc.IndexStride, desc.PageIndexCapacity, gpu::BufferUse::Index | gpu::BufferUse::CopyDestination | desc.AdditionalUsage, desc.DebugNamePrefix) {}
};

Nullable<unique_ptr<VertexBufferPool>> VertexBufferPool::Create(gpu::Device& device, const VertexBufferPoolDescriptor& desc) {
    uint32_t vertices = 0, instances = 0;
    if (!desc.VertexStride || !desc.PageVertexCapacity || uint64_t{desc.VertexStride} * desc.PageVertexCapacity > std::numeric_limits<uint32_t>::max() || !ValidAdditionalUsage(desc.AdditionalUsage)) return nullptr;
    for (const auto& layout : desc.Layouts) {
        if (layout.StepMode == gpu::VertexStepMode::Vertex) {
            if (layout.Binding != desc.VertexStreamBinding || layout.ArrayStride != desc.VertexStride) return nullptr;
            ++vertices;
        } else if (layout.StepMode == gpu::VertexStepMode::Instance) {
            ++instances;
        } else {
            return nullptr;
        }
    }
    if (vertices != 1 || instances > 1) return nullptr;
    auto impl = make_unique<Impl>(device, desc);
    if (!ValidateVertexInputLayout(impl->Input)) return nullptr;
    return unique_ptr<VertexBufferPool>{new VertexBufferPool{std::move(impl)}};
}

VertexBufferPool::VertexBufferPool(unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}
VertexBufferPool::~VertexBufferPool() noexcept = default;

std::optional<VertexBufferSubAllocation> VertexBufferPool::Allocate(uint32_t vertexCount) {
    RADRAY_PROFILE_SCOPE_N("RadRay.VertexBufferPool.Allocate");
    const auto range = AllocateRange(*_impl, vertexCount, &_impl->Input);
    if (!range) return std::nullopt;
    return VertexBufferSubAllocation{*this, range->first, range->second};
}

uint32_t VertexBufferPool::GetPageCount() const noexcept { return _impl->CommittedPageCount; }
uint64_t VertexBufferPool::GetCapacityBytes() const noexcept { return _impl->TotalCapacityElements * _impl->Stride; }
uint64_t VertexBufferPool::GetAllocatedBytes() const noexcept { return (_impl->TotalCapacityElements - _impl->TotalFreeElements) * _impl->Stride; }
uint32_t VertexBufferPool::TrimEmptyPages() noexcept { return TrimPages(*_impl); }
uint64_t VertexBufferPool::GetFreeBytes() const noexcept { return FreeBytes(*_impl); }
void VertexBufferPool::AddPassReadAccess(RDGBuilder& graph, RDGRasterPassBuilder& pass, gpu::BufferStates state) const { AddReadAccess(*_impl, graph, pass, state); }

Nullable<unique_ptr<IndexBufferPool>> IndexBufferPool::Create(gpu::Device& device, const IndexBufferPoolDescriptor& desc) {
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

uint32_t IndexBufferPool::GetPageCount() const noexcept { return _impl->CommittedPageCount; }
uint64_t IndexBufferPool::GetCapacityBytes() const noexcept { return _impl->TotalCapacityElements * _impl->Stride; }
uint64_t IndexBufferPool::GetAllocatedBytes() const noexcept { return (_impl->TotalCapacityElements - _impl->TotalFreeElements) * _impl->Stride; }
uint32_t IndexBufferPool::TrimEmptyPages() noexcept { return TrimPages(*_impl); }
uint64_t IndexBufferPool::GetFreeBytes() const noexcept { return FreeBytes(*_impl); }
void IndexBufferPool::AddPassReadAccess(RDGBuilder& graph, RDGRasterPassBuilder& pass, gpu::BufferStates state) const { AddReadAccess(*_impl, graph, pass, state); }

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
    if (_pool) {
        _pool->_impl->Pages[_page]->Allocator.Destroy(_allocation);
        _pool->_impl->TotalFreeElements += _allocation.Count;
    }
    _pool = nullptr;
    _page = 0;
    _allocation = {};
}
uint32_t VertexBufferSubAllocation::GetPageIndex() const noexcept { return _pool ? _page : 0u; }
VertexBufferSubAllocation::operator bool() const noexcept { return bool{_pool}; }
const VertexInput& VertexBufferSubAllocation::GetVertexInput() const noexcept { return _pool->_impl->Pages[_page]->Input; }
gpu::Buffer* VertexBufferSubAllocation::GetBuffer() const noexcept { return _pool->_impl->Pages[_page]->Buffer->GetBuffer(); }
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
    if (_pool) {
        _pool->_impl->Pages[_page]->Allocator.Destroy(_allocation);
        _pool->_impl->TotalFreeElements += _allocation.Count;
    }
    _pool = nullptr;
    _page = 0;
    _allocation = {};
}
uint32_t IndexBufferSubAllocation::GetPageIndex() const noexcept { return _pool ? _page : 0u; }
IndexBufferSubAllocation::operator bool() const noexcept { return bool{_pool}; }
gpu::Buffer* IndexBufferSubAllocation::GetBuffer() const noexcept { return _pool->_impl->Pages[_page]->Buffer->GetBuffer(); }
IntrusivePtr<GpuBuffer> IndexBufferSubAllocation::GetGpuBuffer() const noexcept { return _pool->_impl->Pages[_page]->Buffer; }
uint32_t IndexBufferSubAllocation::GetIndexStride() const noexcept { return _pool->_impl->Stride; }
uint32_t IndexBufferSubAllocation::GetFirstIndex() const noexcept { return _allocation.Offset; }
uint32_t IndexBufferSubAllocation::GetIndexCount() const noexcept { return _allocation.Count; }
uint64_t IndexBufferSubAllocation::GetByteOffset() const noexcept { return uint64_t{_allocation.Offset} * GetIndexStride(); }
uint64_t IndexBufferSubAllocation::GetByteSize() const noexcept { return uint64_t{_allocation.Count} * GetIndexStride(); }
gpu::IndexBufferView IndexBufferSubAllocation::GetIndexBufferView() const noexcept { return {GetBuffer(), 0, GetIndexStride()}; }

}  // namespace radray
