#pragma once

#include <radray/render/rhi.h>

namespace radray {

Viewport MakeViewport(
    render::RenderBackend backend,
    float x,
    float y,
    float width,
    float height,
    float minDepth = 0.0f,
    float maxDepth = 1.0f) noexcept;

inline Viewport MakeViewport(
    render::RenderBackend backend,
    uint32_t width,
    uint32_t height) noexcept {
    return MakeViewport(
        backend,
        0.0f,
        0.0f,
        static_cast<float>(width),
        static_cast<float>(height));
}

}  // namespace radray
