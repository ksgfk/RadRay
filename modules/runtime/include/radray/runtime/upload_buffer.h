#pragma once

// 上传与帧所有权契约见 docs/architecture/runtime.md。

#include <string_view>

#include <radray/runtime/rdg.h>

namespace radray {

class MappedUploadBuffer final {
public:
    static Nullable<unique_ptr<MappedUploadBuffer>> Create(render::Device& device, uint64_t capacity, render::BufferUses usage = render::BufferUse::CopySource);
    ~MappedUploadBuffer() noexcept;
    MappedUploadBuffer(const MappedUploadBuffer&) = delete;
    MappedUploadBuffer& operator=(const MappedUploadBuffer&) = delete;

    // 要求 GPU 使用已完成，且引用它的命令分配器已重置，例如在 BeginFrame 之后调用。
    // 扩容会丢弃原内容并使指针失效；失败时保留原分配。
    bool Reserve(render::Device& device, uint64_t minCapacity);
    bool Flush(uint64_t offset, uint64_t byteSize) noexcept;
    render::Buffer* Get() const noexcept;
    byte* GetData() const noexcept;
    uint64_t GetCapacity() const noexcept;

private:
    MappedUploadBuffer(render::Device& device, unique_ptr<render::Buffer> buffer, byte* data, uint64_t capacity, render::BufferUses usage) noexcept;
    render::Device& _device;
    unique_ptr<render::Buffer> _buffer;
    byte* _data;
    uint64_t _capacity;
    render::BufferUses _usage;
};

class BatchedBufferUpload final {
public:
    static Nullable<unique_ptr<BatchedBufferUpload>> Create(render::Device& device, uint64_t defaultPageSize = 1024 * 1024);
    ~BatchedBufferUpload() noexcept;
    BatchedBufferUpload(const BatchedBufferUpload&) = delete;
    BatchedBufferUpload& operator=(const BatchedBufferUpload&) = delete;

    // 仅支持 Device 内存的 CopyDestination 缓冲；对齐值必须是非零的二次幂。
    // 后续 Acquire 不会使已有映射指针失效；须在 CommitToGraph 之前完成写入。
    Nullable<byte*> Acquire(render::Buffer* dstBuffer, uint64_t dstOffset, uint64_t byteSize, uint64_t alignment = 16);
    bool Stage(render::Buffer* dstBuffer, uint64_t dstOffset, const void* data, uint64_t byteSize, uint64_t alignment = 16);
    // 刷新页面并记录一个拷贝 Pass；目标缓冲使用 Common → Common 边界并标记为输出。
    // 每次 Reset 后只能调用一次；图内捕获的拷贝记录由图自行持有。
    bool CommitToGraph(RDGBuilder& graph, std::string_view passName = "BatchedBufferUpload");
    // 要求 GPU 使用已完成且命令存储已重置；保留全部页面供复用。
    void Reset() noexcept;
    uint32_t GetPageCount() const noexcept;
    uint64_t GetCapacity() const noexcept;
    uint64_t GetStagedByteSize() const noexcept;

private:
    struct Page {
        unique_ptr<MappedUploadBuffer> Buffer;
        uint64_t Cursor{0};
    };
    struct BufferUploadRegion {
        uint32_t PageIndex;
        uint64_t SrcOffset;
        render::Buffer* DstBuffer;
        uint64_t DstOffset;
        uint64_t ByteSize;
    };
    BatchedBufferUpload(render::Device& device, uint64_t defaultPageSize) noexcept;
    render::Device& _device;
    uint64_t _defaultPageSize;
    vector<Page> _pages;
    vector<BufferUploadRegion> _regions;
    uint32_t _activePage{0};
    bool _committed{false};
};

}  // namespace radray
