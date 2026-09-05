#include <radray/runtime/render_framework/render_graph_runtime.h>
#include <radray/logger.h>

namespace radray {
RenderGraphRuntime::RenderGraphRuntime(render::Device& device, render::RenderPassRegistry& registry, uint32_t flights) {
    for (uint32_t flight = 0; flight < flights; ++flight) {
        _flights.push_back(make_unique<RenderGraphFrameResources>(device, registry));
    }
}
RenderGraphFrameResources& RenderGraphRuntime::BeginFlight(
    uint32_t flight, uint64_t serial, HostWriteBatch& hostWrites) {
    RADRAY_ASSERT(flight < _flights.size());
    _flights[flight]->BeginFlight(serial, hostWrites);
    return *_flights[flight];
}
const RenderResourcePoolStats& RenderGraphRuntime::GetPoolStats(uint32_t flight) const { return _flights[flight]->GetPoolStats(); }
void RenderGraphRuntime::Clear() {
    for (auto& flight : _flights) flight->Clear();
}
}  // namespace radray
