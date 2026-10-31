#pragma once

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/render/rhi.h>

namespace radray {

struct VertexInputAttribute {
    uint32_t BufferBinding{0};
    uint32_t Offset{0};
    string Semantic{};
    uint32_t SemanticIndex{0};
    render::VertexFormat Format{render::VertexFormat::UNKNOWN};
    uint32_t Location{0};
};

struct VertexInputStream {
    // Borrowed; required for every bound stream. Offset and Size are in bytes.
    Nullable<render::Buffer*> Buffer{nullptr};
    uint64_t Offset{0};
    uint64_t Size{0};
    uint32_t Binding{0};
};

struct VertexInput {
    vector<render::VertexBufferLayout> Layouts;
    vector<VertexInputAttribute> Attributes;
    vector<VertexInputStream> Streams;
};

}  // namespace radray
