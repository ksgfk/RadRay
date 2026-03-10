#pragma once

#include <radray/nullable.h>

namespace radray::runtime {

class RenderGraph;
struct RenderFrameContext;

struct ForwardPipelineCreateDesc {
    RenderGraphExecuteFn UploadPassExecute{};
    RenderGraphExecuteFn MainScenePassExecute{};
};

class IRenderPipeline {
public:
    virtual ~IRenderPipeline() noexcept = default;

    virtual bool Build(RenderGraph& graph, const RenderFrameContext& frame, Nullable<string*> reason = nullptr) = 0;
};

unique_ptr<IRenderPipeline> CreateForwardPipeline(const ForwardPipelineCreateDesc& desc);

}  // namespace radray::runtime
