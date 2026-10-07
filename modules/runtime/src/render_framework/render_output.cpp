#include <radray/runtime/render_framework/render_output.h>

#include <algorithm>
#include <radray/logger.h>

namespace radray {
std::optional<RenderOutputInfo> ValidateRenderOutput(const RenderOutput& output, render::Device& device) {
    if ((output.Colors.empty() && !output.Depth) || output.Colors.size() > device.GetCapabilities().Limits.MaxColorAttachments) return std::nullopt;
    RenderOutputInfo result{};
    vector<render::TextureViewDescriptor> views;
    const auto validate = [&](const RenderOutputAttachment& attachment, bool depth) {
        if (!attachment.View || !attachment.View->IsValid()) return false;
        const auto view = attachment.View->GetDesc();
        if (!view.Target || !view.Target->IsValid()) return false;
        for (const auto& previous : views)
            if (previous.Target == view.Target && previous.Range.BaseMipLevel == view.Range.BaseMipLevel && previous.Range.BaseArrayLayer == view.Range.BaseArrayLayer) return false;
        views.push_back(view);
        const auto texture = view.Target->GetDesc();
        if (view.Range.BaseMipLevel >= texture.MipLevels || view.Range.MipLevelCount != 1 || view.Range.ArrayLayerCount != 1 ||
            view.Range.BaseArrayLayer >= texture.DepthOrArraySize || attachment.Exit == render::TextureState::Undefined ||
            attachment.Enter == render::TextureState::UNKNOWN || attachment.Exit == render::TextureState::UNKNOWN) return false;
        if (depth ? view.Usage != render::TextureViewUsage::DepthWrite : view.Usage != render::TextureViewUsage::RenderTarget) return false;
        const uint32_t width = std::max(1u, texture.Width >> view.Range.BaseMipLevel);
        const uint32_t height = std::max(1u, texture.Height >> view.Range.BaseMipLevel);
        if (result.Width && (result.Width != width || result.Height != height || result.Samples != texture.SampleCount)) return false;
        result.Width = width;
        result.Height = height;
        result.Samples = texture.SampleCount;
        if (depth)
            result.Depth = view.Format;
        else
            result.Colors.push_back(view.Format);
        return true;
    };
    for (const auto& color : output.Colors)
        if (!validate(color, false)) return std::nullopt;
    if (output.Depth && !validate(*output.Depth, true)) return std::nullopt;
    return result;
}
RenderOutputState InitialRenderOutputState(const RenderOutput& output) {
    RenderOutputState state;
    for (const auto& color : output.Colors) state.Colors.push_back(color.Enter);
    if (output.Depth) state.Depth = output.Depth->Enter;
    return state;
}
void TransitionRenderOutput(render::CommandBuffer* command, const RenderOutput& output, RenderOutputState& state, bool exit) {
    if (state.Colors.size() != output.Colors.size() || state.Depth.has_value() != output.Depth.has_value()) RADRAY_ABORT("output state does not match attachments");
    vector<render::ResourceBarrierDescriptor> barriers;
    const auto transition = [&](const RenderOutputAttachment& attachment, render::TextureStates& current, render::TextureStates next) {
        if (current == next) return;
        const auto view = attachment.View->GetDesc();
        barriers.emplace_back(render::BarrierTextureDescriptor{view.Target, current, next, view.Range});
        current = next;
    };
    for (size_t i = 0; i < output.Colors.size(); ++i)
        transition(output.Colors[i], state.Colors[i], exit ? output.Colors[i].Exit : render::TextureStates{render::TextureState::RenderTarget});
    if (output.Depth) transition(*output.Depth, *state.Depth, exit ? output.Depth->Exit : render::TextureStates{render::TextureState::DepthWrite});
    if (!barriers.empty()) command->ResourceBarrier(barriers);
}
bool IsRenderOutputFinished(const RenderOutput& output, const RenderOutputState& state) noexcept {
    if (state.Colors.size() != output.Colors.size() || state.Depth.has_value() != output.Depth.has_value()) return false;
    for (size_t i = 0; i < output.Colors.size(); ++i)
        if (output.Colors[i].Exit != state.Colors[i]) return false;
    return !output.Depth || output.Depth->Exit == *state.Depth;
}
}  // namespace radray
