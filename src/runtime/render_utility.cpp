#include <radray/runtime/render_utility.h>

namespace radray::runtime {

ConstantBufferPool::ConstantBufferPool(render::Device* device, uint64_t initSize) noexcept
    : _device(device),
      _initSize(initSize) {}

ConstantBufferPool::Ref ConstantBufferPool::Allocate(uint64_t size) noexcept {
    for (auto&& i : _blocks) {
        uint64_t freeSize = i.capacity - i.size;
        if (freeSize >= size) {
            uint64_t offset = i.size;
            i.size += size;
            return {i.buf.get(), offset};
        }
    }
    uint64_t allocSize = std::max<uint64_t>(size, _initSize);
    shared_ptr<render::Buffer> buf = CreateCBuffer(allocSize);
    _blocks.emplace_back(ConstantBufferPool::Block{
        std::move(buf),
        allocSize,
        size});
    return {_blocks.back().buf.get(), 0};
}

void ConstantBufferPool::Clear() noexcept {
    if (_blocks.size() == 1) {
        _blocks[0].size = 0;
    } else {
        uint64_t sumSize = 0u;
        for (auto&& i : _blocks) {
            sumSize += i.capacity;
        }
        _blocks.clear();
        shared_ptr<render::Buffer> buf = CreateCBuffer(sumSize);
        _blocks.emplace_back(ConstantBufferPool::Block{
            std::move(buf),
            sumSize,
            0});
    }
}

shared_ptr<render::Buffer> ConstantBufferPool::CreateCBuffer(uint64_t size) noexcept {
    return _device->CreateBuffer(
                      size,
                      render::ResourceType::CBuffer,
                      render::ResourceUsage::Upload,
                      ToFlag(render::ResourceState::GenericRead),
                      ToFlag(render::ResourceMemoryTip::None))
        .Unwrap();
}

DescriptorSetPool::DescriptorSetPool(render::Device* device) noexcept
    : _device(device) {}

shared_ptr<render::DescriptorSet> DescriptorSetPool::GetOrCreateDescriptorSet(
    render::RootSignature* rootSig,
    uint32_t set,
    std::span<render::ResourceView*> views) noexcept {
    DescriptorSetPool::PoolKey key{{views.begin(), views.end()}};
    auto [iter, isInsert] = _pool.try_emplace(key, nullptr);
    if (isInsert) {
        auto descSet = _device->CreateDescriptorSet(rootSig, set).Unwrap();
        iter->second = descSet;
    }
    return iter->second;
}

size_t DescriptorSetPool::PoolKey::operator()() const noexcept {
    return HashData(views.data(), views.size() * sizeof(render::ResourceView*));
}

bool DescriptorSetPool::PoolKey::operator==(const PoolKey& rhs) const noexcept {
    return views.size() == rhs.views.size() && std::equal(views.begin(), views.end(), rhs.views.begin());
}

ResourceTable::ResourceTable(render::RootSignature* rootSig) noexcept {
    size_t cbSize = 0;
    for (uint32_t i = 0, all = rootSig->GetRootConstantCount(); i < all; i++) {
        render::RootSignatureRootConstantSlotInfo slotInfo = rootSig->GetRootConstantSlotInfo(i);
        auto [exist, isInsert] = _slots.try_emplace(slotInfo.Name, RootConstSlot{slotInfo, 0});
        if (isInsert) {
            auto& rcSlot = std::get<RootConstSlot>(exist->second);
            rcSlot.CbCacheStart = cbSize;
            cbSize += rcSlot.Info.Size;
        } else {
            RADRAY_ERR_LOG("Material exist root constant slot: {}", exist->first);
        }
    }
    for (uint32_t i = 0, all = rootSig->GetConstantBufferSlotCount(); i < all; i++) {
        render::RootSignatureConstantBufferSlotInfo slotInfo = rootSig->GetConstantBufferSlotInfo(i);
        auto [exist, isInsert] = _slots.try_emplace(slotInfo.Name, CBufferSlot{slotInfo, 0});
        if (isInsert) {
            auto& cbSlot = std::get<CBufferSlot>(exist->second);
            cbSlot.CbCacheStart = cbSize;
            cbSize += cbSlot.Info.Size;
        } else {
            RADRAY_ERR_LOG("Material exist constant buffer slot: {}", exist->first);
        }
    }
    for (uint32_t i = 0, all = rootSig->GetDescriptorSetCount(); i < all; i++) {
        radray::vector<render::DescriptorLayout> descLayouts = rootSig->GetDescriptorSetLayout(i);
        for (render::DescriptorLayout& descLayout : descLayouts) {
            size_t v = _descLayouts.size();
            auto [exist, isInsert] = _slots.try_emplace(
                descLayout.Name,
                DescriptorLayoutIndex{
                    v,
                    0,
                    {descLayout.Count, weak_ptr<render::ResourceView>{}}});
            if (isInsert) {
                auto& descIndex = std::get<DescriptorLayoutIndex>(exist->second);
                if (descLayout.Type == render::ShaderResourceType::CBuffer) {
                    descIndex.CbCacheStart = cbSize;
                    cbSize += descLayout.CbSize * descLayout.Count;
                }
            } else {
                RADRAY_ERR_LOG("Material exist descriptor slot: {}", exist->first);
            }
            _descLayouts.emplace_back(std::move(descLayout));
        }
    }
    _descLayouts.shrink_to_fit();
    _cbCache.Allocate(cbSize);
}

void ResourceTable::SetConstantBufferData(std::string_view name, uint32_t index, std::span<const byte> data) noexcept {
    auto iter = _slots.find(name);
    if (iter == _slots.end()) {
        RADRAY_ERR_LOG("Material not found slot: {}", name);
        return;
    }
    std::visit(
        [this, index, data](auto&& v) noexcept {
            auto copyImpl = [](std::span<const byte> src, std::span<byte> dst, std::string_view tips) noexcept {
                if (dst.size() < src.size()) {
                    RADRAY_ERR_LOG("Material value {} size not match", tips);
                } else {
                    std::copy(src.begin(), src.end(), dst.begin());
                }
            };
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, DescriptorLayoutIndex>) {
                render::DescriptorLayout& layout = _descLayouts[v.Index];
                if (index >= layout.Count) {
                    RADRAY_ERR_LOG("Material value {} index out of range", layout.Name);
                    return;
                }
                if (layout.Type == render::ShaderResourceType::CBuffer) {
                    size_t start = v.CbCacheStart + layout.CbSize * index;
                    std::span<byte> dst = _cbCache.GetSpan(start, layout.CbSize);
                    copyImpl(data, dst, layout.Name);
                } else {
                    RADRAY_ERR_LOG("Material value {} is not cbuffer", layout.Name);
                }
            } else {
                std::span<byte> dst = _cbCache.GetSpan(v.CbCacheStart, v.Info.Size);
                copyImpl(data, dst, v.Info.Name);
            }
        },
        iter->second);
}

void ResourceTable::SetResource(std::string_view name, uint32_t index, shared_ptr<render::ResourceView> rv) noexcept {
    auto iter = _slots.find(name);
    if (iter == _slots.end()) {
        RADRAY_ERR_LOG("Material not found slot: {}", name);
        return;
    }
    if (std::holds_alternative<DescriptorLayoutIndex>(iter->second)) {
        DescriptorLayoutIndex& v = std::get<DescriptorLayoutIndex>(iter->second);
        const render::DescriptorLayout& layout = _descLayouts[v.Index];
        if (index >= v.Views.size()) {
            RADRAY_ERR_LOG("Material value {} index out of range", layout.Name);
            return;
        }
        v.Views[index] = rv;
    } else {
        RADRAY_ERR_LOG("Material value {} is not descriptor", name);
    }
}

}  // namespace radray::runtime
