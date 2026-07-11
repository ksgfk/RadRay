#pragma once

#include <optional>

#include <radray/guid.h>
#include <radray/hash.h>
#include <radray/render/common.h>
#include <radray/types.h>

namespace radray {

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
    } ColorTargets[render::kMaxColorTargets];

    // VertexLayouts 展平
    uint32_t VertexLayoutCount;
    struct VertexLayoutEntry {
        uint64_t ArrayStride;
        int32_t StepMode;
        uint32_t ElemCount;
        struct ElemEntry {
            uint64_t Offset;
            char Semantic[render::kMaxSemanticLength];
            uint32_t SemanticIndex;
            int32_t Format;
            uint32_t Location;
        } Elems[render::kMaxVertexElementsPerLayout];
    } VertexLayouts[render::kMaxVertexBufferLayouts];
};

struct ComputePsoKey {
    Guid LayoutId;
    Guid CSId;
};

static_assert(std::is_trivially_copyable_v<GraphicsPsoKey>, "GraphicsPsoKey must be trivially copyable");
static_assert(std::is_trivially_copyable_v<ComputePsoKey>, "ComputePsoKey must be trivially copyable");

// 从 GraphicsPipelineStateDescriptor 构造 POD key.
// 失败情形 (返回 nullopt 并记录错误):
//   - PipelineLayout / VS / PS 为空或 Guid 为 Empty (未经缓存分配身份)
//   - ColorTargets / VertexLayouts / 每层 Elements 数量超过上限
//   - Semantic 字符串长度 >= kMaxSemanticLength
std::optional<GraphicsPsoKey> BuildGraphicsPsoKey(const render::GraphicsPipelineStateDescriptor& desc) noexcept;

std::optional<ComputePsoKey> BuildComputePsoKey(const render::ComputePipelineStateDescriptor& desc) noexcept;

class GraphicsPipelineStateLibrary {
public:
    explicit GraphicsPipelineStateLibrary(render::Device* device) noexcept;
    ~GraphicsPipelineStateLibrary() noexcept;
    GraphicsPipelineStateLibrary(const GraphicsPipelineStateLibrary&) = delete;
    GraphicsPipelineStateLibrary& operator=(const GraphicsPipelineStateLibrary&) = delete;

    Nullable<render::GraphicsPipelineState*> GetOrCreate(
        const render::GraphicsPipelineStateDescriptor& desc) noexcept;
    bool Remove(render::GraphicsPipelineState* pso) noexcept;
    void Clear() noexcept;
    uint32_t Count() const noexcept { return static_cast<uint32_t>(_cache.size()); }
    uint64_t GetId(const render::GraphicsPipelineState* pso) const noexcept;
    uint64_t GetHitCount() const noexcept { return _hits; }
    uint64_t GetMissCount() const noexcept { return _misses; }

private:
    render::Device* _device;
    unordered_map<
        GraphicsPsoKey,
        unique_ptr<render::GraphicsPipelineState>,
        PodHasher<GraphicsPsoKey>,
        PodEqual<GraphicsPsoKey>>
        _cache;
    unordered_map<const render::GraphicsPipelineState*, uint64_t> _ids;
    uint64_t _hits{0};
    uint64_t _misses{0};
    uint64_t _nextId{1};
};

class ComputePipelineStateLibrary {
public:
    explicit ComputePipelineStateLibrary(render::Device* device) noexcept;
    ~ComputePipelineStateLibrary() noexcept;
    ComputePipelineStateLibrary(const ComputePipelineStateLibrary&) = delete;
    ComputePipelineStateLibrary& operator=(const ComputePipelineStateLibrary&) = delete;

    Nullable<render::ComputePipelineState*> GetOrCreate(
        const render::ComputePipelineStateDescriptor& desc) noexcept;
    bool Remove(render::ComputePipelineState* pso) noexcept;
    void Clear() noexcept;
    uint32_t Count() const noexcept { return static_cast<uint32_t>(_cache.size()); }

private:
    render::Device* _device;
    unordered_map<
        ComputePsoKey,
        unique_ptr<render::ComputePipelineState>,
        PodHasher<ComputePsoKey>,
        PodEqual<ComputePsoKey>>
        _cache;
};

}  // namespace radray
