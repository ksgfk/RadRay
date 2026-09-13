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

/// Instance array high-water capacities, including idle reusable rows. NestedBytes counts vector
/// allocations owned by resource/pass/execution/slot/native-access rows, excluding compiled plans.
struct RenderGraphPayloadStats {
    size_t Entries{0};
    uint64_t Creations{0}, Reuses{0};
};

struct RenderGraphInstanceStorageStats {
    size_t Instances{0}, ActiveInstances{0};
    size_t ResourceCapacity{0}, PassCapacity{0}, ViewCapacity{0}, WorkCapacity{0}, TemplateCapacity{0}, ExecutionCapacity{0};
    size_t NestedBytes{0};
    friend bool operator==(const RenderGraphInstanceStorageStats&, const RenderGraphInstanceStorageStats&) = default;
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
    RenderGraphInstanceStorageStats GetInstanceStorageStats() const noexcept;
    RenderGraphPayloadStats GetPayloadStats() const noexcept;
    /// Writable-flight storage. The caller resets every frame field before binding the payload.
    /// ResetForReuse(), when available and noexcept, also runs at the next safe flight boundary.
    /// Retained older versions are never mutated; every acquisition is an exclusive writable version.
    template <class T, class... Args>
    shared_ptr<T> AcquireFramePayload(const void* owner, uint64_t key, Args&&... args) {
        auto create = [&]() -> shared_ptr<void> { return make_shared<T>(std::forward<Args>(args)...); };
        return std::static_pointer_cast<T>(AcquireFramePayloadStorage(owner, key, &PayloadType<T>, +[](void* factory) -> shared_ptr<void> { return (*static_cast<decltype(create)*>(factory))(); }, &create, +[](void* value) noexcept {
                if constexpr (requires(T& object) { { object.ResetForReuse() } noexcept; }) static_cast<T*>(value)->ResetForReuse(); }));
    }
    void Clear();

private:
    friend class RenderGraph;
    friend class RenderPipelineContext;
    shared_ptr<FrameGraphTemplateCache>& DefaultCompositionCacheStorage() noexcept;
    template <class T>
    static inline constexpr byte PayloadType{};
    shared_ptr<void> AcquireFramePayloadStorage(const void* owner, uint64_t key, const void* type,
                                                shared_ptr<void> (*create)(void*), void* factory, void (*reset)(void*) noexcept);
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
