#include <radray/runtime/render_framework/render_graph_runtime.h>
#include <radray/logger.h>

namespace radray {
RenderGraphRuntime::RenderGraphRuntime(render::Device& device, render::RenderPassRegistry& registry, uint32_t flights) {
    for (uint32_t flight = 0; flight < flights; ++flight) _pools.push_back(make_unique<RenderResourcePool>(device, registry));
}
RenderResourcePool& RenderGraphRuntime::BeginFlight(uint32_t flight, uint64_t serial) {
    RADRAY_ASSERT(flight < _pools.size());
    _pools[flight]->BeginFlight(serial);
    return *_pools[flight];
}
const RenderResourcePoolStats& RenderGraphRuntime::GetPoolStats(uint32_t flight) const { return _pools[flight]->GetStats(); }
void RenderGraphRuntime::Clear() {
    for (auto& pool : _pools) pool->Clear();
}
}  // namespace radray
