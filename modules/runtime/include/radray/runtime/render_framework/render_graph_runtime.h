#pragma once
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/render_framework/render_resource_pool.h>

namespace radray {

class RenderGraph;

/// Per-flight storage that outlives a graph and keeps its descriptors and uploaded constants alive
/// until the flight is safe to reuse.
class RenderGraphFrameResources {
public:
    RenderGraphFrameResources(render::Device& device, render::RenderPassRegistry& registry);
    ~RenderGraphFrameResources() noexcept;
    RenderGraphFrameResources(const RenderGraphFrameResources&) = delete;
    RenderGraphFrameResources& operator=(const RenderGraphFrameResources&) = delete;

    void BeginFlight(uint64_t serial, HostWriteBatch& hostWrites);
    RenderResourcePool& GetPool() noexcept;
    const RenderResourcePoolStats& GetPoolStats() const noexcept;
    size_t GetParameterSetCount() const noexcept;
    void Clear();

private:
    friend class RenderGraph;
    struct Impl;
    unique_ptr<Impl> _impl;
};

class RenderGraphRuntime {
public:
    RenderGraphRuntime(render::Device& device, render::RenderPassRegistry& registry, uint32_t flights);
    RenderGraphFrameResources& BeginFlight(uint32_t flight, uint64_t serial, HostWriteBatch& hostWrites);
    const RenderResourcePoolStats& GetPoolStats(uint32_t flight) const;
    void Clear();

private:
    vector<unique_ptr<RenderGraphFrameResources>> _flights;
};
}  // namespace radray
