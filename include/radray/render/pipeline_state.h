#pragma once

#include <radray/render/common.h>

namespace radray::render {

class VertexElement {
public:
    uint64_t Offset;
    radray::string Semantic;
    uint32_t SemanticIndex;
    VertexFormat Format;
    uint32_t Location;
};

class VertexBufferLayout {
public:
    uint64_t ArrayStride;
    VertexStepMode StepMode;
    radray::vector<VertexElement> Elements;
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

inline PrimitiveState DefaultPrimitiveState() noexcept {
    return {
        .Topology = PrimitiveTopology::TriangleList,
        .StripIndexFormat = IndexFormat::UINT32,
        .FaceClockwise = FrontFace::CW,
        .Cull = CullMode::Back,
        .Poly = PolygonMode::Fill,
        .UnclippedDepth = true,
        .Conservative = false,
    };
}

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

inline DepthStencilState DefaultDepthStencilState() noexcept {
    return {
        .Format = TextureFormat::D24_UNORM_S8_UINT,
        .DepthCompare = CompareFunction::Less,
        .Stencil = {
            .Front = {
                .Compare = CompareFunction::Always,
                .FailOp = StencilOperation::Keep,
                .DepthFailOp = StencilOperation::Keep,
                .PassOp = StencilOperation::Keep,
            },
            .Back = {
                .Compare = CompareFunction::Always,
                .FailOp = StencilOperation::Keep,
                .DepthFailOp = StencilOperation::Keep,
                .PassOp = StencilOperation::Keep,
            },
            .ReadMask = 0xFF,
            .WriteMask = 0xFF,
        },
        .DepthBias = {
            .Constant = 0,
            .SlopScale = 0.0f,
            .Clamp = 0.0f,
        },
        .DepthWriteEnable = true,
        .StencilEnable = false,
    };
}

struct MultiSampleState {
    uint32_t Count;
    uint64_t Mask;
    bool AlphaToCoverageEnable;
};

inline MultiSampleState DefaultMultiSampleState() noexcept {
    return {
        .Count = 1,
        .Mask = 0xFFFFFFFF,
        .AlphaToCoverageEnable = false,
    };
}

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

inline ColorTargetState DefaultColorTargetState(TextureFormat format) noexcept {
    return {
        .Format = format,
        .Blend = {
            .Color = {
                .Src = BlendFactor::One,
                .Dst = BlendFactor::Zero,
                .Op = BlendOperation::Add,
            },
            .Alpha = {
                .Src = BlendFactor::One,
                .Dst = BlendFactor::Zero,
                .Op = BlendOperation::Add,
            },
        },
        .WriteMask = ColorWrite::All,
        .BlendEnable = false,
    };
}

class GraphicsPipelineStateDescriptor {
public:
    radray::string Name;
    RootSignature* RootSig;
    Shader* VS;
    Shader* PS;
    radray::vector<VertexBufferLayout> VertexBuffers;
    PrimitiveState Primitive;
    DepthStencilState DepthStencil;
    MultiSampleState MultiSample;
    radray::vector<ColorTargetState> ColorTargets;
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
