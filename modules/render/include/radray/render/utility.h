#pragma once

#include <radray/vertex_data.h>
#include <radray/render/common.h>

namespace radray::render {

struct SemanticMapping {
    std::string_view Semantic;
    uint32_t SemanticIndex;
    uint32_t Location;
    VertexFormat Format;
};

IndexFormat MapIndexType(VertexIndexType type) noexcept;
std::optional<vector<VertexElement>> MapVertexElements(std::span<VertexLayout> layouts, std::span<SemanticMapping> semantics) noexcept;

}  // namespace radray::render
