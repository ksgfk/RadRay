#pragma once

#include <radray/nullable.h>
#include <radray/types.h>

namespace radray::render {
class Buffer;
}

namespace radray {

// Direct indexed drawing with one instance, starting at instance zero.
struct MeshBatchElement {
    // Borrowed; required when IndexCount is nonzero. Offset is in bytes; stride is 2 or 4.
    Nullable<render::Buffer*> IndexBuffer{nullptr};
    uint32_t IndexBufferOffset{0};
    uint32_t IndexStride{4};
    // FirstIndex is relative to IndexBufferOffset and measured in indices.
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
};

}  // namespace radray
