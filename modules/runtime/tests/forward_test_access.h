#pragma once

#include "forward_pipeline/forward_frame.h"
#include <radray/runtime/forward_pipeline/forward_pipeline.h>

namespace radray::forward_detail {

struct ForwardPipelineTestAccess {
    static const ForwardFrameInput& Input(const ForwardPipeline& pipeline, uint32_t flightIndex) {
        return pipeline.GetFrameInput(flightIndex);
    }
};

}  // namespace radray::forward_detail
