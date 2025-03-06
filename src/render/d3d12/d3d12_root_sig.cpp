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
    if (v->_heap) {
        v->_heap->RecycleRange(v->_start, v->_count);
        v->_heap = nullptr;
    }
}

GpuDescriptorHeapView::GpuDescriptorHeapView(
    DescriptorHeap* heap,
    ResourceType type,
    uint32_t start,
    uint32_t count) noexcept
    : _heap(heap),
      _type(type),
      _start(start),
      _count(count) {}

GpuDescriptorHeapView::~GpuDescriptorHeapView() noexcept {
    DestroyGpuDescriptorHeapView(this);
}

bool GpuDescriptorHeapView::IsValid() const noexcept {
    return _heap != nullptr;
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
