#pragma once

// 固定的 Extract/Prepare/Record/Submit 执行顺序见 docs/architecture/runtime.md。

#include <radray/gpu/types.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/types.h>
#include <radray/runtime/frame_pipeline.h>
#include <radray/runtime/rdg.h>
#include <radray/runtime/gpu_device.h>

namespace radray {

enum class RDGGpuProfilingMode { Off, FrameOnly, FrameAndPasses };

enum class RDGGpuTimingStatus { Valid, Off, Unavailable, AllocationFailed, DeviceLost };

struct RDGGpuPassTiming {
    string Name;
    uint64_t Nanoseconds{};
};

struct RDGGpuTimingSnapshot {
    uint64_t FrameId{};
    uint64_t UserSequence{};
    uint64_t TotalNanoseconds{};
    vector<RDGGpuPassTiming> Passes;
    RDGGpuTimingStatus Status{RDGGpuTimingStatus::Off};
};

class RenderGraphFramePipeline : public FramePipeline {
public:
    explicit RenderGraphFramePipeline(DeviceContext& device, uint32_t slotCount, uint64_t uploadPageSize = 1024 * 1024);
    ~RenderGraphFramePipeline() noexcept override;
    void Extract(FrameSlot& slot) noexcept final;
    void Prepare(FrameSlot& slot) noexcept final;
    void Record(FrameSlot& slot) noexcept final;
    bool Submit(FrameSlot& slot) noexcept final;
    void OnSlotRetired(FrameSlot& slot, bool deviceLost) noexcept final;
    // BeginFrame 后设置，仅应用于本次提交。
    void SetGpuProfiling(FrameSlot& slot, RDGGpuProfilingMode mode, uint64_t userSequence) noexcept;
    // 快照属于已完成的槽，有效至该槽下一次 Prepare。
    Nullable<const RDGGpuTimingSnapshot*> GetCompletedGpuTiming(const FrameSlot& slot) const noexcept;

protected:
    virtual void OnFrameSlotRetired(FrameSlot& slot, const RDGGpuTimingSnapshot& snapshot) noexcept;
    virtual void ExtractFrameData(FrameSlot& slot) noexcept = 0;
    // 执行回调可在 Record 期间借用管线输入；清理时不得访问它。
    virtual void SetupGraph(RDGBuilder& graph, FrameSlot& slot) noexcept = 0;
    virtual void ExtractMeshPass(MeshPassId pass, FrameSlot& slot, MeshPassFrameDrawList& drawList) noexcept;
    virtual bool SubmitFrame(FrameSlot& slot, RDGSubmitPlan& plan) noexcept;

private:
    class SlotState;
    RDGDeviceConstraints _constraints;
    vector<unique_ptr<SlotState>> _slotStates;
};

}  // namespace radray
