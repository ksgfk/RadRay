#pragma once
#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray::forward_detail {
struct ForwardCapture {
    unique_ptr<render::Buffer> Readback;
    RenderExternalBuffer Import{};
    RenderExtent Size;
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
    uint64_t Pitch{0};
    std::filesystem::path Directory;
    string Name, Report, Dot;
    bool Pending{false};
    bool Build(RenderGraph& graph, RenderPipelineContext& context, render::Device& device);
    bool Complete();
};
}  // namespace radray::forward_detail
