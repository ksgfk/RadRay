#pragma once
#include "forward_pipeline/forward_frame.h"
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
namespace radray::forward_detail {
struct ForwardPipelineTestAccess {
    static const RenderSceneSnapshot& Input(const ForwardPipeline& pipeline, uint32_t flightIndex) { return pipeline.GetSceneSnapshot(flightIndex); }
    static std::span<const ForwardViewDrawWork> Views(const ForwardPipeline& pipeline, uint32_t flightIndex, uint32_t familyIndex) { return pipeline.GetViewWork(flightIndex, familyIndex); }
};
}  // namespace radray::forward_detail
