#pragma once

// 固定的 Extract/Record 执行顺序见 docs/architecture/runtime.md。

#include <radray/runtime/frame_pipeline.h>
#include <radray/runtime/rdg.h>

namespace radray {

class DeviceContext;

class RenderGraphFramePipeline : public FramePipeline {
public:
    explicit RenderGraphFramePipeline(DeviceContext& device, uint32_t slotCount);
    ~RenderGraphFramePipeline() noexcept override;
    void Extract(const Scene& source, FrameSlot& slot) noexcept final;
    void Record(const Scene& source, FrameSlot& slot) noexcept final;

protected:
    virtual void ExtractFrameData(const Scene& source, FrameSlot& slot) noexcept = 0;
    // 执行回调可在 Record 期间借用源 Scene；清理时不得访问它。
    virtual void SetupGraph(RDGBuilder& graph, const Scene& source, FrameSlot& slot) noexcept = 0;
    virtual void ExtractMeshPass(MeshPassId pass, const Scene& source, FrameSlot& slot, MeshPassFrameDrawList& drawList) noexcept = 0;
    virtual void SubmitFrame(FrameSlot& slot, render::CommandBuffer& commandBuffer) noexcept = 0;

private:
    class SlotState;
    vector<unique_ptr<SlotState>> _slotStates;
};

}  // namespace radray
