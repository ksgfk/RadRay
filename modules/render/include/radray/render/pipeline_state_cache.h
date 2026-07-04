#pragma once

#include <optional>

#include <radray/guid.h>
#include <radray/render/common.h>

namespace radray::render {

// PSO 缓存的 POD key. 所有字段为标量 (无指针/optional/span),
// 构造时以 `Key{}` 清零, 再逐字段赋值, 保证 padding 恒为 0,
// 从而可安全用于 PodHasher (byte-wise xxHash) 与 PodEqual (memcmp).
struct GraphicsPsoKey {
    Guid LayoutId;
    Guid VSId;
    Guid PSId;

    // PrimitiveState 展平
    int32_t Topology;
    int32_t FaceClockwise;
    int32_t Cull;
    int32_t Poly;
    uint32_t HasStripIndexFormat;
    int32_t StripIndexFormat;
    uint32_t UnclippedDepth;
    uint32_t Conservative;

    // DepthStencil 展平
    uint32_t HasDepthStencil;
    int32_t DSFormat;
    int32_t DepthCompare;
    uint32_t DepthWriteEnable;
    int32_t DepthBiasConstant;
    float DepthBiasSlopScale;
    float DepthBiasClamp;
    uint32_t HasStencil;
    int32_t StencilFrontCompare;
    int32_t StencilFrontFailOp;
    int32_t StencilFrontDepthFailOp;
    int32_t StencilFrontPassOp;
    int32_t StencilBackCompare;
    int32_t StencilBackFailOp;
    int32_t StencilBackDepthFailOp;
    int32_t StencilBackPassOp;
    uint32_t StencilReadMask;
    uint32_t StencilWriteMask;

    // MultiSample 展平
    uint32_t MsCount;
    uint64_t MsMask;
    uint32_t MsAlphaToCoverage;

    // ColorTargets 展平
    uint32_t ColorTargetCount;
    struct ColorTargetEntry {
        int32_t Format;
        uint32_t HasBlend;
        int32_t ColorSrc;
        int32_t ColorDst;
        int32_t ColorOp;
        int32_t AlphaSrc;
        int32_t AlphaDst;
        int32_t AlphaOp;
        uint32_t WriteMask;
    } ColorTargets[kMaxColorTargets];

    // VertexLayouts 展平
    uint32_t VertexLayoutCount;
    struct VertexLayoutEntry {
        uint64_t ArrayStride;
        int32_t StepMode;
        uint32_t ElemCount;
        struct ElemEntry {
            uint64_t Offset;
            char Semantic[kMaxSemanticLength];
            uint32_t SemanticIndex;
            int32_t Format;
            uint32_t Location;
        } Elems[kMaxVertexElementsPerLayout];
    } VertexLayouts[kMaxVertexBufferLayouts];
};

struct ComputePsoKey {
    Guid LayoutId;
    Guid CSId;
};

static_assert(std::is_trivially_copyable_v<GraphicsPsoKey>, "GraphicsPsoKey must be trivially copyable");
static_assert(std::is_trivially_copyable_v<ComputePsoKey>, "ComputePsoKey must be trivially copyable");

// 从 GraphicsPipelineStateDescriptor 构造 POD key.
// 失败情形 (返回 nullopt 并记录错误):
//   - BindingLayout / VS / PS 为空或 Guid 为 Empty (未经缓存分配身份)
//   - ColorTargets / VertexLayouts / 每层 Elements 数量超过上限
//   - Semantic 字符串长度 >= kMaxSemanticLength
std::optional<GraphicsPsoKey> BuildGraphicsPsoKey(const GraphicsPipelineStateDescriptor& desc) noexcept;

std::optional<ComputePsoKey> BuildComputePsoKey(const ComputePipelineStateDescriptor& desc) noexcept;

}  // namespace radray::render
