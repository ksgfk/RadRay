#pragma once

#include <radray/render/common.h>

namespace radray::render {

class RootSignature;
class Shader;

class GraphicsPipelineStateDescriptor {
public:
    std::string Name;
    RootSignature* RootSig;
    Shader* VS;
    Shader* PS;
    std::vector<VertexBufferLayout> VertexBuffers;
    PrimitiveState Primitive;
    DepthStencilState DepthStencil;
    MultiSampleState MultiSample;
    std::vector<ColorTargetState> ColorTargets;
    bool DepthStencilEnable;
};

class PipelineState : public RenderBase {
public:
    ~PipelineState() noexcept override = default;
};

class GraphicsPipelineState : public PipelineState {
public:
    ~GraphicsPipelineState() noexcept override = default;
};

}  // namespace radray::render
