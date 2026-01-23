#include <radray/render/gpu_resource.h>

#include <radray/utility.h>

namespace radray::render {

GpuUploader::GpuUploader(
    Device* device,
    CommandQueue* queue) noexcept
    : _device(device),
      _queue(queue) {}

GpuUploader::~GpuUploader() noexcept {
    this->Destroy();
}

void GpuUploader::Destroy() noexcept {
    _queue->Wait();
    vector<std::shared_ptr<PendingTask>> toResume;
    if (_currBatch) {
        toResume.insert(toResume.end(), _currBatch->_awaiters.begin(), _currBatch->_awaiters.end());
        _currBatch = nullptr;
    }
    for (auto& batch : _pendings) {
        toResume.insert(toResume.end(), batch->_awaiters.begin(), batch->_awaiters.end());
    }
    _pendings.clear();
    for (auto& task : toResume) {
        if (task && task->Handle) {
            try {
                task->Handle.resume();
            } catch (...) {
                RADRAY_ERR_LOG("GpuUploader destruction: Coroutine threw exception during forced resume");
            }
        }
    }
}

class GpuUploader::BatchAwaiter {
public:
    explicit BatchAwaiter(UploadBatch* batch, std::shared_ptr<GpuUploader::PendingTask> task)
        : _batch(batch), _task(std::move(task)) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(coroutine_handle<> h) noexcept {
        _task->Handle = h;
        _batch->_awaiters.emplace_back(std::move(_task));
    }
    void await_resume() noexcept {}

private:
    UploadBatch* _batch;
    std::shared_ptr<GpuUploader::PendingTask> _task;
};

task<RenderMesh> GpuUploader::UploadMeshAsync(const MeshResource& resource) {
    uint64_t totalBufferSize = 0;
    for (const auto& bufData : resource.Bins) {
        totalBufferSize += Align(bufData.GetSize(), 256);
    }
    auto upload = AllocateUploadBuffer(totalBufferSize, 256);
    {
        void* uploadMapped = upload.CpuAddress;
        for (const auto& bufData : resource.Bins) {
            auto data = bufData.GetData();
            std::memcpy(uploadMapped, data.data(), data.size());
            uploadMapped = static_cast<byte*>(uploadMapped) + Align(data.size(), 256);
        }
    }
    auto taskState = std::make_shared<PendingTask>();
    auto guard = MakeScopeGuard([taskState]() {
        taskState->Handle = nullptr;
    });
    taskState->RetainedBuffers.reserve(resource.Bins.size());
    for (const auto& bufData : resource.Bins) {
        BufferDescriptor dstDesc{
            bufData.GetSize(),
            MemoryType::Device,
            BufferUse::CopyDestination | BufferUse::Vertex | BufferUse::Index,
            ResourceHint::None};
        taskState->RetainedBuffers.emplace_back(_device->CreateBuffer(dstDesc).Unwrap());
    }
    auto batch = GetOrCreateCurrentBatch();
    uint64_t currentOffset = 0;
    for (size_t i = 0; i < resource.Bins.size(); ++i) {
        const auto& bufData = resource.Bins[i];
        const auto& buf = taskState->RetainedBuffers[i];
        batch->_requests.emplace_back(upload.Buffer, currentOffset, buf.get(), 0, bufData.GetSize());
        currentOffset += Align(bufData.GetSize(), 256);
    }
    co_await BatchAwaiter(batch, taskState);
    RenderMesh mesh;
    mesh._buffers = std::move(taskState->RetainedBuffers);
    co_return mesh;
}

void GpuUploader::Submit() {
    if (!_currBatch) {
        return;
    }
    auto batch = _currBatch.get();
    auto cmdBuffer = batch->_cmdBuffer.get();
    cmdBuffer->Begin();
    vector<render::BarrierBufferDescriptor> bufBarriers;
    bufBarriers.reserve(batch->_requests.size());
    for (const auto& req : batch->_requests) {
        bufBarriers.emplace_back(
            req.Dst,
            BufferUse::Common,
            BufferUse::CopyDestination);
    }
    cmdBuffer->ResourceBarrier(bufBarriers, {});
    for (const auto& req : batch->_requests) {
        cmdBuffer->CopyBufferToBuffer(
            req.Dst,
            req.DstOffset,
            req.Src,
            req.SrcOffset,
            req.Size);
    }
    bufBarriers.clear();
    for (const auto& req : batch->_requests) {
        bufBarriers.emplace_back(
            req.Dst,
            BufferUse::CopyDestination,
            BufferUse::Common);
    }
    cmdBuffer->ResourceBarrier(bufBarriers, {});
    cmdBuffer->End();
    CommandQueueSubmitDescriptor submitDesc{
        std::span{&cmdBuffer, 1},
        batch->_fence.get()};
    _pendings.emplace_back(std::move(_currBatch));
    _queue->Submit(submitDesc);
}

void GpuUploader::Tick() {
    auto it = std::partition(_pendings.begin(), _pendings.end(), [](const unique_ptr<UploadBatch>& batch) {
        return batch->_fence->GetStatus() != FenceStatus::Complete;
    });
    vector<unique_ptr<UploadBatch>> ready;
    ready.reserve(std::distance(it, _pendings.end()));
    for (auto i = it; i != _pendings.end(); ++i) {
        ready.emplace_back(std::move(*i));
    }
    _pendings.erase(it, _pendings.end());
    for (const auto& batch : ready) {
        for (auto& task : batch->_awaiters) {
            if (task && task->Handle) {
                task->Handle.resume();
            }
        }
    }
}

GpuUploader::UploadBatch* GpuUploader::GetOrCreateCurrentBatch() {
    if (!_currBatch) {
        _currBatch = std::make_unique<UploadBatch>();
        _currBatch->_cmdBuffer = _device->CreateCommandBuffer(_queue).Unwrap();
        _currBatch->_fence = _device->CreateFence().Unwrap();
    }
    return _currBatch.get();
}

GpuUploader::BufferSpan GpuUploader::AllocateUploadBuffer(uint64_t size, uint64_t alignment) {
    size = Align(size, alignment);
    BufferDescriptor uploadDesc{
        size,
        MemoryType::Upload,
        BufferUse::CopySource | BufferUse::MapWrite,
        ResourceHint::None};
    auto uploadBuffer = _device->CreateBuffer(uploadDesc).Unwrap();
    void* mapped = uploadBuffer->Map(0, uploadDesc.Size);
    auto batch = GetOrCreateCurrentBatch();
    auto& tmpBuffer = batch->_tmpBuffers.emplace_back(std::move(uploadBuffer));
    return {tmpBuffer.get(), mapped, 0, size};
}

}  // namespace radray::render
