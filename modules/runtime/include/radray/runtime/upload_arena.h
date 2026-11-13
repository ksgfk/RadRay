#pragma once

// 上传页与帧生命周期契约见 docs/architecture/runtime.md。

#include <radray/render/rhi.h>

namespace radray {

struct UploadAllocation {
    Nullable<render::Buffer*> Buffer{nullptr};
    Nullable<byte*> Data{nullptr};
    uint64_t Offset{0};
    uint64_t Size{0};
    uint64_t ReservedSize{0};
};

class UploadArena final {
public:
    explicit UploadArena(render::Device& device, uint64_t pageSize = 1024 * 1024) noexcept;
    ~UploadArena() noexcept;
    UploadArena(const UploadArena&) = delete;
    UploadArena& operator=(const UploadArena&) = delete;

    // GPU 完成、命令存储重置后复用；稳定页上的参数组绑定可保留。
    void Reset() noexcept;
    // 释放页前还须解除所有参数组和 view 引用。
    void Clear() noexcept;
    // 有效大小与物理保留范围分开；本帧已返回的分配不搬移，零大小返回空切片。
    std::optional<UploadAllocation> Allocate(uint64_t size, uint64_t alignment, uint64_t reservedSize, render::BufferStates reads);
    bool Flush() noexcept;
    template <class Allocator>
    void AppendReadBarriers(std::vector<render::ResourceBarrierDescriptor, Allocator>& barriers) const {
        for (const auto& page : _pages) {
            if (!page.Cursor) continue;
            if (page.SubmittedState != render::BufferState::HostWrite)
                barriers.emplace_back(render::BarrierBufferDescriptor{page.Buffer.get(), page.SubmittedState, render::BufferState::HostWrite});
            barriers.emplace_back(render::BarrierBufferDescriptor{page.Buffer.get(), render::BufferState::HostWrite, page.Reads});
        }
    }
    // 只能在包含这些页的命令正常提交后调用。
    void CommitSubmittedStates() noexcept;
    uint64_t GetCapacity() const noexcept;

private:
    struct Page {
        unique_ptr<render::Buffer> Buffer;
        byte* Data;
        uint64_t Capacity;
        uint64_t Cursor{0};
        uint64_t DirtyBegin{0}, DirtyEnd{0};
        render::BufferStates Reads{};
        render::BufferStates SubmittedState{render::BufferState::HostWrite};
    };
    render::Device& _device;
    uint64_t _pageSize;
    vector<Page> _pages;
};

}  // namespace radray
