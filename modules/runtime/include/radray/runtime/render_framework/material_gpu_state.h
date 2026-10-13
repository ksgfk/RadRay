#pragma once
#include <radray/runtime/gpu_resource.h>

namespace radray {
struct RasterMaterialPlan;
struct RenderServiceGeneration;
struct MaterialRenderData;
struct MaterialPass;
/// RT default bindings for one immutable material version; no back reference to its asset.
class MaterialGpuState {
public:
    MaterialGpuState();
    ~MaterialGpuState() noexcept;
    MaterialGpuState(const MaterialGpuState&) = delete;
    MaterialGpuState& operator=(const MaterialGpuState&) = delete;

private:
    friend class RasterDraw;
    struct Group {
        uint32_t Index;
        unique_ptr<render::ShaderParameterSet> Parameters;
        vector<render::ShaderParameterDynamicOffset> Offsets;
    };
    struct Pass {
        Nullable<const MaterialPass*> Source;
        shared_ptr<const RasterMaterialPlan> Plan;
        vector<Group> Groups;
    };
    shared_ptr<RenderServiceGeneration> _generation;
    Nullable<const MaterialRenderData*> _data{nullptr};
    vector<std::optional<PersistentCBufferArena::Allocation>> _constants;
    vector<Pass> _passes;
};
}  // namespace radray
