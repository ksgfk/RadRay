#pragma once
#include <radray/runtime/render_framework/render_resource_pool.h>

namespace radray {
class RenderGraphRuntime {
public:
    RenderGraphRuntime(render::Device& device, render::RenderPassRegistry& registry, uint32_t flights);
    RenderResourcePool& BeginFlight(uint32_t flight, uint64_t serial);
    const RenderResourcePoolStats& GetPoolStats(uint32_t flight) const;
    void Clear();

private:
    vector<unique_ptr<RenderResourcePool>> _pools;
};
}  // namespace radray
