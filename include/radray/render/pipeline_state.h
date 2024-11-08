#pragma once

#include <radray/render/common.h>

namespace radray::render {

class PipelineState : public RenderBase {
public:
    ~PipelineState() noexcept override = default;
};

class GraphicsPipelineState : public PipelineState {
public:
    ~GraphicsPipelineState() noexcept override = default;
};

}  // namespace radray::render
