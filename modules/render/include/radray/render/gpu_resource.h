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

}  // namespace radray::render
