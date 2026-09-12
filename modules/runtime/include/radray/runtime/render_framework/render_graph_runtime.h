#pragma once
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/render_framework/render_resource_pool.h>

namespace radray {

class RenderGraph;
class FrameGraphTemplateCache;
class RenderPipelineContext;

/// Bounded CPU execution-plan variants shared by serialized graph instances across flights.
/// Eviction releases only the cache's reference; in-flight users keep their immutable plan alive.
class RenderGraphPlanCache {
public:
    explicit RenderGraphPlanCache(size_t capacity = 4);
    ~RenderGraphPlanCache() noexcept;
    RenderGraphPlanCache(const RenderGraphPlanCache&) = delete;
    RenderGraphPlanCache& operator=(const RenderGraphPlanCache&) = delete;
    size_t Size() const noexcept;
    void Clear() noexcept;

private:
    friend class RenderGraph;
    friend class RenderGraphFrameResources;
    shared_ptr<FrameGraphTemplateCache> _compositionCache;
    struct Impl;
    unique_ptr<Impl> _impl;
};

/// Per-flight storage that outlives a graph and keeps its descriptors and uploaded constants alive
/// until the flight is safe to reuse. Compilation scratch is CPU-only and may be reused by the
/// next serialized compile; compiled results and submissions do not borrow that scratch.
class RenderGraphFrameResources {
public:
    RenderGraphFrameResources(render::Device& device, render::RenderPassRegistry& registry);
    RenderGraphFrameResources(render::Device& device, render::RenderPassRegistry& registry, shared_ptr<RenderGraphPlanCache> plans);
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
    friend class RenderPipelineContext;
    shared_ptr<FrameGraphTemplateCache>& DefaultCompositionCacheStorage() noexcept;
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
    shared_ptr<RenderGraphPlanCache> _plans;
    vector<unique_ptr<RenderGraphFrameResources>> _flights;
};
}  // namespace radray
