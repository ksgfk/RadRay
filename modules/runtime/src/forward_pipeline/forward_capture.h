#pragma once
#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray {
class RenderSystem;
}
namespace radray::forward_detail {
struct ForwardCapture {
    RgReadbackTicket Readback;
    RenderExtent Size;
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
    uint64_t Pitch{0};
    std::filesystem::path Directory;
    string Name, Report, Dot;
    bool Pending{false};
    void CaptureReport(const RenderGraphExecutionReport& report);
    bool Build(RenderGraph& graph, RenderPipelineContext& context, render::Device& device, RenderSystem& renderer, std::span<RenderGraphOutputBinding> outputs);
    bool Complete();
};
}  // namespace radray::forward_detail
