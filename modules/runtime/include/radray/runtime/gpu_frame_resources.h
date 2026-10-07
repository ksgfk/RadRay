#pragma once

#include <radray/runtime/gpu_resource.h>

namespace radray {
class GpuSystem;

/// Borrowed frame services. Allocation cursors are reset only at safe flight reuse.
class GpuFrameResources {
public:
    GpuFrameResources(render::Device* device, ResourceUploader& uploader, HostWriteBatch& hostWrites);
    render::Device* GetDevice() const noexcept { return _device; }
    ResourceUploader& GetUploader() const noexcept { return _uploader; }
    HostWriteBatch& GetHostWrites() const noexcept { return _hostWrites; }
    MappedUploadPage::Reservation AllocateConstants(uint64_t size);
    /// Each call produces a distinct parameter version for this frame; caller fills the entire group.
    Nullable<render::ShaderParameterSet*> AllocateParameters(render::PipelineLayout* layout, uint32_t group);

private:
    friend class GpuSystem;
    void ResetForReuse();
    struct ParameterPool {
        vector<unique_ptr<render::ShaderParameterSet>> Sets;
        size_t Used{0};
    };
    render::Device* _device;
    ResourceUploader& _uploader;
    HostWriteBatch& _hostWrites;
    DynamicCBufferArena _constants;
    unordered_map<render::PipelineLayout*, unordered_map<uint32_t, ParameterPool>> _parameters;
};
}  // namespace radray
