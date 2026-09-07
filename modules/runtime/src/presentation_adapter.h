#pragma once

#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/render_output.h>

namespace radray {
class WindowManager;
struct PresentationFrame {
    vector<RenderSurfaceFrame> Surfaces;
    vector<RenderOutputInfo> Outputs;
    struct Entry {
        uint32_t Surface;
        AppFrameTarget Target;
    };
    vector<Entry> Presentations;
};

class PresentationAdapter {
public:
    explicit PresentationAdapter(WindowManager& windows) noexcept : _windows(windows) {}
    vector<RenderOutputInfo> GetOutputInfos(const RenderOutputRegistry& outputs) const;
    PresentationFrame Acquire(AppFrameContext& ctx, RenderOutputRegistry& outputs, std::span<const RenderOutputInfo> preparedOutputs,
                              std::span<const RenderOutputId> requestedOutputs) const;
    void Commit(const PresentationFrame& frame) const;
private:
    WindowManager& _windows;
};
}  // namespace radray
