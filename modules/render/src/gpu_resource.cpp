#include <radray/render/gpu_resource.h>

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
    _cmdPool.clear();
    _fencePool.clear();
}

task<RenderMesh> GpuUploader::UploadMeshAsync(const MeshResource& resource) {
    // if (!_currentBatch) {
    //     _currentBatch = std::make_shared<UploadBatch>();
    // }
    // // Capture current batch to keep it alive until this task resumes
    // auto batch = _currentBatch;

    // uint64_t totalBufferSize = 0;
    // for (const auto& bufData : resource.Bins) {
    //     totalBufferSize += Align(bufData.GetSize(), 256);
    // }
    // auto uploadBuffer = AllocateUploadBuffer(totalBufferSize, 256);
    // // Keep staging buffer alive in the batch
    // batch->StagingBuffers.emplace_back(std::move(uploadBuffer.BufferOwner)); 

    // void* uploadMapped = uploadBuffer.CpuAddress;
    // for (const auto& bufData : resource.Bins) {
    //     auto data = bufData.GetData();
    //     std::memcpy(uploadMapped, data.data(), data.size());
    //     uploadMapped = static_cast<byte*>(uploadMapped) + Align(data.size(), 256);
    // }

    // vector<unique_ptr<Buffer>> dsts;
    // dsts.reserve(resource.Bins.size());
    
    // uint64_t currentOffset = 0;
    // for (const auto& bufData : resource.Bins) {
    //     BufferDescriptor dstDesc{
    //         bufData.GetSize(),
    //         MemoryType::Device,
    //         BufferUse::CopyDestination | BufferUse::Vertex | BufferUse::Index,
    //         ResourceHint::None};
    //     auto dstBuf = _device->CreateBuffer(dstDesc).Unwrap();
        
    //     batch->Requests.emplace_back(UploadRequest{
    //         uploadBuffer.Buffer,
    //         dstBuf.get(),
    //         uploadBuffer.Offset + currentOffset,
    //         0,
    //         bufData.GetSize()});
            
    //     dsts.emplace_back(std::move(dstBuf));
    //     currentOffset += Align(bufData.GetSize(), 256);
    // }
    
    // // Wait for submission completion
    // co_await batch->Event;

    // // Construct result
    // RenderMesh mesh;
    // mesh._buffers = std::move(dsts);
    // co_return mesh;
}

// void GpuUploader::Submit(Scheduler& scheduler) {
//     if (!_currentBatch || _currentBatch->Requests.empty()) {
//         return;
//     }

//     auto batch = std::move(_currentBatch);
//     // Reset current batch for new requests
//     _currentBatch = nullptr;

//     auto cmd = _cmdPool.back().get();
//     // Simplified: Assuming cmd pool management exists or reuse logic
//     // In real impl, you need to acquire a fresh cmd buffer
    
//     cmd->Begin();
//     for(const auto& req : batch->Requests) {
//         cmd->CopyBufferRegion(req.Dst, req.DstOffset, req.Src, req.SrcOffset, req.Size);
//     }
//     cmd->End();
    
//     _queue->ExecuteCommandBuffers({cmd});
    
//     // Fence logic would go here. 
//     // Assuming Fence implementation
//     auto fence = _fencePool.back().get();
//     uint64_t signalVal = fence->Signal(_queue);

//     // Launch a polling task to wait for fence and signal event
//     scheduler.Schedule([batch, fence, signalVal, &scheduler]() -> task<void> {
//         while(fence->GetCompletedValue() < signalVal) {
//             co_await scheduler.NextTick();
//         }
//         batch->Event.set();
//     }());
// }
//     // }

//     // // 等待拷贝完成
//     // co_await RequestCopyAsync(requests);

//     // RenderMesh mesh{std::move(dsts)};
//     // co_return mesh;
// }

void GpuUploader::Submit(Scheduler& scheduler) {

    // // 获取命令列表
    // unique_ptr<CommandBuffer> cmd;
    // if (_cmdPool.empty()) {
    //     cmd = _device->CreateCommandBuffer(_queue).Unwrap();
    // } else {
    //     cmd = std::move(_cmdPool.back());
    //     _cmdPool.pop_back();
    // }

    // // 记录命令
    // cmd->Begin();
    // vector<BarrierBufferDescriptor> barriers;
    // barriers.reserve(_pendingRequests.size());
    // for (const auto& req : _pendingRequests) {
    //     barriers.emplace_back(
    //         BarrierBufferDescriptor{
    //             req.Dst,
    //             BufferUse::Common,
    //             BufferUse::CopyDestination,
    //             nullptr,
    //             false});
    // }
    // cmd->ResourceBarrier(barriers, {});
    // for (const auto& req : _pendingRequests) {
    //     cmd->CopyBufferToBuffer(req.Dst, req.DstOffset, req.Src, req.SrcOffset, req.Size);
    // }
    // for (auto& b : barriers) {
    //     std::swap(b.Before, b.After);
    // }
    // cmd->ResourceBarrier(barriers, {});
    // cmd->End();

    // // 获取 Fence
    // unique_ptr<Fence> fence;
    // if (_fencePool.empty()) {
    //     fence = _device->CreateFence().Unwrap();
    // } else {
    //     fence = std::move(_fencePool.back());
    //     _fencePool.pop_back();
    //     fence->Reset();
    // }

    // // 提交到队列
    // CommandBuffer* cmds[] = {cmd.get()};
    // CommandQueueSubmitDescriptor submitDesc{};
    // submitDesc.CmdBuffers = cmds;
    // submitDesc.SignalFence = fence.get();
    // _queue->Submit(submitDesc);

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
}

}  // namespace radray::render
