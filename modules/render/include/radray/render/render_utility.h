#pragma once

#include <span>
#include <string_view>

#include <radray/vertex_data.h>
#include <radray/structured_buffer.h>

#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>

namespace radray::render {

struct SemanticMapping {
    std::string_view Semantic{};
    uint32_t SemanticIndex{0};
    uint32_t Location{0};
    VertexFormat Format{VertexFormat::UNKNOWN};
};
std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept;

std::optional<StructuredBufferStorage> CreateCBufferStorage(const MergedHlslShaderDesc& desc) noexcept;

std::optional<StructuredBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept;

}  // namespace radray::render
