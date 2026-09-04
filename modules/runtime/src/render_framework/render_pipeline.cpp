#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray {

RenderPipeline::~RenderPipeline() noexcept = default;

void RenderPipeline::PrepareFrame(const AppUpdateContext&, vector<StreamingAssetRefAny>&) {}

}  // namespace radray
