#pragma once

#include <cstdint>

namespace radray {

struct Viewport {
    float X;
    float Y;
    float Width;
    float Height;
    float MinDepth;
    float MaxDepth;
};

struct Rect {
    int32_t X;
    int32_t Y;
    uint32_t Width;
    uint32_t Height;
};

}  // namespace radray
