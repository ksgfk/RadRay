#pragma once

#include <radray/runtime/render_framework/render_scene.h>
#include <radray/render/rhi.h>
#include <radray/runtime/gpu_resource.h>

namespace radray {

class AppFrameContext;
class ICmdAllocator;
struct PipelineContext;

struct SceneObjectGpuData {
    array<float, 16> LocalToWorld;
};
static_assert(sizeof(SceneObjectGpuData) == 64);

/// Borrowed until this flight completes. Consumers leave the buffer in ShaderRead state.
struct SceneGpuView {
    render::ShaderBufferBinding Objects;
    uint64_t BufferGeneration;
};

/// RT-owned mirror; each physical flight buffer commits state only after completion feedback.
class SceneGpuData {
public:
    SceneGpuData(render::Device* device, uint32_t flightCount);
    void ApplyChanges(const SceneApplyChanges& changes);
    void Complete(uint32_t flightIndex, uint64_t serial, bool executed, const RenderScene& scene);
    std::optional<SceneGpuView> Prepare(const RenderScene& scene, AppFrameContext& frame);
    std::optional<SceneGpuView> Prepare(const RenderScene& scene, PipelineContext& context, vector<render::CommandBuffer*>& outCommands);

private:
    std::optional<SceneGpuView> Prepare(const RenderScene& scene, uint32_t flightIndex, uint64_t frameSerial, ICmdAllocator& allocator, ResourceUploader& uploader, vector<render::CommandBuffer*>& outCommands);
    struct ObjectSet {
        vector<ShapeId> Ids;
        vector<uint32_t> Positions;
        void Add(ShapeId id);
        void Remove(ShapeId id);
        void Clear();
    };
    struct Flight {
        unique_ptr<render::Buffer> Buffer;
        uint64_t Generation{0};
        uint64_t PreparedSerial{0};
        uint64_t AttemptSerial{0};
        uint64_t AttemptGeneration{0};
        bool Initialized{false};
        bool PreparedValid{false};
        ObjectSet Pending;
        vector<ShapeId> Attempt;
    };
    render::Device* _device;
    vector<Flight> _flights;
    vector<ShapeId> _sorted;
    vector<SceneObjectGpuData> _packed;
    vector<BufferUploadRequest> _uploads;
};

}  // namespace radray
