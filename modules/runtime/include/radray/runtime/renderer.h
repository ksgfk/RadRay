#pragma once

#include <radray/runtime/resources.h>
#include <radray/runtime/graph_execution.h>

namespace radray {

struct BindingResource {
    Nullable<render::Buffer*> buffer{};
    Nullable<render::Texture*> texture{};
    Access access{Access::Sample};
    render::ShaderStages stages{};
    ImportOptions import;
};

struct MeshBinding {
    Nullable<render::GraphicsPipelineState*> pipeline{};
    Nullable<render::ShaderParameterSet*> parameters{};
    std::optional<uint32_t> vertexSlot, instanceSlot;
    vector<BindingResource> resources;
};

class PreparedMeshPass {
public:
    vector<VisibleMeshDraw> visible;
    DrawList draws;
    map<uint64_t, BindingResource> pages, indices;
    map<uint64_t, MeshBinding> bindings;
    BindingResource instances, indirect;
    bool direct{};
    vector<ResourceAccess> Declare(GraphBuilder&) const;
    bool Draw(PassContext&) const;
};

class FrameContext {
public:
    uint32_t Slot() const { return _slot; }
    Nullable<render::Buffer*> Upload(string name, std::span<const byte>, render::BufferUses);
    void KeepAlive(shared_ptr<void> owner);

private:
    friend class Renderer;

    struct UploadBuffer {
        RenderBuffer buffer;
        uint64_t size{};
        render::BufferUses usage;
    };

    FrameContext(
        render::Device& device,
        uint32_t slot,
        unique_ptr<render::CommandAllocator> allocator,
        unique_ptr<render::CommandBuffer> command,
        unique_ptr<render::QueryPool> timestamps,
        RenderBuffer timing);

    render::Device& _device;
    uint32_t _slot;
    unique_ptr<render::CommandAllocator> _allocator;
    unique_ptr<render::CommandBuffer> _command;
    uint64_t _completion{};
    map<string, UploadBuffer> _uploads;
    vector<shared_ptr<void>> _keepAlive;
    unique_ptr<GraphExecution> _execution;
    RenderBuffer _staging;
    uint64_t _stagingSize{};
    unique_ptr<render::QueryPool> _timestamps;
    RenderBuffer _timing;
    bool _measured{}, _timingInitialized{};
};

class IRenderPipeline {
public:
    virtual ~IRenderPipeline() = default;
    virtual const MeshPassProcessor& Processor() const = 0;
    virtual bool BuildGraph(GraphBuilder&, FrameContext&, PreparedMeshPass&, Nullable<const void*> frameData) = 0;
};

struct TimelinePoint {
    Nullable<render::Fence*> fence{};
    uint64_t value{};
};

struct Submission {
    vector<TimelinePoint> waits, signals;
    bool forceDirect{};
    bool profile{};
    uint32_t timeoutMs{5000};
};

class Renderer {
public:
    ~Renderer();
    static Nullable<unique_ptr<Renderer>> Create(DeviceContext&, unique_ptr<IRenderPipeline>);
    bool Render(Scene&, const SceneUpdate&, const ViewSubmission&, uint64_t bindingVersion, Nullable<const void*> frameData, const Submission&);
    bool Finish(uint32_t timeoutMs = 5000);
    const string& LastGraph() const { return _lastGraph; }
    uint64_t Submitted() const { return _submitted; }
    render::Fence& Completion() const { return *_completion; }
    std::span<const ExtractedResource> Extractions() const;
    ResourceStore& Resources() { return _pool; }

private:
    Renderer(DeviceContext& d, unique_ptr<IRenderPipeline> p, shared_ptr<render::Fence> fence, array<unique_ptr<FrameContext>, 2> frames);
    DeviceContext& _device;
    unique_ptr<IRenderPipeline> _pipeline;
    shared_ptr<render::Fence> _completion;
    array<unique_ptr<FrameContext>, 2> _frames;
    ResourceStore _pool;
    GeometryStore _geometry;
    uint64_t _submitted{};
    string _lastGraph;
    vector<double> _gpuSamples;
};
}  // namespace radray
