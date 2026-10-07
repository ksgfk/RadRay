#pragma once

#include <chrono>
#include <cstdint>

namespace radray {

struct AppRenderContext {
    uint32_t FlightIndex{0};
    std::chrono::duration<float> DeltaTime{};
    std::chrono::duration<float> LastFrameLatency{};
    bool IsInModalLoop{false};
};

}  // namespace radray
