#pragma once

// 缓冲池、上传与帧所有权契约见 docs/architecture/runtime.md。

#include <optional>
#include <span>
#include <string_view>

#include <radray/gpu/types.h>
#include <radray/intrusive_ptr.h>
#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/allocator.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/vertex_input.h>

namespace radray {

class RDGBuilder;
class RDGRasterPassBuilder;
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
    // 池内稀疏槽号；空句柄返回 0。存活句柄的槽号可达或超过 GetPageCount()。
    uint32_t GetPageIndex() const noexcept;
    // 引用随所属页撤销提交而失效；单独保留 GetGpuBuffer() 不延长其寿命。
    const VertexInput& GetVertexInput() const noexcept;
    gpu::Buffer* GetBuffer() const noexcept;
    // 仅延长底层缓冲寿命；子分配区间仍须保留到 GPU 工作及命令引用结束。
    IntrusivePtr<GpuBuffer> GetGpuBuffer() const noexcept;
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
    // 池内稀疏槽号；空句柄返回 0。存活句柄的槽号可达或超过 GetPageCount()。
    uint32_t GetPageIndex() const noexcept;
    gpu::Buffer* GetBuffer() const noexcept;
    // 仅延长底层缓冲寿命；子分配区间仍须保留到 GPU 工作及命令引用结束。
    IntrusivePtr<GpuBuffer> GetGpuBuffer() const noexcept;
    uint32_t GetIndexStride() const noexcept;
    uint32_t GetFirstIndex() const noexcept;
    uint32_t GetIndexCount() const noexcept;
    uint64_t GetByteOffset() const noexcept;
    uint64_t GetByteSize() const noexcept;
    // 返回整页视图，配合 GetFirstIndex() 使用；同页绘制可兼容同一次索引缓冲绑定。
    gpu::IndexBufferView GetIndexBufferView() const noexcept;

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
    gpu::BufferUses AdditionalUsage{};
    std::span<const gpu::VertexBufferLayout> Layouts{};
    std::span<const VertexInputAttribute> Attributes{};
    std::string_view DebugNamePrefix{"VertexBufferPool"};
    GpuBufferMemory Memory{GpuBufferMemory::DeviceLocal};
};

struct IndexBufferPoolDescriptor {
    uint32_t IndexStride{4};
    uint32_t PageIndexCapacity{300'000};
    gpu::BufferUses AdditionalUsage{};
    std::string_view DebugNamePrefix{"IndexBufferPool"};
    GpuBufferMemory Memory{GpuBufferMemory::DeviceLocal};
};

// 池借用设备，所有调用必须串行。
class VertexBufferPool final {
public:
    static Nullable<unique_ptr<VertexBufferPool>> Create(gpu::Device& device, const VertexBufferPoolDescriptor& desc);
    ~VertexBufferPool() noexcept;
    VertexBufferPool(const VertexBufferPool&) = delete;
    VertexBufferPool& operator=(const VertexBufferPool&) = delete;

    std::optional<VertexBufferSubAllocation> Allocate(uint32_t vertexCount);
    // 当前已提交页数，不含稀疏空槽。
    uint32_t GetPageCount() const noexcept;
    uint64_t GetCapacityBytes() const noexcept;
    uint64_t GetFreeBytes() const noexcept;
    uint64_t GetAllocatedBytes() const noexcept;
    // 撤销所有整页空闲的提交，返回页数；外部页面强引用仍然有效。
    uint32_t TrimEmptyPages() noexcept;
    // 导入全部已提交缓冲（含空页）。影响本 Pass 读取的建页、重提交和 Trim 须先于此调用。
    void AddPassReadAccess(RDGBuilder& graph, RDGRasterPassBuilder& pass, gpu::BufferStates state = gpu::BufferState::Vertex) const;

private:
    friend class VertexBufferSubAllocation;
    class Impl;
    explicit VertexBufferPool(unique_ptr<Impl> impl) noexcept;
    unique_ptr<Impl> _impl;
};

class IndexBufferPool final {
public:
    static Nullable<unique_ptr<IndexBufferPool>> Create(gpu::Device& device, const IndexBufferPoolDescriptor& desc = {});
    ~IndexBufferPool() noexcept;
    IndexBufferPool(const IndexBufferPool&) = delete;
    IndexBufferPool& operator=(const IndexBufferPool&) = delete;

    std::optional<IndexBufferSubAllocation> Allocate(uint32_t indexCount);
    // 当前已提交页数，不含稀疏空槽。
    uint32_t GetPageCount() const noexcept;
    uint64_t GetCapacityBytes() const noexcept;
    uint64_t GetFreeBytes() const noexcept;
    uint64_t GetAllocatedBytes() const noexcept;
    // 撤销所有整页空闲的提交，返回页数；外部页面强引用仍然有效。
    uint32_t TrimEmptyPages() noexcept;
    // 导入全部已提交缓冲（含空页）。影响本 Pass 读取的建页、重提交和 Trim 须先于此调用。
    void AddPassReadAccess(RDGBuilder& graph, RDGRasterPassBuilder& pass, gpu::BufferStates state = gpu::BufferState::Index) const;

private:
    friend class IndexBufferSubAllocation;
    class Impl;
    explicit IndexBufferPool(unique_ptr<Impl> impl) noexcept;
    unique_ptr<Impl> _impl;
};

}  // namespace radray
