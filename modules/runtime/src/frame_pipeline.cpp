#include <radray/runtime/frame_pipeline.h>

namespace radray {

FramePipeline::~FramePipeline() noexcept = default;

void FramePipeline::OnSlotRetired(FrameSlot&, bool) noexcept {}

std::span<const MeshPassRegistration> FramePipeline::GetMeshPassRegistrations() noexcept {
    return {};
}

}  // namespace radray
