#pragma once

// 缓冲池、上传与帧所有权契约见 docs/architecture/runtime.md。

#include <optional>
#include <span>
#include <string_view>

#include <radray/allocator.h>
#include <radray/runtime/rdg.h>
#include <radray/runtime/vertex_input.h>

namespace radray {

class VertexBufferPool;
class IndexBufferPool;

// 可为空、仅可移动的借用区间；所属池必须晚于它销毁。
// 仅在相关 GPU 工作完成且所有引用它的命令 epoch 重置后，才可释放。
class VertexBufferSubAllocation final {
public:
    VertexBufferSubAllocation() noexcept = default;
    ~VertexBufferSubAllocation() noexcept;
    VertexBufferSubAllocation(VertexBufferSubAllocation&& other) noexcept;
    VertexBufferSubAllocation& operator=(VertexBufferSubAllocation&& other) noexcept;
    VertexBufferSubAllocation(const VertexBufferSubAllocation&) = delete;
    VertexBufferSubAllocation& operator=(const VertexBufferSubAllocation&) = delete;

    explicit operator bool() const noexcept;
    const VertexInput& GetVertexInput() const noexcept;
    render::Buffer* GetBuffer() const noexcept;
    uint32_t GetVertexStride() const noexcept;
    uint32_t GetFirstVertex() const noexcept;
    uint32_t GetVertexCount() const noexcept;
    uint64_t GetByteOffset() const noexcept;
    uint64_t GetByteSize() const noexcept;

private:
    friend class VertexBufferPool;
    VertexBufferSubAllocation(VertexBufferPool& pool, uint32_t page, BestFitAllocator::Allocation allocation) noexcept;
    void Reset() noexcept;
    Nullable<VertexBufferPool*> _pool{nullptr};
    uint32_t _page{0};
    BestFitAllocator::Allocation _allocation{};
};

class IndexBufferSubAllocation final {
public:
    IndexBufferSubAllocation() noexcept = default;
    ~IndexBufferSubAllocation() noexcept;
    IndexBufferSubAllocation(IndexBufferSubAllocation&& other) noexcept;
    IndexBufferSubAllocation& operator=(IndexBufferSubAllocation&& other) noexcept;
    IndexBufferSubAllocation(const IndexBufferSubAllocation&) = delete;
    IndexBufferSubAllocation& operator=(const IndexBufferSubAllocation&) = delete;

    explicit operator bool() const noexcept;
    render::Buffer* GetBuffer() const noexcept;
    uint32_t GetIndexStride() const noexcept;
    uint32_t GetFirstIndex() const noexcept;
    uint32_t GetIndexCount() const noexcept;
    uint64_t GetByteOffset() const noexcept;
    uint64_t GetByteSize() const noexcept;
    // 返回整页视图，配合 GetFirstIndex() 使用；同页绘制可兼容同一次索引缓冲绑定。
    render::IndexBufferView GetIndexBufferView() const noexcept;

private:
    friend class IndexBufferPool;
    IndexBufferSubAllocation(IndexBufferPool& pool, uint32_t page, BestFitAllocator::Allocation allocation) noexcept;
    void Reset() noexcept;
    Nullable<IndexBufferPool*> _pool{nullptr};
    uint32_t _page{0};
    BestFitAllocator::Allocation _allocation{};
};

struct VertexBufferPoolDescriptor {
    uint32_t VertexStride{0};
    uint32_t VertexStreamBinding{0};
    uint32_t PageVertexCapacity{150'000};
    render::BufferUses AdditionalUsage{};
    std::span<const render::VertexBufferLayout> Layouts{};
    std::span<const VertexInputAttribute> Attributes{};
    std::string_view DebugNamePrefix{"VertexBufferPool"};
};

struct IndexBufferPoolDescriptor {
    uint32_t IndexStride{4};
    uint32_t PageIndexCapacity{300'000};
    render::BufferUses AdditionalUsage{};
    std::string_view DebugNamePrefix{"IndexBufferPool"};
};

// 池借用设备，保留全部页面直到自身销毁；所有调用必须串行。
class VertexBufferPool final {
public:
    static Nullable<unique_ptr<VertexBufferPool>> Create(render::Device& device, const VertexBufferPoolDescriptor& desc);
    ~VertexBufferPool() noexcept;
    VertexBufferPool(const VertexBufferPool&) = delete;
    VertexBufferPool& operator=(const VertexBufferPool&) = delete;

    std::optional<VertexBufferSubAllocation> Allocate(uint32_t vertexCount);
    uint32_t GetPageCount() const noexcept;
    uint64_t GetFreeBytes() const noexcept;
    // 以 Common → Common 边界导入全部页面，包括保留的空页面。
    void AddPassReadAccess(RDGBuilder& graph, RDGRasterPassBuilder& pass, render::BufferStates state = render::BufferState::Vertex) const;

private:
    friend class VertexBufferSubAllocation;
    class Impl;
    explicit VertexBufferPool(unique_ptr<Impl> impl) noexcept;
    unique_ptr<Impl> _impl;
};

class IndexBufferPool final {
public:
    static Nullable<unique_ptr<IndexBufferPool>> Create(render::Device& device, const IndexBufferPoolDescriptor& desc = {});
    ~IndexBufferPool() noexcept;
    IndexBufferPool(const IndexBufferPool&) = delete;
    IndexBufferPool& operator=(const IndexBufferPool&) = delete;

    std::optional<IndexBufferSubAllocation> Allocate(uint32_t indexCount);
    uint32_t GetPageCount() const noexcept;
    uint64_t GetFreeBytes() const noexcept;
    void AddPassReadAccess(RDGBuilder& graph, RDGRasterPassBuilder& pass, render::BufferStates state = render::BufferState::Index) const;

private:
    friend class IndexBufferSubAllocation;
    class Impl;
    explicit IndexBufferPool(unique_ptr<Impl> impl) noexcept;
    unique_ptr<Impl> _impl;
};

}  // namespace radray
