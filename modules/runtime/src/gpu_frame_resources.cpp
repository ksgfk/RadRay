#include <radray/runtime/gpu_frame_resources.h>

#include <algorithm>

namespace radray {
GpuFrameResources::GpuFrameResources(render::Device* device, ResourceUploader& uploader, HostWriteBatch& hostWrites)
    : _device(device), _uploader(uploader), _hostWrites(hostWrites),
      _constants(device, &hostWrites, DynamicCBufferArena::Descriptor{.Alignment = std::max(uint64_t{1}, device->GetCapabilities().Limits.CBufferOffsetAlignment)}) {}

MappedUploadPage::Reservation GpuFrameResources::AllocateConstants(uint64_t size) { return _constants.Reserve(size); }

Nullable<render::ShaderParameterSet*> GpuFrameResources::AllocateParameters(render::PipelineLayout* layout, uint32_t group) {
    auto& pool = _parameters[layout][group];
    if (pool.Used < pool.Sets.size()) return pool.Sets[pool.Used++].get();
    auto set = _device->CreateShaderParameterSet({layout, group});
    if (!set) return nullptr;
    pool.Sets.push_back(set.Release());
    return pool.Sets[pool.Used++].get();
}

void GpuFrameResources::ResetForReuse() {
    _constants.Reset();
    for (auto& [layout, groups] : _parameters)
        for (auto& [group, pool] : groups) pool.Used = 0;
}
}  // namespace radray
