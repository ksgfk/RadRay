#pragma once

#include <stdexcept>

#include <radray/basic_corotinue.h>
#include <radray/vertex_data.h>
#include <radray/render/common.h>

namespace radray::render {

using cppcoro::async_manual_reset_event;

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

    void Submit(Scheduler& scheduler);

private:
    // struct UploadRequest {
    //     Buffer* Src{nullptr};
    //     Buffer* Dst{nullptr};
    //     uint64_t SrcOffset{0};
    //     uint64_t DstOffset{0};
    //     uint64_t Size{0};
    // };

    // struct UploadBatch {
    //     vector<UploadRequest> Requests;
    //     async_manual_reset_event Event;
    //     vector<unique_ptr<Buffer>> StagingBuffers;
    // };

    struct BufferSpan {
        Buffer* Buffer{nullptr};
        void* CpuAddress{nullptr};
        uint64_t Offset{0};
        uint64_t Size{0};
    };

    class UploadBatch {
    public:
        vector<unique_ptr<Buffer>> _tempBuffers;
    };

    BufferSpan AllocateUploadBuffer(uint64_t size, uint64_t alignment);

    Device* _device{nullptr};
    CommandQueue* _queue{nullptr};

    // std::shared_ptr<UploadBatch> _currentBatch;

    vector<unique_ptr<CommandBuffer>> _cmdPool;
    vector<unique_ptr<Fence>> _fencePool;
};

}  // namespace radray::render
