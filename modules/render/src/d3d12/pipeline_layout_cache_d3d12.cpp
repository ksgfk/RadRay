#include "pipeline_layout_cache_d3d12.h"

namespace radray::render::d3d12 {

size_t PipelineLayoutKeyHashD3D12::operator()(const vector<byte>& key) const noexcept {
    HashCode hash;
    hash.Add(key.size());
    for (byte value : key) hash.Add(static_cast<uint8_t>(value));
    return hash.ToHashCode();
}

CachedPipelineLayoutD3D12::CachedPipelineLayoutD3D12(
    PipelineLayoutCacheD3D12* cache, ComPtr<ID3D12RootSignature> rootSignature) noexcept
    : RootSignature(std::move(rootSignature)), _cache(cache) {}

CachedPipelineLayoutD3D12::~CachedPipelineLayoutD3D12() noexcept = default;

PipelineLayoutCacheD3D12::PipelineLayoutCacheD3D12(DeviceD3D12* device) noexcept
    : _device(device) {}

PipelineLayoutCacheD3D12::~PipelineLayoutCacheD3D12() noexcept {
    Destroy();
}

size_t PipelineLayoutCacheD3D12::GetEntryCount() const noexcept {
    return _layouts.size();
}

IntrusivePtr<CachedPipelineLayoutD3D12> PipelineLayoutCacheD3D12::GetOrCreate(std::span<const byte> serialized) noexcept {
    if (IsClosed() || _device->_device == nullptr || serialized.empty()) return nullptr;
    vector<byte> key(serialized.begin(), serialized.end());
    if (const auto it = _layouts.find(key); it != _layouts.end()) return RetainRef(it->second.get());

    ComPtr<ID3D12RootSignature> rootSignature;
    if (HRESULT hr = _device->_device->CreateRootSignature(
            0, serialized.data(), serialized.size(), IID_PPV_ARGS(rootSignature.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateRootSignature failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    const auto [it, inserted] = _layouts.try_emplace(
        std::move(key), make_unique<CachedPipelineLayoutD3D12>(this, std::move(rootSignature)));
    RADRAY_ASSERT(inserted);
    it->second->Key = &it->first;
    return RetainRef(it->second.get());
}

void PipelineLayoutCacheD3D12::Evict(CachedPipelineLayout* layout) noexcept {
    auto* entry = static_cast<CachedPipelineLayoutD3D12*>(layout);
    const auto it = _layouts.find(*entry->Key);
    RADRAY_ASSERT(it != _layouts.end() && it->second.get() == entry && entry->GetRefCount() == 0);
    _layouts.erase(it);
}

}  // namespace radray::render::d3d12
