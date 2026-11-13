#pragma once

// 固定的 Extract/Prepare/Record/Submit 执行顺序见 docs/architecture/runtime.md。

#include <radray/runtime/frame_pipeline.h>
#include <radray/runtime/rdg.h>

namespace radray {

class DeviceContext;

class RenderGraphFramePipeline : public FramePipeline {
public:
    explicit RenderGraphFramePipeline(DeviceContext& device, uint32_t slotCount, uint64_t uploadPageSize = 1024 * 1024);
    ~RenderGraphFramePipeline() noexcept override;
    void Extract(FrameSlot& slot) noexcept final;
    void Prepare(FrameSlot& slot) noexcept final;
    void Record(FrameSlot& slot) noexcept final;
    void Submit(FrameSlot& slot) noexcept final;

protected:
    virtual void ExtractFrameData(FrameSlot& slot) noexcept = 0;
    // 执行回调可在 Record 期间借用管线输入；清理时不得访问它。
    virtual void SetupGraph(RDGBuilder& graph, FrameSlot& slot) noexcept = 0;
    virtual void ExtractMeshPass(MeshPassId pass, FrameSlot& slot, MeshPassFrameDrawList& drawList) noexcept;
    virtual void SubmitFrame(FrameSlot& slot, render::CommandBuffer& commandBuffer) noexcept = 0;

private:
    class SlotState;
    RDGDeviceConstraints _constraints;
    vector<unique_ptr<SlotState>> _slotStates;
};

}  // namespace radray
