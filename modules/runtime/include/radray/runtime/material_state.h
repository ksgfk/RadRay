#pragma once

#include <optional>

#include <radray/render/rhi.h>

namespace radray {

struct MaterialPrimitiveState {
    render::FrontFace FaceClockwise{render::PrimitiveState::Default().FaceClockwise};
    render::CullMode Cull{render::PrimitiveState::Default().Cull};
    render::PolygonMode Poly{render::PrimitiveState::Default().Poly};
    bool UnclippedDepth{render::PrimitiveState::Default().UnclippedDepth};
    bool Conservative{render::PrimitiveState::Default().Conservative};

    friend bool operator==(const MaterialPrimitiveState&, const MaterialPrimitiveState&) = default;
};

struct MaterialDepthStencilState {
    render::CompareFunction DepthCompare{render::DepthStencilState::Default().DepthCompare};
    render::DepthBiasState DepthBias{render::DepthStencilState::Default().DepthBias};
    std::optional<render::StencilState> Stencil{render::DepthStencilState::Default().Stencil};
    bool DepthTestEnable{render::DepthStencilState::Default().DepthTestEnable};
    bool DepthWriteEnable{render::DepthStencilState::Default().DepthWriteEnable};

    friend bool operator==(const MaterialDepthStencilState&, const MaterialDepthStencilState&) = default;
};

struct MaterialPipelineState {
    MaterialPrimitiveState Primitive{};
    MaterialDepthStencilState DepthStencil{};
    std::optional<render::BlendState> Blend{};
    render::ColorWrites WriteMask{render::ColorWrite::All};

    friend bool operator==(const MaterialPipelineState&, const MaterialPipelineState&) = default;
};

}  // namespace radray
