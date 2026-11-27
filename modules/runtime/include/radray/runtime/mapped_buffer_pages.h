#pragma once

// 上传页与帧生命周期契约见 docs/architecture/runtime.md。

#include <cstdint>
#include <optional>
#include <vector>

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/gpu/types.h>

namespace radray {

struct UploadAllocation {
    Nullable<gpu::Buffer*> Buffer{nullptr};
    Nullable<byte*> Data{nullptr};
    uint64_t Offset{0};
    uint64_t Size{0};
    uint64_t ReservedSize{0};
};

namespace detail {

class MappedBufferPages {
public:
    MappedBufferPages(const MappedBufferPages&) = delete;
    MappedBufferPages& operator=(const MappedBufferPages&) = delete;

    // Called after GPU completion, command-storage reset and release of slice bindings.
    void BeginFrame() noexcept;
    void Clear() noexcept;
    bool Flush() noexcept;
    template <class Allocator>
    void AppendReadBarriers(std::vector<gpu::ResourceBarrierDescriptor, Allocator>& barriers) const {
        for (const auto& page : _pages) {
            if (!page.Cursor) continue;
            if (page.SubmittedState != gpu::BufferState::HostWrite)
                barriers.emplace_back(gpu::BarrierBufferDescriptor{page.Buffer.get(), page.SubmittedState, gpu::BufferState::HostWrite});
            barriers.emplace_back(gpu::BarrierBufferDescriptor{page.Buffer.get(), gpu::BufferState::HostWrite, page.Reads});
        }
    }
    // 只能在包含这些页的命令正常提交后调用。
    void CommitSubmittedStates() noexcept;
    uint64_t GetCapacity() const noexcept;
    gpu::Device& GetDevice() const noexcept;

protected:
    enum class IdlePagePolicy : uint8_t { ReleaseAll, KeepOneStandardPagePerMemoryType };
    MappedBufferPages(gpu::Device& device, uint64_t pageSize, gpu::BufferUses usage, IdlePagePolicy idlePagePolicy) noexcept;
    ~MappedBufferPages() noexcept = default;
    std::optional<UploadAllocation> AllocateInDomain(gpu::MemoryType memory, uint64_t size, uint64_t alignment, uint64_t reservedSize, gpu::BufferStates reads);

private:
    struct Page {
        unique_ptr<gpu::Buffer> Buffer;
        byte* Data;
        uint64_t Capacity;
        gpu::MemoryType Memory;
        bool IsBasePage;
        uint8_t IdleFrames{0};
        uint64_t Cursor{0};
        uint64_t DirtyBegin{0}, DirtyEnd{0};
        gpu::BufferStates Reads{};
        gpu::BufferStates SubmittedState{gpu::BufferState::HostWrite};
    };
    gpu::Device& _device;
    uint64_t _pageSize;
    gpu::BufferUses _usage;
    IdlePagePolicy _idlePagePolicy;
    vector<Page> _pages;
};

}  // namespace detail
}  // namespace radray
