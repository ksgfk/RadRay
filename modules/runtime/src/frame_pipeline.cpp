#include <radray/runtime/frame_pipeline.h>

namespace radray {

FramePipeline::~FramePipeline() noexcept = default;

void FramePipeline::OnSlotCompleted(FrameSlot&) noexcept {}

std::span<const MeshPassRegistration> FramePipeline::GetMeshPassRegistrations() noexcept {
    return {};
}

}  // namespace radray
