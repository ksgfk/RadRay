#pragma once

#include <radray/render/common.h>

namespace radray::render {

class RootSignature;
class Shader;

struct VertexElement {
    uint64_t Offset;
    VertexSemantic Semantic;
    uint32_t SemanticIndex;
    VertexFormat Format;
    uint32_t Location;
};

class VertexBufferLayout {
public:
    uint64_t ArrayStride;
    VertexStepMode StepMode;
    std::vector<VertexElement> Elements;
};

struct PrimitiveState {
    PrimitiveTopology Topology;
    IndexFormat StripIndexFormat;
    FrontFace FaceClockwise;
    CullMode Cull;
    PolygonMode Poly;
    bool UnclippedDepth;
    bool Conservative;
};

struct StencilFaceState {
    CompareFunction Compare;
    StencilOperation FailOp;
    StencilOperation DepthFailOp;
    StencilOperation PassOp;
};

struct StencilState {
    StencilFaceState Front;
    StencilFaceState Back;
    uint32_t ReadMask;
    uint32_t WriteMask;
};

struct DepthBiasState {
    int32_t Constant;
    float SlopScale;
    float Clamp;
};

struct DepthStencilState {
    TextureFormat Format;
    CompareFunction DepthCompare;
    StencilState Stencil;
    DepthBiasState DepthBias;
    bool DepthWriteEnable;
    bool StencilEnable;
};

struct MultiSampleState {
    uint32_t Count;
    uint64_t Mask;
    bool AlphaToCoverageEnable;
};

struct BlendComponent {
    BlendFactor Src;
    BlendFactor Dst;
    BlendOperation Op;
};

struct BlendState {
    BlendComponent Color;
    BlendComponent Alpha;
};

struct ColorTargetState {
    TextureFormat Format;
    BlendState Blend;
    ColorWrites WriteMask;
    bool BlendEnable;
};

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

class RootSignature : public RenderBase {
public:
    virtual ~RootSignature() noexcept = default;
};

}  // namespace radray::render
