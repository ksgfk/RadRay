#pragma once

#include <radray/runtime/frame_pipeline.h>
#include <radray/runtime/rdg.h>

namespace radray {

class DeviceContext;

// See docs/architecture/runtime.md for the fixed Extract/Record sequence.
class RenderGraphFramePipeline : public FramePipeline {
public:
    explicit RenderGraphFramePipeline(DeviceContext& device, uint32_t slotCount);
    ~RenderGraphFramePipeline() noexcept override;
    void Extract(const Scene& source, FrameSlot& slot) noexcept final;
    void Record(const Scene& source, FrameSlot& slot) noexcept final;

protected:
    virtual void ExtractFrameData(const Scene& source, FrameSlot& slot) noexcept = 0;
    // Execute callbacks may borrow source through Record, but must not access it during cleanup.
    virtual void SetupGraph(RDGBuilder& graph, const Scene& source, FrameSlot& slot) noexcept = 0;
    virtual void ExtractMeshPass(MeshPassId pass, const Scene& source, FrameSlot& slot, MeshPassFrameDrawList& drawList) noexcept = 0;
    virtual void SubmitFrame(FrameSlot& slot, render::CommandBuffer& commandBuffer) noexcept = 0;

private:
    struct SlotState;
    vector<unique_ptr<SlotState>> _slotStates;
};

}  // namespace radray
