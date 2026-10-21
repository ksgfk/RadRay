#pragma once

#include <radray/render/backend/d3d12_impl.h>
#include <radray/render/pipeline_layout_cache.h>

namespace radray::render::d3d12 {

struct PipelineLayoutKeyHashD3D12 {
    size_t operator()(const vector<byte>& key) const noexcept;
};

class CachedPipelineLayoutD3D12 final : public CachedPipelineLayout {
public:
    CachedPipelineLayoutD3D12(PipelineLayoutCacheD3D12* cache, ComPtr<ID3D12RootSignature> rootSignature) noexcept;
    ~CachedPipelineLayoutD3D12() noexcept override;

    ComPtr<ID3D12RootSignature> RootSignature;
    const vector<byte>* Key{nullptr};

private:
    friend void IntrusivePtrAddRef(CachedPipelineLayoutD3D12* layout) noexcept;
    friend void IntrusivePtrRelease(CachedPipelineLayoutD3D12* layout) noexcept;
    PipelineLayoutCacheD3D12* _cache;
};

class PipelineLayoutCacheD3D12 final : public PipelineLayoutCache {
public:
    explicit PipelineLayoutCacheD3D12(DeviceD3D12* device) noexcept;
    ~PipelineLayoutCacheD3D12() noexcept override;

    IntrusivePtr<CachedPipelineLayoutD3D12> GetOrCreate(std::span<const byte> serialized) noexcept;
    size_t GetEntryCount() const noexcept override;

private:
    friend void IntrusivePtrRelease(CachedPipelineLayoutD3D12* layout) noexcept;
    void Evict(CachedPipelineLayout* layout) noexcept override;
    DeviceD3D12* _device;
    unordered_map<vector<byte>, unique_ptr<CachedPipelineLayoutD3D12>, PipelineLayoutKeyHashD3D12> _layouts;
};

}  // namespace radray::render::d3d12
