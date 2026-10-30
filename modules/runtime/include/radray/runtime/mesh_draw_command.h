#pragma once

#include <radray/render/rhi.h>

namespace radray {

// See docs/architecture/runtime.md for ownership and construction boundaries.
// Geometry binding descriptions are owned; RHI objects are borrowed through GPU completion.
// This version supports direct indexed draws with one instance, starting at instance zero.
struct MeshDrawCommand {
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    vector<render::VertexBufferBinding> VertexBuffers;
    render::IndexBufferView IndexBuffer{};
    uint32_t IndexCount{0};
    uint32_t FirstIndex{0};
    int32_t VertexOffset{0};
};

}  // namespace radray
