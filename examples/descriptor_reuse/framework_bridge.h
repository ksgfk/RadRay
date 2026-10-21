#pragma once

#include "descriptor_probe.h"
#include "workload.h"
#include <radray/runtime/gpu_system.h>

namespace radray::example {
// The sole framework adapter. Workload inputs, shaders, readback and probe stay fixed across the refactor.
class DescriptorFrameworkBridge {
public:
    static std::string_view ImplementationName() noexcept;
    DescriptorFrameworkBridge(render::Device* native, render::Device* probeDevice, DescriptorProbe& probe, const DescriptorReuseOptions& options);
    ~DescriptorFrameworkBridge();
    bool Initialize(render::TextureView* image);
    bool Prepare(AppFrameContext& frame, uint32_t viewCount, uint32_t sequence);
    void Record(render::GraphicsCommandEncoder* encoder, uint32_t view, uint32_t pass);
    void Unregister();
    const string& Error() const noexcept;

private:
    struct State;
    unique_ptr<State> _state;
};
}  // namespace radray::example
