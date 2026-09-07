#include "presentation_adapter.h"
#include <radray/runtime/window_manager.h>

namespace radray {
vector<RenderOutputInfo> PresentationAdapter::GetOutputInfos(const RenderOutputRegistry& outputs) const {
    auto infos = outputs.GetGameThreadInfos();
    auto* windows = &_windows;
    for (auto& output : infos)
        if (output.Kind == RenderOutputKind::Presentation) {
            for (size_t i = 0; i < windows->GetWindowCount(); ++i) {
                auto* window = windows->GetWindow(i);
                if (window->GetRenderOutputId() == output.Id) output.Active = output.Active && !window->IsMinimized();
            }
        }
    return infos;
}
PresentationFrame PresentationAdapter::Acquire(AppFrameContext& ctx, RenderOutputRegistry& outputs, std::span<const RenderOutputInfo> preparedOutputs,
                                                std::span<const RenderOutputId> requestedOutputs) const {
    PresentationFrame frame;
    auto* windows = &_windows;
    for (const auto output : requestedOutputs) {
        Nullable<const RenderOutputInfo*> known{nullptr};
        for (const auto& prepared : preparedOutputs)
            if (prepared.Id == output) known = &prepared;
        RenderOutputInfo info = known ? *known : RenderOutputInfo{.Id = output};
        info.Active = false;
        if (known && known->Active) {
            if (known->Kind == RenderOutputKind::ExternalColorTexture) {
                auto surface = outputs.ResolveExternal(output);
                if (surface) {
                    frame.Surfaces.push_back(*surface);
                    info.Active = true;
                }
            } else {
                for (size_t w = 0; w < windows->GetWindowCount(); ++w) {
                    auto* window = windows->GetWindow(w);
                    if (window->GetRenderOutputId() != output || !window->GetSwapChain()) continue;
                    auto target = ctx.AcquireWindow(window);
                    if (!target) break;
                    const auto desc = target->BackBuffer->GetDesc();
                    frame.Presentations.push_back({static_cast<uint32_t>(frame.Surfaces.size()), *target});
                    frame.Surfaces.push_back({output, target->BackBuffer, target->BackBufferView, desc,
                                        window->GetBackBufferState(target->BackBufferIndex), render::TextureState::Present, false, false});
                    info.Width = desc.Width;
                    info.Height = desc.Height;
                    info.Format = desc.Format;
                    info.SampleCount = desc.SampleCount;
                    info.Active = true;
                    break;
                }
            }
        }
        frame.Outputs.push_back(info);
    }
    return frame;
}
void PresentationAdapter::Commit(const PresentationFrame& frame) const {
    for (const auto& presentation : frame.Presentations)
        presentation.Target.Window->SetBackBufferState(presentation.Target.BackBufferIndex, frame.Surfaces[presentation.Surface].CurrentState);
}
}  // namespace radray
