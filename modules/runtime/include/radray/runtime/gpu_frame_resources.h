#pragma once

#include <radray/runtime/gpu_resource.h>

namespace radray {
class GpuSystem;
struct RenderServiceGeneration;

/// Borrowed frame services. Allocation cursors are reset only at safe flight reuse.
class GpuFrameResources {
public:
    GpuFrameResources(render::Device* device, ResourceUploader& uploader, HostWriteBatch& hostWrites);
    render::Device* GetDevice() const noexcept { return _device; }
    ResourceUploader& GetUploader() const noexcept { return _uploader; }
    HostWriteBatch& GetHostWrites() const noexcept { return _hostWrites; }
    MappedUploadPage::Reservation AllocateConstants(uint64_t size);
    MappedUploadPage::Reservation AllocateStorage(uint64_t size, uint32_t structureByteStride = 4);
    /// Each call produces a distinct parameter version for this frame; caller fills the entire group.
    Nullable<render::ShaderParameterSet*> AllocateParameters(render::PipelineLayout* layout, uint32_t group, shared_ptr<RenderServiceGeneration> generation = {});

private:
    friend class GpuSystem;
    void ResetForReuse();
    struct ParameterPool {
        vector<unique_ptr<render::ShaderParameterSet>> Sets;
        size_t Used{0};
        uint32_t Idle{0};
        shared_ptr<RenderServiceGeneration> Generation;
    };
    render::Device* _device;
    ResourceUploader& _uploader;
    HostWriteBatch& _hostWrites;
    DynamicCBufferArena _constants;
    vector<unique_ptr<MappedUploadPage>> _storagePages;
    size_t _storagePage{0};
    uint32_t _storageIdle{0};
    unordered_map<render::PipelineLayout*, unordered_map<uint32_t, ParameterPool>> _parameters;
};
}  // namespace radray
