#pragma once

#include <stdexcept>

#include <radray/basic_corotinue.h>
#include <radray/vertex_data.h>
#include <radray/render/common.h>

namespace radray::render {

class GpuResourceException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RenderMesh {
public:
    vector<unique_ptr<Buffer>> _buffers;
};

class GpuUploader {
public:
    GpuUploader(Device* device, CommandQueue* queue) noexcept;
    GpuUploader(const GpuUploader&) = delete;
    GpuUploader& operator=(const GpuUploader&) = delete;
    GpuUploader(GpuUploader&&) noexcept = delete;
    GpuUploader& operator=(GpuUploader&&) noexcept = delete;
    ~GpuUploader() noexcept;

    void Destroy() noexcept;

    task<RenderMesh> UploadMeshAsync(const MeshResource& resource);

    void Submit();

    void Tick();

private:
    class BatchAwaiter;

    struct BufferSpan {
        Buffer* Buffer{nullptr};
        void* CpuAddress{nullptr};
        uint64_t Offset{0};
        uint64_t Size{0};
    };

    struct UploadRequest {
        Buffer* Src{nullptr};
        uint64_t SrcOffset{0};
        Buffer* Dst{nullptr};
        uint64_t DstOffset{0};
        uint64_t Size{0};
    };

    struct PendingTask {
        coroutine_handle<> Handle;
        vector<unique_ptr<Buffer>> RetainedBuffers;
    };

    class UploadBatch {
    public:
        unique_ptr<CommandBuffer> _cmdBuffer;
        unique_ptr<Fence> _fence;
        vector<unique_ptr<Buffer>> _tmpBuffers;
        vector<UploadRequest> _requests;
        vector<std::shared_ptr<PendingTask>> _awaiters;
    };

    UploadBatch* GetOrCreateCurrentBatch();

    BufferSpan AllocateUploadBuffer(uint64_t size, uint64_t alignment);

    Device* _device{nullptr};
    CommandQueue* _queue{nullptr};
    unique_ptr<UploadBatch> _currBatch{nullptr};
    vector<unique_ptr<UploadBatch>> _pendings;
};

class CBufferArena {
public:
    struct Descriptor {
        uint64_t BasicSize{256 * 256};
        uint64_t Alignment{256};
        uint64_t MaxResetSize{std::numeric_limits<uint64_t>::max()};
        string NamePrefix{};
    };

    struct Allocation {
        Buffer* Target{nullptr};
        void* Mapped{nullptr};
        uint64_t Offset{0};
        uint64_t Size{0};

        static constexpr Allocation Invalid() noexcept { return Allocation{}; }
    };

    class Block {
    public:
        explicit Block(unique_ptr<Buffer> buf) noexcept;
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
        ~Block() noexcept;

    public:
        unique_ptr<Buffer> _buf;
        void* _mapped{nullptr};
        uint64_t _used{0};
    };

    CBufferArena(Device* device, const Descriptor& desc) noexcept;
    explicit CBufferArena(Device* device) noexcept;
    CBufferArena(const CBufferArena&) = delete;
    CBufferArena& operator=(const CBufferArena&) = delete;
    CBufferArena(CBufferArena&& other) noexcept;
    CBufferArena& operator=(CBufferArena&& other) noexcept;
    ~CBufferArena() noexcept;

    bool IsValid() const noexcept;

    void Destroy() noexcept;

    Allocation Allocate(uint64_t size) noexcept;

    void Reset() noexcept;

    void Clear() noexcept;

    friend void swap(CBufferArena& a, CBufferArena& b) noexcept;

private:
    Nullable<Block*> GetOrCreateBlock(uint64_t size) noexcept;

    Device* _device;
    vector<unique_ptr<Block>> _blocks;
    Descriptor _desc;
    uint64_t _minBlockSize{};
};

}  // namespace radray::render
