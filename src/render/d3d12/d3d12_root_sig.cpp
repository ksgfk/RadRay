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

// void GpuDescriptorHeapView::SetResources(std::span<ResourceView*> views) noexcept {
//     for (ResourceView* rv : views) {
//         auto type = rv->GetViewType();
//         if (type == ResourceView::Type::Buffer) {
//             // TODO:
//             // BufferViewD3D12* bv = static_cast<BufferViewD3D12*>(rv);
//             // const auto& desc = bv->_desc;
//             // desc.heap->CopyTo(desc.heapIndex, 1, _shaderResHeap.Value(), _shaderResStart);
//         }
//     }
// }

}  // namespace radray::render::d3d12
