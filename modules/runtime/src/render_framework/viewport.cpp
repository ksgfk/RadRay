#include <radray/runtime/render_framework/viewport.h>

namespace radray {

Viewport MakeViewport(
    render::RenderBackend backend,
    float x,
    float y,
    float width,
    float height,
    float minDepth,
    float maxDepth) noexcept {
    if (backend == render::RenderBackend::Vulkan) {
        return Viewport{x, y + height, width, -height, minDepth, maxDepth};
    }
    return Viewport{x, y, width, height, minDepth, maxDepth};
}

}  // namespace radray
