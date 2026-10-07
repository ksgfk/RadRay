#pragma once

#include <radray/render/rhi.h>

namespace radray {
struct RenderOutputAttachment {
    render::TextureView* View;
    render::TextureStates Enter;
    render::TextureStates Exit;
};
/// Actual views define format, samples, and mip extent. Owned by the caller through GPU completion.
struct RenderOutput {
    vector<RenderOutputAttachment> Colors;
    std::optional<RenderOutputAttachment> Depth;
};
struct RenderOutputInfo {
    uint32_t Width, Height, Samples;
    vector<render::TextureFormat> Colors;
    std::optional<render::TextureFormat> Depth;
};
/// Per invocation planned states; never imply successful GPU execution.
struct RenderOutputState {
    vector<render::TextureStates> Colors;
    std::optional<render::TextureStates> Depth;
};
std::optional<RenderOutputInfo> ValidateRenderOutput(const RenderOutput& output, render::Device& device);
RenderOutputState InitialRenderOutputState(const RenderOutput& output);
void TransitionRenderOutput(render::CommandBuffer* command, const RenderOutput& output, RenderOutputState& state, bool exit);
bool IsRenderOutputFinished(const RenderOutput& output, const RenderOutputState& state) noexcept;
}  // namespace radray
