#include "d3d12_texture.h"

#include "d3d12_device.h"
#include "d3d12_descriptor_heap.h"
#include "d3d12_buffer.h"

namespace radray::render::d3d12 {

TextureD3D12::TextureD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12Resource> tex,
    ComPtr<D3D12MA::Allocation> alloc,
    const D3D12_RESOURCE_STATES& initState,
    ResourceType type) noexcept
    : _device(device),
      _tex(std::move(tex)),
      _alloc(std::move(alloc)),
      _initState(initState),
      _type(type) {
    _desc = _tex->GetDesc();
}

bool TextureD3D12::IsValid() const noexcept { return _tex != nullptr; }

void TextureD3D12::Destroy() noexcept {
    _tex = nullptr;
    _alloc = nullptr;
}

ResourceType TextureD3D12::GetType() const noexcept {
    return _type;
}

ResourceStates TextureD3D12::GetInitState() const noexcept {
    return MapType(_initState);
}

uint64_t TextureD3D12::GetUploadNeedSize(uint32_t mipLevel, uint32_t arrayLayer, uint32_t layerCount) const noexcept {
    uint32_t subresource = SubresourceIndex(
        mipLevel,
        arrayLayer,
        0,
        1,
        layerCount);
    UINT64 total;
    _device->_device->GetCopyableFootprints(
        &_desc,
        subresource,
        1,
        0,
        nullptr,
        nullptr,
        nullptr,
        &total);
    return total;
}

void TextureD3D12::HelpCopyDataToUpload(
    Resource* dst,
    const void* src,
    size_t srcSize,
    uint32_t mipLevel,
    uint32_t arrayLayer,
    uint32_t layerCount) const noexcept {
    RADRAY_ASSERT(dst->GetType() == ResourceType::Buffer);
    auto* dstBuf = static_cast<BufferD3D12*>(dst);
    uint32_t subresource = SubresourceIndex(
        mipLevel,
        arrayLayer,
        0,
        1,
        layerCount);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    UINT numRows;
    UINT64 rowSizeInBytes, total;
    _device->_device->GetCopyableFootprints(
        &_desc,
        subresource,
        1,
        0,
        &layout,
        &numRows,
        &rowSizeInBytes,
        &total);
    if (dstBuf->GetSize() < total) {
        RADRAY_ERR_LOG("d3d12 Buffer size is not enough for upload, need {}, but have {}", total, dstBuf->GetSize());
        return;
    }
    auto dstBufMapValue = dstBuf->Map(0, dstBuf->GetSize());
    if (!dstBufMapValue.HasValue()) {
        RADRAY_ERR_LOG("d3d12 Buffer map failed, size {}", dstBuf->GetSize());
        return;
    }
    auto* dstPtr = static_cast<BYTE*>(dstBufMapValue.Value());
    D3D12_MEMCPY_DEST destData{
        dstPtr + layout.Offset,
        layout.Footprint.RowPitch,
        SIZE_T(layout.Footprint.RowPitch) * SIZE_T(numRows)};
    D3D12_SUBRESOURCE_DATA srcData{
        src,
        static_cast<LONG_PTR>(srcSize) / numRows,
        static_cast<LONG_PTR>(srcSize)};
    MemcpySubresource(&destData, &srcData, rowSizeInBytes, numRows, layout.Footprint.Depth);
    dstBuf->Unmap();
}

TextureViewD3D12::~TextureViewD3D12() noexcept {
    Destroy();
}

bool TextureViewD3D12::IsValid() const noexcept {
    return _desc.texture != nullptr && _desc.heapView.IsValid();
}

void TextureViewD3D12::Destroy() noexcept {
    if (IsValid()) {
        _desc.heapAlloc->Destroy(_desc.heapView);
        _desc.heapView = DescriptorHeapView::Invalid();
        _desc.texture = nullptr;
    }
}

void TextureViewD3D12::CopyTo(DescriptorHeap* dst, UINT dstStart) noexcept {
    _desc.heapView.Heap->CopyTo(_desc.heapView.Start, 1, dst, dstStart);
}

}  // namespace radray::render::d3d12
