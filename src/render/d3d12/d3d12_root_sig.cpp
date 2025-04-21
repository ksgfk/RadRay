#include "d3d12_root_sig.h"

#include "d3d12_descriptor_heap.h"
#include "d3d12_buffer.h"
#include "d3d12_texture.h"

namespace radray::render::d3d12 {

RootSigD3D12::RootSigD3D12(ComPtr<ID3D12RootSignature> rootSig) noexcept : _rootSig(std::move(rootSig)) {}

void RootSigD3D12::Destroy() noexcept { _rootSig = nullptr; }

std::span<const RootConstantInfo> RootSigD3D12::GetRootConstants() const noexcept {
    return _rootConstants;
}

std::span<const RootDescriptorInfo> RootSigD3D12::GetRootDescriptors() const noexcept {
    return _rootDescriptors;
}

std::span<const DescriptorSetElementInfo> RootSigD3D12::GetBindDescriptors() const noexcept {
    return _bindDescriptors;
}

static void DestroyGpuDescriptorHeapView(GpuDescriptorHeapView* v) noexcept {
    if (v->_heapView.IsValid()) {
        v->_allocator->Destroy(v->_heapView);
        v->_heapView = DescriptorHeapView::Invalid();
        v->_allocator = nullptr;
    }
}

GpuDescriptorHeapView::GpuDescriptorHeapView(
    DescriptorHeapView heapView,
    GpuDescriptorAllocator* allocator,
    ResourceType type) noexcept
    : _heapView(heapView),
      _allocator(allocator),
      _type(type) {}

GpuDescriptorHeapView::~GpuDescriptorHeapView() noexcept {
    DestroyGpuDescriptorHeapView(this);
}

bool GpuDescriptorHeapView::IsValid() const noexcept {
    return _heapView.IsValid();
}

void GpuDescriptorHeapView::Destroy() noexcept {
    DestroyGpuDescriptorHeapView(this);
}

void GpuDescriptorHeapView::SetResources(uint32_t start, std::span<ResourceView*> views) noexcept {
    if (views.size() == 0) {
        return;
    }
    if (views.size() > _heapView.Length - start) {
        RADRAY_WARN_LOG("d3d12 set desc view out of range. all {}, start {}, input {}", _heapView.Length, start, views.size());
    }
    size_t len = std::min(_heapView.Length, (UINT)views.size());
    for (size_t i = 0; i < len; i++) {
        auto view = views[i];
        auto rawBase = static_cast<ResourceViewD3D12*>(view);
        switch (rawBase->GetViewType()) {
            case ResourceView::Type::Buffer: {
                auto raw = static_cast<BufferViewD3D12*>(rawBase);
                raw->CopyTo(_heapView.Heap, _heapView.Start + start + i);
                break;
            }
            case ResourceView::Type::Texture: {
                auto raw = static_cast<TextureViewD3D12*>(rawBase);
                raw->CopyTo(_heapView.Heap, _heapView.Start + start + i);
                break;
            }
        }
    }
}

}  // namespace radray::render::d3d12
