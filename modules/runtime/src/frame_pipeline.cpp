#include <radray/runtime/frame_pipeline.h>

namespace radray {

FramePipeline::~FramePipeline() noexcept = default;

std::span<const MeshPassRegistration> FramePipeline::GetMeshPassRegistrations() noexcept {
    return {};
}

}  // namespace radray
