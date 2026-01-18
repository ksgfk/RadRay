#include <radray/render/gpu_resource.h>

namespace radray::render {

GpuUploader::GpuUploader(
    Device* device,
    CommandQueue* queue) noexcept
    : _device(device),
      _queue(queue) {}

GpuUploader::BufferSpan GpuUploader::AllocateUploadBuffer(size_t size, size_t alignment) {
    if (_activePage.has_value()) {
        auto& page = _activePage.value();
        size_t alignedOffset = Align(page.CurrentOffset, alignment);
        if (alignedOffset + size <= page.Size) {
            auto ptr = static_cast<byte*>(page.MappedPtr) + alignedOffset;
            page.CurrentOffset = alignedOffset + size;
            return {page.Handle.get(), ptr, alignedOffset, size};
        }
        _usedPages.emplace_back(std::move(page));
        _activePage.reset();
    }
    _activePage = RequestPage(size);
    auto& page = _activePage.value();
    size_t alignedOffset = Align(size_t(0), alignment);
    auto ptr = static_cast<byte*>(page.MappedPtr) + alignedOffset;
    page.CurrentOffset = alignedOffset + size;
    return {page.Handle.get(), ptr, alignedOffset, size};
}

GpuUploader::Page GpuUploader::RequestPage(uint64_t reqSize) {
    for (auto& page : _freePages) {
        if (page.Size >= reqSize) {
            Page p = std::move(page);
            std::swap(page, _freePages.back());
            _freePages.pop_back();
            p.CurrentOffset = 0;
            return p;
        }
    }
    BufferDescriptor desc{
        std::max(reqSize, _defaultPageSize),
        MemoryType::Upload,
        BufferUse::CopySource | BufferUse::MapWrite,
        ResourceHint::None};
    auto result = _device->CreateBuffer(desc);
    if (result.HasValue()) {
        auto buf = result.Unwrap();
        void* mapped = buf->Map(0, desc.Size);
        return Page{std::move(buf), mapped, 0, desc.Size};
    }
    throw GpuResourceException{"failed to create GpuUploader page"};
}

task<RenderMesh> GpuUploader::UploadMeshAsync(const MeshResource& resource) {
    size_t totalBufferSize = 0;
    for (const auto& bufData : resource.Bins) {
        totalBufferSize += Align(bufData.GetSize(), 256);
    }
    auto uploadBuffer = AllocateUploadBuffer(totalBufferSize, 256);
    void* uploadMapped = uploadBuffer.CpuAddress;
    for (const auto& bufData : resource.Bins) {
        auto data = bufData.GetData();
        std::memcpy(uploadMapped, data.data(), data.size());
        uploadMapped = static_cast<byte*>(uploadMapped) + Align(data.size(), 256);
    }
    vector<unique_ptr<Buffer>> dsts;
    dsts.reserve(resource.Bins.size());
    vector<Request> requests;
    requests.reserve(resource.Bins.size());
    for (const auto& bufData : resource.Bins) {
        BufferDescriptor dstDesc{
            bufData.GetSize(),
            MemoryType::Device,
            BufferUse::CopyDestination | BufferUse::Vertex | BufferUse::Index,
            ResourceHint::None};
        dsts.emplace_back(_device->CreateBuffer(dstDesc).Unwrap());
        requests.emplace_back(BufferSpan{}, BufferSpan{}); // TODO:
    }
    co_await RequestCopyAsync(requests);
    RenderMesh mesh{
        std::move(dsts)};
    co_return mesh;
}

}  // namespace radray::render
