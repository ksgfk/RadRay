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

    task<RenderMesh> UploadMeshAsync(const MeshResource& resource);

    void Submit();

    void Schedule();

private:
    struct BufferSpan {
        Buffer* Buffer{nullptr};
        void* CpuAddress{nullptr};
        size_t Offset{0};
        size_t Size{0};
    };
    struct Page {
        unique_ptr<Buffer> Handle;
        void* MappedPtr{nullptr};
        size_t CurrentOffset{0};
        size_t Size{0};
    };
    struct Request {
        BufferSpan Src;
        BufferSpan Dst;
    };

    BufferSpan AllocateUploadBuffer(uint64_t size, uint64_t alignment);

    Page RequestPage(uint64_t reqSize);

    task<> RequestCopyAsync(std::span<Request> req);

    Device* _device{nullptr};
    CommandQueue* _queue{nullptr};

    const uint64_t _defaultPageSize{4 * 1024 * 1024};
    std::optional<Page> _activePage;
    vector<Page> _usedPages;
    vector<Page> _freePages;
};

}  // namespace radray::render
