#include <radray/runtime/gpu_frame_resources.h>

#include <algorithm>
#include <numeric>
#include <radray/logger.h>
#include <radray/runtime/render_system.h>

namespace radray {
GpuFrameResources::GpuFrameResources(render::Device* device, ResourceUploader& uploader, HostWriteBatch& hostWrites)
    : _device(device), _uploader(uploader), _hostWrites(hostWrites),
      _constants(device, &hostWrites, DynamicCBufferArena::Descriptor{.Alignment = std::max(uint64_t{1}, device->GetCapabilities().Limits.CBufferOffsetAlignment)}) {}

MappedUploadPage::Reservation GpuFrameResources::AllocateConstants(uint64_t size) { return _constants.Reserve(size); }
MappedUploadPage::Reservation GpuFrameResources::AllocateStorage(uint64_t size, uint32_t structureByteStride) {
    if (!size || !structureByteStride) return {};
    const auto deviceAlignment = std::max(uint64_t{4}, _device->GetCapabilities().Limits.StorageBufferOffsetAlignment);
    const auto divisor = std::gcd(deviceAlignment, uint64_t{structureByteStride});
    if (deviceAlignment / divisor > UINT64_MAX / structureByteStride) return {};
    const auto alignment = deviceAlignment / divisor * structureByteStride;
    for (; _storagePage < _storagePages.size(); ++_storagePage) {
        auto& page = *_storagePages[_storagePage];
        const auto remainder = page.GetUsed() % alignment;
        const auto padding = remainder ? alignment - remainder : 0;
        const auto remaining = page.GetCapacity() - page.GetUsed();
        if (padding > remaining || size > remaining - padding) continue;
        if (padding) {
            auto gap = page.Reserve(padding, 1, _hostWrites);
            gap.Commit(0);
        }
        return page.Reserve(size, 1, _hostWrites);
    }
    if (size > UINT64_MAX - alignment) return {};
    const auto limit = _device->GetCapabilities().Limits.MaxBufferSize;
    const auto capacity = std::max(uint64_t{1024 * 1024}, (size + alignment - 1) / alignment * alignment);
    if (limit && capacity > limit) return {};
    auto buffer = _device->CreateBuffer({capacity, render::MemoryType::Upload,
                                         render::BufferUse::Resource | render::BufferUse::MapWrite, render::ResourceHint::PersistentMap});
    if (!buffer) return {};
    _storagePages.push_back(make_unique<MappedUploadPage>(buffer.Release()));
    return _storagePages.back()->Reserve(size, 1, _hostWrites);
}

Nullable<render::ShaderParameterSet*> GpuFrameResources::AllocateParameters(render::PipelineLayout* layout, uint32_t group, shared_ptr<RenderServiceGeneration> generation) {
    if (generation && !generation->Alive) return nullptr;
    auto& pool = _parameters[layout][group];
    if (pool.Generation && pool.Generation != generation) {
        if (pool.Used) {
            RADRAY_ERR_LOG("Cannot replace a parameter layout service generation while this flight still borrows its sets");
            return nullptr;
        }
        pool = {};
    }
    pool.Generation = std::move(generation);
    if (pool.Used < pool.Sets.size()) return pool.Sets[pool.Used++].get();
    auto set = _device->CreateShaderParameterSet({layout, group});
    if (!set) return nullptr;
    pool.Sets.push_back(set.Release());
    return pool.Sets[pool.Used++].get();
}

void GpuFrameResources::ResetForReuse() {
    _constants.Reset();
    size_t used = 0;
    for (size_t i = 0; i < _storagePages.size(); ++i)
        if (_storagePages[i]->GetUsed()) used = i + 1;
    if (used < _storagePages.size() && used <= _storagePages.size() / 2)
        ++_storageIdle;
    else
        _storageIdle = 0;
    if (_storageIdle >= 60) {
        _storagePages.resize(used);
        _storageIdle = 0;
    }
    for (auto& page : _storagePages) page->Reset();
    _storagePage = 0;
    for (auto it = _parameters.begin(); it != _parameters.end();) {
        auto& groups = it->second;
        for (auto item = groups.begin(); item != groups.end();) {
            auto& pool = item->second;
            if (pool.Generation && !pool.Generation->Alive) {
                item = groups.erase(item);
                continue;
            }
            if (pool.Used < pool.Sets.size() && pool.Used <= pool.Sets.size() / 2)
                ++pool.Idle;
            else
                pool.Idle = 0;
            if (pool.Idle >= 60) {
                pool.Sets.resize(pool.Used);
                pool.Idle = 0;
            }
            pool.Used = 0;
            if (pool.Sets.empty())
                item = groups.erase(item);
            else
                ++item;
        }
        if (groups.empty())
            it = _parameters.erase(it);
        else
            ++it;
    }
}
}  // namespace radray
