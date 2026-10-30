#pragma once

#include <radray/types.h>

namespace radray {

struct MeshSection {
    uint32_t MaterialSlot{0};
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
};

}  // namespace radray
