#pragma once

#include <radray/render/root_signature.h>
#include <radray/render/descriptor_set.h>
#include "d3d12_helper.h"
#include "d3d12_descriptor_heap.h"

namespace radray::render::d3d12 {

class RootSigD3D12 : public RootSignature {
public:
    explicit RootSigD3D12(ComPtr<ID3D12RootSignature>) noexcept;
    ~RootSigD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return _rootSig.Get() != nullptr; }
    void Destroy() noexcept override;

    std::span<const RootConstantInfo> GetRootConstants() const noexcept override;

    std::span<const RootDescriptorInfo> GetRootDescriptors() const noexcept override;

    std::span<const DescriptorSetElementInfo> GetBindDescriptors() const noexcept override;

public:
    ComPtr<ID3D12RootSignature> _rootSig;
    radray::vector<RootConstantInfo> _rootConstants;
    radray::vector<RootDescriptorInfo> _rootDescriptors;
    radray::vector<DescriptorSetElementInfo> _bindDescriptors;
    UINT _rootConstStart;
    UINT _rootDescStart;
    UINT _bindDescStart;
};

class GpuDescriptorHeapView : public DescriptorSet {
public:
    GpuDescriptorHeapView(
        DescriptorHeapView heapView,
        GpuDescriptorAllocator* allocator,
        ResourceType type) noexcept;
    ~GpuDescriptorHeapView() noexcept override;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;

    DescriptorHeapView _heapView;
    GpuDescriptorAllocator* _allocator;
    ResourceType _type;
};

}  // namespace radray::render::d3d12
