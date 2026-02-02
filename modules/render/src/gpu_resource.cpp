#include <radray/render/gpu_resource.h>

#include <radray/scope_guard.h>

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

CBufferArena::Block::Block(unique_ptr<Buffer> buf) noexcept
    : _buf(std::move(buf)) {
    _mapped = _buf->Map(0, _buf->GetDesc().Size);
    RADRAY_ASSERT(_mapped != nullptr);
}

CBufferArena::Block::~Block() noexcept {
    if (_buf && _mapped) {
        _buf->Unmap(0, _buf->GetDesc().Size);
        _mapped = nullptr;
    }
}

CBufferArena::CBufferArena(Device* device, const Descriptor& desc) noexcept
    : _device(device),
      _desc(desc) {
    if (_desc.Alignment == 0 || (_desc.Alignment & (_desc.Alignment - 1)) != 0) {
        RADRAY_ABORT("CBufferArena invalid Alignment: {} (must be power-of-two and non-zero)", _desc.Alignment);
    }
}

CBufferArena::CBufferArena(Device* device) noexcept
    : CBufferArena(device, Descriptor{256 * 256, 256, 1024 * 1024, "cb_arena"}) {}

CBufferArena::CBufferArena(CBufferArena&& other) noexcept
    : _device(other._device),
      _blocks(std::move(other._blocks)),
      _desc(std::move(other._desc)),
      _minBlockSize(other._minBlockSize) {
    other._device = nullptr;
    other._minBlockSize = 0;
}

CBufferArena& CBufferArena::operator=(CBufferArena&& other) noexcept {
    CBufferArena tmp{std::move(other)};
    swap(*this, tmp);
    return *this;
}

CBufferArena::~CBufferArena() noexcept {
    this->Destroy();
}

bool CBufferArena::IsValid() const noexcept {
    return _device != nullptr;
}

void CBufferArena::Destroy() noexcept {
    _blocks.clear();
    _device = nullptr;
}

CBufferArena::Allocation CBufferArena::Allocate(uint64_t size) noexcept {
    if (size == 0) {
        return Allocation::Invalid();
    }
    auto blockOpt = this->GetOrCreateBlock(size);
    if (!blockOpt.HasValue()) {
        RADRAY_ABORT("allocation failed: cannot create cbuffer block");
    }
    auto block = blockOpt.Release();
    uint64_t offsetStart = Align(block->_used, _desc.Alignment);
    block->_used = offsetStart + size;
    Allocation alloc{};
    alloc.Target = block->_buf.get();
    alloc.Mapped = static_cast<byte*>(block->_mapped) + offsetStart;
    alloc.Offset = offsetStart;
    alloc.Size = size;
    return alloc;
}

Nullable<CBufferArena::Block*> CBufferArena::GetOrCreateBlock(uint64_t size) noexcept {
    if (!_blocks.empty()) {
        auto last = _blocks.back().get();
        auto desc = last->_buf->GetDesc();
        auto offsetStart = Align(last->_used, _desc.Alignment);
        if (offsetStart < desc.Size) {
            auto remain = desc.Size - offsetStart;
            if (remain >= size) {
                return last;
            }
        }
    }
    string name = radray::format("{}_{}", _desc.NamePrefix, _blocks.size());
    BufferDescriptor desc{};
    desc.Size = Align(std::max(_minBlockSize, std::max(size, _desc.BasicSize)), _desc.Alignment);
    desc.Memory = MemoryType::Upload;
    desc.Usage = BufferUse::CBuffer | BufferUse::MapWrite | BufferUse::CopySource;
    desc.Hints = ResourceHint::None;
    desc.Name = name;
    CBufferArena::Block* result = nullptr;
    {
        auto bufOpt = _device->CreateBuffer(desc);
        if (!bufOpt.HasValue()) {
            return nullptr;
        }
        result = _blocks.emplace_back(make_unique<CBufferArena::Block>(bufOpt.Release())).get();
    }
    return result;
}

void CBufferArena::Reset() noexcept {
    if (_blocks.empty()) {
        return;
    } else if (_blocks.size() == 1) {
        if (_blocks[0]->_buf->GetDesc().Size > _desc.MaxResetSize) {
            _minBlockSize = _desc.MaxResetSize;
            _blocks.clear();
        } else {
            _blocks[0]->_used = 0;
        }
    } else {
        _minBlockSize = 0;
        for (const auto& i : _blocks) {
            _minBlockSize += i->_buf->GetDesc().Size;
        }
        _minBlockSize = std::min(_minBlockSize, _desc.MaxResetSize);
        _blocks.clear();
    }
}

void CBufferArena::Clear() noexcept {
    _blocks.clear();
    _minBlockSize = 0;
}

void swap(CBufferArena& a, CBufferArena& b) noexcept {
    using std::swap;
    swap(a._device, b._device);
    swap(a._blocks, b._blocks);
    swap(a._desc, b._desc);
    swap(a._minBlockSize, b._minBlockSize);
}

}  // namespace radray::render
