#pragma once

#include <algorithm>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <radray/vertex_data.h>

#include <radray/render/common.h>
#include <radray/render/dxc.h>

namespace radray::render {

bool IsDepthStencilFormat(TextureFormat format) noexcept;
uint32_t GetVertexFormatSize(VertexFormat format) noexcept;
uint32_t GetIndexFormatSize(IndexFormat format) noexcept;
PrimitiveState DefaultPrimitiveState() noexcept;
DepthStencilState DefaultDepthStencilState() noexcept;
StencilState DefaultStencilState() noexcept;
MultiSampleState DefaultMultiSampleState() noexcept;
ColorTargetState DefaultColorTargetState(TextureFormat format) noexcept;
BlendState DefaultBlendState() noexcept;
IndexFormat MapIndexType(uint32_t size) noexcept;
struct SemanticMapping {
    std::string_view Semantic{};
    uint32_t SemanticIndex{0};
    uint32_t Location{0};
    VertexFormat Format{VertexFormat::UNKNOWN};
};
std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept;

}  // namespace radray::render
