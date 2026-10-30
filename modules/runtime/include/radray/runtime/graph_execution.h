#pragma once

#include <radray/runtime/graph.h>

namespace radray {

struct ResourceAllocation {
    unique_ptr<render::Buffer> buffer;
    unique_ptr<render::Texture> texture;
    ResourceState state;
    shared_ptr<render::Fence> completion;
    uint64_t value{};
};

struct ExtractedResource {
    ResourceHandle handle;
    shared_ptr<ResourceAllocation> allocation;
    shared_ptr<void> importedOwner;
    Nullable<render::Buffer*> buffer{};
    Nullable<render::Texture*> texture{};
    ResourceState state;
    shared_ptr<render::Fence> completion;
    uint64_t value{};
};

class ResourceStore {
public:
    explicit ResourceStore(render::Device& device);
    Nullable<shared_ptr<ResourceAllocation>> Acquire(const GraphResource&);
    size_t PooledCount() const { return _pool.size(); }

private:
    render::Device& _device;
    vector<shared_ptr<ResourceAllocation>> _pool;
};

class GraphExecution;

class PassContext {
public:
    Nullable<render::Buffer*> Buffer(BufferHandle) const;
    Nullable<render::Texture*> Texture(TextureHandle) const;
    Nullable<render::GraphicsCommandEncoder*> Raster() const;
    Nullable<render::ComputeCommandEncoder*> Compute() const;
    // Copy callbacks receive the command buffer; pass boundaries and barriers remain executor-owned.
    Nullable<render::CommandBuffer*> Copy() const;
    render::Device& Device() const;
    Nullable<render::TextureView*> CreateView(TextureHandle, render::TextureViewUsage);
    void KeepAlive(shared_ptr<void>);

private:
    friend class GraphExecution;
    PassContext(GraphExecution&, const GraphPass&, render::CommandBuffer&, Nullable<render::GraphicsCommandEncoder*>, Nullable<render::ComputeCommandEncoder*>);
    bool Check(ResourceHandle, ResourceKind) const;
    GraphExecution& _execution;
    const GraphPass& _pass;
    render::CommandBuffer& _command;
    Nullable<render::GraphicsCommandEncoder*> _raster;
    Nullable<render::ComputeCommandEncoder*> _compute;
    mutable bool _failed{};
};

class GraphExecution {
public:
    static Nullable<unique_ptr<GraphExecution>> Create(render::Device&, ResourceStore&, CompiledGraph);
    bool Record(render::CommandBuffer&, Nullable<render::QueryPool*> timestamps = nullptr);
    void Submitted(shared_ptr<render::Fence>, uint64_t);
    const CompiledGraph& Graph() const { return _graph; }
    const vector<ExtractedResource>& Extractions() const { return _extractions; }

private:
    friend class PassContext;
    GraphExecution(render::Device& d, CompiledGraph g);
    Nullable<render::TextureView*> View(uint32_t, render::TextureViewUsage);
    void Barriers(render::CommandBuffer&, std::span<const PlannedBarrier>);
    render::Device& _device;
    CompiledGraph _graph;
    vector<shared_ptr<ResourceAllocation>> _allocations;
    vector<Nullable<render::Buffer*>> _buffers;
    vector<Nullable<render::Texture*>> _textures;
    vector<unique_ptr<render::TextureView>> _views;
    vector<shared_ptr<void>> _owners;
    vector<ExtractedResource> _extractions;
};

}  // namespace radray
