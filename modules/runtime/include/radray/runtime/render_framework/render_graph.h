#pragma once

#include <source_location>
#include <span>
#include <string_view>
#include <variant>
#include <radray/inline_vector.h>
#include <radray/runtime/frame_submission.h>
#include <radray/runtime/render_framework/render_resource_pool.h>
#include <radray/runtime/render_framework/render_graph_compiler.h>
#include <radray/runtime/render_framework/render_graph_runtime_options.h>
#include <radray/runtime/shader_program.h>

namespace radray {

template <class Tag>
struct RgHandle {
    uint32_t Index{UINT32_MAX};
    uint64_t Generation{0};
    bool IsValid() const noexcept { return Index != UINT32_MAX && Generation != 0; }
    friend bool operator==(const RgHandle&, const RgHandle&) = default;
};
template <class Tag>
struct RgResourceValue {
    uint32_t Index{UINT32_MAX};
    uint64_t Generation{0};
    uint32_t Version{0};
    bool IsValid() const noexcept { return Index != UINT32_MAX && Generation != 0; }
    friend bool operator==(const RgResourceValue&, const RgResourceValue&) = default;
};
using RgTextureValue = RgResourceValue<struct RgTextureTag>;
using RgBufferValue = RgResourceValue<struct RgBufferTag>;
using RgTexturePort = RgHandle<struct RgTexturePortTag>;
using RgBufferPort = RgHandle<struct RgBufferPortTag>;
using RgTextureViewHandle = RgHandle<struct RgTextureViewTag>;
using RgPassHandle = RgHandle<struct RgPassTag>;
using RgIndirectArgumentsHandle = RgHandle<struct RgIndirectArgumentsTag>;
using RgWorkHandle = RgHandle<struct RgWorkTag>;

/// Filled by live work before upload preparation; the view and its bytes live through Execute.
struct RgUploadData {
    std::span<const byte> Bytes;
};

enum class RenderGraphExternalAccess : uint8_t { ReadOnly,
                                                 ReadWrite,
                                                 ObservableOutput };
enum class RgPassType : uint8_t { Raster,
                                  Compute,
                                  Copy,
                                  Resolve,
                                  Upload,
                                  Export };
enum class RgBufferAccess : uint8_t { Vertex,
                                      Index,
                                      Constant,
                                      ShaderRead,
                                      UnorderedAccess,
                                      Indirect,
                                      CopySource,
                                      CopyDestination,
                                      HostRead };
enum class RgIndirectCommand : uint8_t { Draw,
                                         DrawIndexed,
                                         Dispatch };

struct RenderExternalTexture {
    render::Texture* Texture;
    render::TextureDescriptor Desc;
    std::span<render::TextureStates> SubresourceStates;
    std::span<uint8_t> ContentValid;
    Nullable<render::TextureView*> ColorAttachmentView{nullptr};
    bool Written{false};
    Nullable<vector<PooledTextureView>*> PersistentViews{nullptr};
    shared_ptr<void> Owner{};
};
struct RenderExternalBuffer {
    render::Buffer* Buffer;
    render::BufferDescriptor Desc;
    render::BufferStates State;
    bool ContentValid{false};
    bool Written{false};
    shared_ptr<void> Owner{};
};
struct RgTextureViewDesc {
    render::TextureDimension Dimension{render::TextureDimension::UNKNOWN};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
    render::SubresourceRange Range{0, render::SubresourceRange::All, 0, render::SubresourceRange::All};
    render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
};
struct RgColorAttachmentDesc {
    RgTextureViewDesc View{};
    render::LoadAction Load{render::LoadAction::Clear};
    render::StoreAction Store{render::StoreAction::Store};
    render::ColorClearValue Clear{};
};
struct RgDepthAttachmentDesc {
    RgTextureViewDesc View{};
    render::LoadAction Load{render::LoadAction::Clear};
    render::StoreAction Store{render::StoreAction::Store};
    render::DepthStencilClearValue Clear{1.0f, 0};
    bool ReadOnly{false};
};
/// Textures bind through the view handle their declaration returned, so the descriptor and the
/// planned access can never disagree. Read/write usage comes from that declaration.
struct RgTextureParameterBinding {
    RgTextureViewHandle View{};
};
struct RgBufferParameterBinding {
    RgBufferValue Buffer{};
    render::BufferRange Range{render::BufferRange::AllRange()};
    uint32_t StructureByteStride{0};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
};
struct RgSamplerParameterBinding {
    render::SamplerDescriptor Sampler{};
};
struct RgCBufferParameterBinding {
    std::span<const byte> Bytes{};
};
using RgParameterBindingValue = std::variant<RgCBufferParameterBinding, RgTextureParameterBinding,
                                             RgBufferParameterBinding, RgSamplerParameterBinding>;
struct RgParameterBinding {
    std::string_view Declaration{};
    uint32_t ArrayElement{0};
    RgParameterBindingValue Value{};
};
/// A parameter group with its native set resolved: the only form the recording stage consumes.
/// Inline capacity covers one dynamic buffer per group; larger counts spill to the heap.
struct PreparedShaderGroup {
    uint32_t Group{0};
    Nullable<render::ShaderParameterSet*> Set{nullptr};
    InlineVector<render::ShaderParameterDynamicOffset, 2> DynamicOffsets;
    bool IsValid() const noexcept { return bool(Set); }
};
struct RenderGraphDiagnostic {
    string Code, Graph, Pass, Binding, Resource, Message, File;
    uint32_t Line{0};
};
struct RenderGraphAccessReport {
    uint32_t Resource, Version, State;
    render::SubresourceRange TextureRange;
    render::BufferRange BufferRange;
    render::ShaderStages Stages;
    bool Read, Write;
};
/// Calls forwarded by runtime to RHI, including fullscreen and indirect work. These do not count
/// backend-internal native calls or operations outside this graph. IndirectDrawArguments is the fixed
/// argument count declared by the graph, not the GPU-generated vertex/instance count.
struct RenderGraphCommandCalls {
    uint64_t Draw{0}, DrawIndexed{0}, DrawIndirect{0}, DrawIndexedIndirect{0};
    uint64_t Dispatch{0}, DispatchIndirect{0}, IndirectDrawArguments{0};
    uint64_t SetPipeline{0}, SetParameters{0}, VertexBuffer{0}, IndexBuffer{0};
    uint64_t PushConstants{0}, Viewport{0}, Scissor{0}, Copy{0}, Resolve{0};
    void Add(const RenderGraphCommandCalls& other) noexcept;
};
struct RenderGraphPassReport {
    string Name, File;
    uint32_t Line{0};
    RgPassType Type;
    bool Live{false}, Executed{false};
    vector<uint32_t> DataDependencies{}, HazardDependencies{};
    string LivenessReason{};
    vector<uint32_t> Reads{}, Writes{};
    uint32_t RasterGroup{RgInvalidIndex};
    vector<string> Decisions{};
    vector<RenderGraphAccessReport> Accesses{};
    RenderGraphCommandCalls CommandCalls{};
};
struct RenderGraphResourceReport {
    string Name, Descriptor;
    bool Texture{false}, External{false};
    uint64_t PhysicalId{0};
    int32_t FirstUse{-1}, LastUse{-1};
    uint64_t ViewId{0}, EstimatedBytes{0};
    uint32_t PhysicalSlot{RgInvalidIndex};
    bool Port{false}, RetainedOwner{false};
};
struct RenderGraphBarrierReport {
    uint32_t Pass, Resource, Subresource, Before, After;
    bool Uav{false};
    string ScopeReason;
};
struct RenderGraphExecutionReport {
    string Name;
    uint32_t DeclaredPasses{0}, LivePasses{0}, CulledPasses{0}, Textures{0}, Buffers{0}, PhysicalAllocations{0};
    uint32_t TransitionBarriers{0}, UavBarriers{0};
    uint32_t ReusedResources{0}, MergedRasterPasses{0}, DiscardedStores{0}, BarrierBatches{0};
    uint32_t GraphicsPipelinePreparations{0}, GraphicsPipelineCreations{0};
    uint64_t GeometryValidationCalls{0}, GeometryDeclarationScans{0};
    RenderGraphCommandCalls CommandCalls{};
    bool CompilePlanReused{false};
    uint64_t ExecutionPlanId{0};
    uint32_t NormalizeBuilds{0}, IrBuilds{0}, TopologyBuilds{0}, StoragePlanBuilds{0}, RasterPlanBuilds{0}, ExecutionPlanBuilds{0};
    uint32_t BarrierTemplateBuilds{0}, RoutePlanBuilds{0};
    /// Actual declaration calls and cold template work; instantiation borrows immutable declarations.
    uint32_t ResourceDeclarations{0}, PassDeclarations{0}, PortResolveBuilds{0};
    uint32_t TemplateInstances{0}, TemplatePlacementBuilds{0}, TemplateMaterializations{0};
    uint32_t ValidatePlanInputCalls{0}, ValidateReadyFrameCalls{0}, ReadyValidationCallbacks{0};
    /// Physical first-use cells and live-pass command destinations patched for this instance.
    uint64_t InitialStatePatches{0}, CommandRoutePatches{0};
    uint32_t DeclaredWorks{0}, LiveWorks{0}, WorkRuns{0}, WorkUploads{0};
    uint64_t WorkUploadBytes{0};
    string FirstErrorCode;
    RenderResourcePoolStats Pool;
    vector<RenderGraphPassReport> Passes;
    vector<RenderGraphResourceReport> Resources;
    vector<RenderGraphBarrierReport> Barriers;
    vector<RgResourceVersionNode> Versions;
    vector<uint32_t> ExecutionOrder;
    vector<RenderGraphDiagnostic> Diagnostics;
    string ToJson() const;
    string ToDot() const;
    string ToText() const;
};
struct RenderGraphExecutionResult {
    bool Success{false};
    bool CommandsRecorded{false};
    shared_ptr<FrameSubmission> Submission{};
};

class RgOperationTicket {
public:
    bool IsValid() const noexcept { return bool(_state); }
    FrameOperationStatus Status() const noexcept { return _state ? _state->Status() : FrameOperationStatus::Cancelled; }
    uint64_t FrameSerial() const noexcept { return _state ? _state->Serial() : 0; }

private:
    friend class RenderGraph;
    shared_ptr<FrameSubmission> _state;
};

class RgReadbackTicket {
public:
    bool IsValid() const noexcept { return _operation.IsValid(); }
    FrameOperationStatus Status() const noexcept { return _operation.Status(); }
    /// Copies owned readback bytes only after the matching submitted frame's fence.
    bool Read(vector<byte>& destination) const;

private:
    friend class RenderGraph;
    struct Storage;
    shared_ptr<Storage> _storage;
    RgOperationTicket _operation;
};

class RenderGraph;
class RenderGraphFrameResources;
class RenderGraphRasterContext;
class RenderGraphComputeContext;
class RenderGraphPrepareContext;
class PreparedRendererList;
struct DrawExecutionStats;
class RenderGraphTemplate;
class RenderGraphTemplateInstance;

template <class Data>
struct RgTemplateSlot {
    uint32_t Index{UINT32_MAX};
    uint64_t Generation{0};
    bool IsValid() const noexcept { return Index != UINT32_MAX && Generation != 0; }
};

/// Immutable CPU declarations. Recipes must own their static inputs; native resources and flight
/// data are supplied by typed instance slots and connected resource ports.
class RenderGraphTemplate {
public:
    ~RenderGraphTemplate();
    RenderGraphTemplate(const RenderGraphTemplate&) = delete;
    RenderGraphTemplate& operator=(const RenderGraphTemplate&) = delete;
    uint64_t GetGeneration() const noexcept;

private:
    friend class RenderGraph;
    struct Impl;
    explicit RenderGraphTemplate(unique_ptr<Impl> impl);
    unique_ptr<Impl> _impl;
};

/// Frame-local binding of an immutable template. Only handles issued by that template can be
/// mapped; the returned handles belong to the current graph generation.
class RenderGraphTemplateInstance {
public:
    bool IsValid() const noexcept { return bool(_graph); }
    RgTextureValue Value(RgTextureValue value) const;
    RgBufferValue Value(RgBufferValue value) const;
    RgTexturePort Value(RgTexturePort value) const;
    RgBufferPort Value(RgBufferPort value) const;
    RgPassHandle Value(RgPassHandle value) const;
    RgWorkHandle Value(RgWorkHandle value) const;
    bool Bind(RgTextureValue slot, RenderExternalTexture& texture) const;
    bool Bind(RgBufferValue slot, RenderExternalBuffer& buffer) const;
    template <class Data>
    bool Bind(RgTemplateSlot<Data> slot, shared_ptr<Data> data) const;

private:
    friend class RenderGraph;
    Nullable<RenderGraph*> _graph{nullptr};
    uint32_t _index{UINT32_MAX};
    uint64_t _generation{0};
};

class RenderGraphPassBuilder {
public:
    RgTextureViewHandle ReadTexture(RgTextureValue texture, const RgTextureViewDesc& view = {});
    RgBufferValue ReadBuffer(RgBufferValue buffer, RgBufferAccess access = RgBufferAccess::ShaderRead, render::BufferRange range = render::BufferRange::AllRange());
    RgBufferValue WriteBuffer(RgBufferValue buffer, RgBufferAccess access = RgBufferAccess::UnorderedAccess, render::BufferRange range = render::BufferRange::AllRange());
    RgBufferValue ReadWriteBuffer(RgBufferValue buffer, RgBufferAccess access = RgBufferAccess::UnorderedAccess, render::BufferRange range = render::BufferRange::AllRange());
    RgIndirectArgumentsHandle ReadIndirectArguments(RgBufferValue buffer, RgIndirectCommand command,
                                                    uint64_t offset = 0, uint32_t count = 1);
    void SetSideEffect();
    void RequireWork(RgWorkHandle work, uint64_t mask = 1);
    RgPassHandle GetPassHandle() const noexcept;
    void Reject(std::string_view code, std::string_view message, std::string_view binding = {});
    bool IsValidationFull() const noexcept;
    const RenderGraphRuntimeOptions& GetRuntimeOptions() const noexcept;

protected:
    friend class RenderGraph;
    RenderGraphPassBuilder(RenderGraph& graph, uint32_t pass) : _graph(graph), _pass(pass) {}
    RenderGraph& _graph;
    uint32_t _pass;
};
class RenderGraphRasterBuilder : public RenderGraphPassBuilder {
public:
    using RenderGraphPassBuilder::RenderGraphPassBuilder;
    RgTextureViewHandle SetColorAttachment(uint32_t slot, RgTextureValue texture, const RgColorAttachmentDesc& desc = {});
    RgTextureViewHandle SetDepthAttachment(RgTextureValue texture, const RgDepthAttachmentDesc& desc = {});
    RgTextureViewHandle WriteTexture(RgTextureValue texture, render::ShaderStages stages,
                                     const RgTextureViewDesc& view = {});
    RgTextureViewHandle ReadWriteTexture(RgTextureValue texture, render::ShaderStages stages,
                                         const RgTextureViewDesc& view = {});
};
class RenderGraphComputeBuilder : public RenderGraphPassBuilder {
public:
    using RenderGraphPassBuilder::RenderGraphPassBuilder;
    RgTextureViewHandle WriteTexture(RgTextureValue texture, const RgTextureViewDesc& view = {});
    RgTextureViewHandle ReadWriteTexture(RgTextureValue texture, const RgTextureViewDesc& view = {});
};

class RenderGraphGraphicsCommands {
public:
    /// Binds a group whose native set is already resolved: either a persistent set of read-only
    /// flight-retained resources, or one this pass built during the prepare stage.
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set, std::span<const render::ShaderParameterDynamicOffset> offsets = {}) noexcept {
        ++_calls.SetParameters;
        _encoder.BindShaderParameterSet(group, set, offsets);
    }
    void BindShaderParameterSet(const PreparedShaderGroup& group) noexcept { BindShaderParameterSet(group.Group, group.Set.Get(), group.DynamicOffsets); }
    bool SetPushConstants(render::BindingHandle binding, std::span<const byte> data) noexcept {
        ++_calls.PushConstants;
        return _encoder.SetPushConstants(binding, data);
    }
    void SetViewport(Viewport viewport) noexcept {
        ++_calls.Viewport;
        _encoder.SetViewport(viewport);
    }
    void SetScissor(Rect rect) noexcept {
        ++_calls.Scissor;
        _encoder.SetScissor(rect);
    }
    /// Graph-owned or imported geometry must pass ValidateGeometryBuffer during the prepare stage.
    void BindVertexBuffers(std::span<const render::VertexBufferBinding> bindings) noexcept {
        ++_calls.VertexBuffer;
        _encoder.BindVertexBuffers(bindings);
    }
    void BindIndexBuffer(render::IndexBufferView view) noexcept {
        ++_calls.IndexBuffer;
        _encoder.BindIndexBuffer(view);
    }
    void BindGraphicsPipelineState(render::GraphicsPipelineState* pso) noexcept {
        ++_calls.SetPipeline;
        _encoder.BindGraphicsPipelineState(pso);
    }
    void Draw(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance) noexcept {
        ++_calls.Draw;
        _encoder.Draw(vertices, instances, firstVertex, firstInstance);
    }
    void DrawIndexed(uint32_t indices, uint32_t instances, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept {
        ++_calls.DrawIndexed;
        _encoder.DrawIndexed(indices, instances, firstIndex, vertexOffset, firstInstance);
    }
    void DrawIndirect(RgIndirectArgumentsHandle arguments) noexcept;
    void DrawIndexedIndirect(RgIndirectArgumentsHandle arguments) noexcept;

private:
    friend class RenderGraphRasterContext;
    friend struct RenderGraphTestDriver;
    friend void RecordRendererList(const PreparedRendererList&, RenderGraphRasterContext&, DrawExecutionStats&);
    RenderGraphGraphicsCommands(RenderGraph& graph, uint32_t pass, render::GraphicsCommandEncoder& encoder);
    RenderGraph& _graph;
    uint32_t _pass;
    render::GraphicsCommandEncoder& _encoder;
    RenderGraphCommandCalls& _calls;
};

class RenderGraphComputeCommands {
public:
    /// Persistent read-only flight-retained sets, or sets built by this pass during the prepare stage.
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set, std::span<const render::ShaderParameterDynamicOffset> offsets = {}) noexcept {
        ++_calls.SetParameters;
        _encoder.BindShaderParameterSet(group, set, offsets);
    }
    void BindShaderParameterSet(const PreparedShaderGroup& group) noexcept { BindShaderParameterSet(group.Group, group.Set.Get(), group.DynamicOffsets); }
    bool SetPushConstants(render::BindingHandle binding, std::span<const byte> data) noexcept {
        ++_calls.PushConstants;
        return _encoder.SetPushConstants(binding, data);
    }
    void BindComputePipelineState(render::ComputePipelineState* pso) noexcept {
        ++_calls.SetPipeline;
        _encoder.BindComputePipelineState(pso);
    }
    void Dispatch(uint32_t x, uint32_t y, uint32_t z) noexcept {
        ++_calls.Dispatch;
        _encoder.Dispatch(x, y, z);
    }
    void DispatchIndirect(RgIndirectArgumentsHandle arguments) noexcept;

private:
    friend class RenderGraphComputeContext;
    RenderGraphComputeCommands(RenderGraph& graph, uint32_t pass, render::ComputeCommandEncoder& encoder);
    RenderGraph& _graph;
    uint32_t _pass;
    render::ComputeCommandEncoder& _encoder;
    RenderGraphCommandCalls& _calls;
};

class RenderGraphRasterContext {
public:
    void Fail(std::string_view message);
    RenderGraphGraphicsCommands& Encoder() noexcept { return _encoder; }
    render::TextureView* GetTextureView(RgTextureViewHandle handle) const;
    render::Buffer* GetBuffer(RgBufferValue handle) const;
    const GraphicsPassState& PassState() const noexcept;
    RgPassHandle GetPassHandle() const noexcept;

private:
    friend class RenderGraph;
    friend struct RenderGraphTestDriver;
    RenderGraphRasterContext(RenderGraph& graph, uint32_t pass, render::GraphicsCommandEncoder& encoder)
        : _graph(graph), _pass(pass), _encoder(graph, pass, encoder) {}
    RenderGraph& _graph;
    uint32_t _pass;
    RenderGraphGraphicsCommands _encoder;
};
class RenderGraphComputeContext {
public:
    RenderGraphComputeCommands& Encoder() noexcept { return _encoder; }
    render::TextureView* GetTextureView(RgTextureViewHandle handle) const;
    render::Buffer* GetBuffer(RgBufferValue handle) const;

private:
    friend class RenderGraph;
    RenderGraphComputeContext(RenderGraph& graph, uint32_t pass, render::ComputeCommandEncoder& encoder)
        : _graph(graph), _pass(pass), _encoder(graph, pass, encoder) {}
    RenderGraph& _graph;
    uint32_t _pass;
    RenderGraphComputeCommands _encoder;
};

/// Runs after realization, for live passes only: creates descriptors, resolves pipeline states and
/// builds recording data. It cannot declare accesses or change graph structure; every resource it
/// binds must already be declared by this pass.
class RenderGraphPrepareContext {
public:
    RgPassHandle GetPassHandle() const noexcept;
    bool IsValidationFull() const noexcept;
    const RenderGraphRuntimeOptions& GetRuntimeOptions() const noexcept;
    /// Full mode only: retains diagnostic data and validates it once all live preparation has
    /// finished. Callers construct this data only when IsValidationFull() is true. The callback
    /// validates existing recording data; it cannot create sets or register another callback.
    void DeferReadyValidation(shared_ptr<const void> payload, bool (*validate)(const void*, RenderGraphPrepareContext&));
    /// Realized attachment formats and sample count of this raster pass.
    const GraphicsPassState& PassState() const noexcept;
    render::TextureView* GetTextureView(RgTextureViewHandle handle) const;
    render::Buffer* GetBuffer(RgBufferValue handle) const;
    Nullable<render::GraphicsPipelineState*> ResolveGraphicsPipeline(ShaderProgram& program, const MaterialPipelineState& state,
                                                                     const PrimitiveVertexLayout& layout = {},
                                                                     PrimitiveTopology topology = PrimitiveTopology::TriangleList);
    Nullable<render::ComputePipelineState*> ResolveComputePipeline(ShaderProgram& program);
    /// Bindings reference view handles and buffer values this pass declared during setup; the access
    /// each one needs is derived from the shader declaration. Full mode retains an owned diagnostic
    /// request and an unwritten set until ValidateReadyFrame accepts all live preparation. The returned
    /// set may only be bound during recording, and is published to the flight cache after native writes succeed.
    PreparedShaderGroup CreateParameterSet(ShaderProgram& program, uint32_t group, std::span<const RgParameterBinding> bindings);
    /// Accepts persistent read-only assets; rejects graph-owned geometry without a matching
    /// Vertex/Index read declaration in this pass. Full preparation queues the check at ValidateReadyFrame;
    /// its return value accepts the request, while a validation callback receives the actual result.
    /// Call once per distinct buffer, not per draw.
    bool ValidateGeometryBuffer(Nullable<render::Buffer*> buffer, RgBufferAccess access);
    void Reject(std::string_view code, std::string_view message, std::string_view binding = {});

private:
    friend class RenderGraph;
    RenderGraphPrepareContext(RenderGraph& graph, uint32_t pass) : _graph(graph), _pass(pass) {}
    RenderGraph& _graph;
    uint32_t _pass;
};

class RenderGraph {
public:
    RenderGraph(render::Device& device, RenderResourcePool& pool, render::RenderPassRegistry& registry, std::string_view name,
                RenderGraphRuntimeOptions runtime = kDiagnosticRenderGraphRuntimeOptions);
    RenderGraph(render::Device& device, RenderGraphFrameResources& resources,
                render::RenderPassRegistry& registry, std::string_view name,
                RenderGraphRuntimeOptions runtime = kDiagnosticRenderGraphRuntimeOptions);
    ~RenderGraph();
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RgTextureValue CreateTexture(const render::TextureDescriptor& desc, std::string_view name, std::source_location location = std::source_location::current());
    RgBufferValue CreateBuffer(const render::BufferDescriptor& desc, std::string_view name, std::source_location location = std::source_location::current());
    RgTextureValue ImportTexture(RenderExternalTexture& texture, std::string_view name, RenderGraphExternalAccess access, std::source_location location = std::source_location::current());
    RgBufferValue ImportBuffer(RenderExternalBuffer& buffer, std::string_view name, RenderGraphExternalAccess access, std::source_location location = std::source_location::current());
    /// Template-only external storage contract. Instances bind native resources before compilation.
    /// Aliases share one physical version chain; conflicting imports are rejected.
    RgTextureValue DeclareExternalTexture(const render::TextureDescriptor& desc, std::string_view name, RenderGraphExternalAccess access);
    RgBufferValue DeclareExternalBuffer(const render::BufferDescriptor& desc, std::string_view name, RenderGraphExternalAccess access);
    /// Ports have a descriptor before expansion and exactly one connection before Compile.
    /// Their value may be read or advanced by a component before its producer is expanded.
    RgTexturePort DeclareTexturePort(const render::TextureDescriptor& desc, std::string_view name);
    RgBufferPort DeclareBufferPort(const render::BufferDescriptor& desc, std::string_view name);
    RgTextureValue Value(RgTexturePort port) const noexcept;
    RgBufferValue Value(RgBufferPort port) const noexcept;
    bool Connect(RgTexturePort input, RgTextureValue output);
    bool Connect(RgBufferPort input, RgBufferValue output);
    RgBufferValue UploadBuffer(std::string_view name, std::span<const byte> bytes, render::BufferUses usage);
    /// Declares a fixed-size upload whose bytes are produced only when a consumer is live.
    RgBufferValue UploadBuffer(std::string_view name, uint64_t size, render::BufferUses usage,
                               const RgUploadData& data, RgWorkHandle work, uint64_t mask = 1);
    /// Runs once with the union of masks requested by live passes; graph declarations are frozen.
    /// Work callbacks are independent and run before every pass prepare callback and upload.
    /// Data is graph-owned and retained with the submission; borrowed inputs follow the flight lifetime.
    template <class Data>
    RgWorkHandle AddWork(std::string_view name, Data data, bool (*prepare)(Data&, uint64_t)) {
        return AddWorkPayload(name, make_unique<TypedWorkPayload<Data>>(std::move(data), prepare));
    }
    RgOperationTicket Track(RgPassHandle pass);
    RgOperationTicket ExportTexture(RgTextureValue value, render::TextureStates finalState,
                                    render::SubresourceRange range = {0, render::SubresourceRange::All, 0, render::SubresourceRange::All});
    RgOperationTicket ExportBuffer(RgBufferValue value, RgBufferAccess finalAccess,
                                   render::BufferRange range = render::BufferRange::AllRange());
    RgReadbackTicket ReadbackTexture(std::string_view name, RgTextureValue value,
                                     render::SubresourceRange range = {0, 1, 0, 1});
    RgReadbackTicket ReadbackBuffer(std::string_view name, RgBufferValue value, render::BufferRange range = render::BufferRange::AllRange());
    void Retain(shared_ptr<void> owner);

    /// A template builder is an ordinary, uncompiled graph using typed slots and template passes.
    /// Freezing rejects legacy payload captures, native imports, readbacks and immediate uploads.
    shared_ptr<const RenderGraphTemplate> FreezeTemplate();
    /// Independent cold declaration graph sharing this graph's device and registry.
    RenderGraph CreateTemplateBuilder(std::string_view name) const;
    RenderGraphTemplateInstance Instantiate(shared_ptr<const RenderGraphTemplate> graphTemplate);
    bool SetColorClear(RgPassHandle pass, uint32_t slot, render::ColorClearValue clear);
    bool SetDepthClear(RgPassHandle pass, render::DepthStencilClearValue clear);
    template <class Data>
    RgTemplateSlot<Data> DeclareTemplateSlot() {
        return {AddTemplateSlot(&TemplateType<Data>), GetGeneration()};
    }
    template <class Data>
    RgWorkHandle AddTemplateWork(std::string_view name, RgTemplateSlot<Data> slot, bool (*prepare)(Data&, uint64_t)) {
        return AddTemplateWorkFactory(name, slot.Index, slot.Generation, &TemplateType<Data>, make_shared<TypedTemplateWorkFactory<Data>>(prepare));
    }
    RgBufferValue UploadBuffer(std::string_view name, uint64_t size, render::BufferUses usage,
                               RgTemplateSlot<RgUploadData> data, RgWorkHandle work, uint64_t mask = 1);
    template <class Recipe, class Data, class Setup>
    RgPassHandle AddTemplateRasterPass(std::string_view name, RgTemplateSlot<Data> slot, Setup&& setup,
                                       bool (*prepare)(const Recipe&, Data&, RenderGraphPrepareContext&),
                                       void (*execute)(const Recipe&, const Data&, RenderGraphRasterContext&),
                                       std::source_location location = std::source_location::current()) {
        auto factory = make_shared<TemplateRasterFactory<Recipe, Data>>();
        factory->PrepareStage = prepare;
        factory->Execute = execute;
        const auto pass = AddPass(name, RgPassType::Raster, location);
        if (!pass.IsValid()) return pass;
        RenderGraphRasterBuilder builder(*this, pass.Index);
        setup(factory->Value, builder);
        SetTemplateFactory(pass, slot.Index, slot.Generation, &TemplateType<Data>, std::move(factory));
        return pass;
    }
    template <class Recipe, class Data, class Setup>
    RgPassHandle AddTemplateComputePass(std::string_view name, RgTemplateSlot<Data> slot, Setup&& setup,
                                        bool (*prepare)(const Recipe&, Data&, RenderGraphPrepareContext&),
                                        void (*execute)(const Recipe&, const Data&, RenderGraphComputeContext&),
                                        std::source_location location = std::source_location::current()) {
        auto factory = make_shared<TemplateComputeFactory<Recipe, Data>>();
        factory->PrepareStage = prepare;
        factory->Execute = execute;
        const auto pass = AddPass(name, RgPassType::Compute, location);
        if (!pass.IsValid()) return pass;
        RenderGraphComputeBuilder builder(*this, pass.Index);
        setup(factory->Value, builder);
        SetTemplateFactory(pass, slot.Index, slot.Generation, &TemplateType<Data>, std::move(factory));
        return pass;
    }

    template <class Data, class Setup>
    RgPassHandle AddRasterPass(std::string_view name, Setup&& setup,
                               bool (*prepare)(Data&, RenderGraphPrepareContext&),
                               void (*execute)(const Data&, RenderGraphRasterContext&),
                               std::source_location location = std::source_location::current()) {
        auto payload = make_unique<RasterPayload<Data>>();
        payload->PrepareStage = prepare;
        payload->Execute = execute;
        const auto pass = AddPass(name, RgPassType::Raster, location);
        if (!pass.IsValid()) return pass;
        RenderGraphRasterBuilder builder(*this, pass.Index);
        setup(payload->Value, builder);
        SetPayload(pass, std::move(payload));
        return pass;
    }
    template <class Data, class Setup>
    RgPassHandle AddRasterPass(std::string_view name, Setup&& setup, void (*execute)(const Data&, RenderGraphRasterContext&),
                               std::source_location location = std::source_location::current()) {
        return AddRasterPass<Data>(name, std::forward<Setup>(setup), nullptr, execute, location);
    }
    template <class Data, class Setup>
    RgPassHandle AddComputePass(std::string_view name, Setup&& setup,
                                bool (*prepare)(Data&, RenderGraphPrepareContext&),
                                void (*execute)(const Data&, RenderGraphComputeContext&),
                                std::source_location location = std::source_location::current()) {
        auto payload = make_unique<ComputePayload<Data>>();
        payload->PrepareStage = prepare;
        payload->Execute = execute;
        const auto pass = AddPass(name, RgPassType::Compute, location);
        if (!pass.IsValid()) return pass;
        RenderGraphComputeBuilder builder(*this, pass.Index);
        setup(payload->Value, builder);
        SetPayload(pass, std::move(payload));
        return pass;
    }
    template <class Data, class Setup>
    RgPassHandle AddComputePass(std::string_view name, Setup&& setup, void (*execute)(const Data&, RenderGraphComputeContext&),
                                std::source_location location = std::source_location::current()) {
        return AddComputePass<Data>(name, std::forward<Setup>(setup), nullptr, execute, location);
    }
    RgPassHandle AddCopyBufferPass(std::string_view name, RgBufferValue source, RgBufferValue destination,
                                   uint64_t size, uint64_t sourceOffset = 0, uint64_t destinationOffset = 0,
                                   std::source_location location = std::source_location::current());
    RgPassHandle AddCopyTexturePass(std::string_view name, RgTextureValue source, RgTextureValue destination,
                                    render::SubresourceRange sourceRange = {0, 1, 0, 1},
                                    render::SubresourceRange destinationRange = {0, 1, 0, 1},
                                    std::source_location location = std::source_location::current());
    RgPassHandle AddCopyTextureToBufferPass(std::string_view name, RgTextureValue source, RgBufferValue destination,
                                            render::SubresourceRange range = {0, 1, 0, 1}, uint64_t destinationOffset = 0,
                                            std::source_location location = std::source_location::current());
    RgPassHandle AddResolveTexturePass(std::string_view name, RgTextureValue source, RgTextureValue destination,
                                       render::SubresourceRange sourceRange = {0, 1, 0, 1},
                                       render::SubresourceRange destinationRange = {0, 1, 0, 1},
                                       std::source_location location = std::source_location::current());
    RgPassHandle AddCopyBufferToTexturePass(std::string_view name, RgBufferValue source, RgTextureValue destination,
                                            const render::BufferTextureCopyRegion& region,
                                            std::source_location location = std::source_location::current());
    /// Reserves a successor content version. Writes target the returned version;
    /// ReadWrite/Load consume its predecessor. Creation reserves the first writable version.
    RgTextureValue NextVersion(RgTextureValue value);
    RgBufferValue NextVersion(RgBufferValue value);
    /// Freezes setup and validates/culls the graph without creating native resources or recording commands.
    bool Compile();
    void SetCompileOptions(RenderGraphCompileOptions options);
    const CompiledRenderGraph& GetCompiledGraph() const noexcept;
    const RenderGraphExecutionReport& GetReport() const noexcept;
    const RenderGraphRuntimeOptions& GetRuntimeOptions() const noexcept;
    bool IsValidationFull() const noexcept;
    bool HasFailed() const noexcept;
    uint32_t GetPassCount() const noexcept;
    std::string_view GetFirstErrorCode() const noexcept;
    bool WasWritten(RgTextureValue handle) const noexcept;
    bool WasWritten(const RenderExternalTexture& texture) const noexcept;
    std::optional<render::TextureStates> RecordedTextureState(RgTextureValue handle, uint32_t subresource = 0) const noexcept;
    bool WasPassExecuted(RgPassHandle handle) const noexcept;
    bool PassWroteTexture(RgPassHandle pass, RgTextureValue texture) const noexcept;
    uint64_t GetGeneration() const noexcept;
    /// Attributes subsequent resources for memory diagnostics; zero means shared/unclassified.
    /// Returns the previous scope so nested graph helpers can restore it.
    uint64_t SetResourceView(uint64_t viewId);
    std::optional<render::TextureDescriptor> GetTextureDescriptor(RgTextureValue texture) const noexcept;
    /// Add a setup diagnostic, preventing graph execution.
    void AddDiagnostic(std::string_view code, std::string_view message);

private:
    friend class RenderGraphTemplate;
    friend class RenderGraphTemplateInstance;
    friend class RenderPipelineContext;
    friend class RenderGraphPassBuilder;
    friend class RenderGraphRasterBuilder;
    friend class RenderGraphComputeBuilder;
    friend class RenderGraphGraphicsCommands;
    friend class RenderGraphComputeCommands;
    friend class RenderGraphRasterContext;
    friend class RenderGraphComputeContext;
    friend class RenderGraphPrepareContext;
    friend struct RenderGraphTestDriver;
    RenderGraph(render::Device& device, RenderGraphFrameResources& resources,
                render::RenderPassRegistry& registry, std::string_view name, uint64_t& generation, RenderGraphExecutionReport& report,
                RenderGraphRuntimeOptions runtime = kDiagnosticRenderGraphRuntimeOptions);
    RenderGraphExecutionResult Execute(render::CommandBuffer& command);
    struct PresentCommandTarget {
        render::Texture* Texture{nullptr};
        render::CommandBuffer* Commands{nullptr};
    };
    RenderGraphExecutionResult Execute(render::CommandBuffer& command, std::span<const PresentCommandTarget> presentTargets);
    struct Payload {
        virtual ~Payload() = default;
        virtual bool Prepare(RenderGraphPrepareContext&) { return true; }
        virtual void Run(RenderGraphRasterContext&) {}
        virtual void Run(RenderGraphComputeContext&) {}
    };
    template <class Data>
    inline static byte TemplateType{};
    struct TemplateFactory {
        virtual ~TemplateFactory() = default;
        virtual unique_ptr<Payload> Instantiate(void* frame) const = 0;
    };
    template <class Recipe, class Data>
    struct TemplateRasterFactory final : TemplateFactory {
        Recipe Value{};
        bool (*PrepareStage)(const Recipe&, Data&, RenderGraphPrepareContext&){nullptr};
        void (*Execute)(const Recipe&, const Data&, RenderGraphRasterContext&){nullptr};
        struct Instance final : Payload {
            const TemplateRasterFactory& Factory;
            Data& Frame;
            Instance(const TemplateRasterFactory& factory, Data& frame) : Factory(factory), Frame(frame) {}
            bool Prepare(RenderGraphPrepareContext& context) override { return !Factory.PrepareStage || Factory.PrepareStage(Factory.Value, Frame, context); }
            void Run(RenderGraphRasterContext& context) override {
                if (Factory.Execute) Factory.Execute(Factory.Value, Frame, context);
            }
        };
        unique_ptr<Payload> Instantiate(void* frame) const override { return make_unique<Instance>(*this, *static_cast<Data*>(frame)); }
    };
    template <class Recipe, class Data>
    struct TemplateComputeFactory final : TemplateFactory {
        Recipe Value{};
        bool (*PrepareStage)(const Recipe&, Data&, RenderGraphPrepareContext&){nullptr};
        void (*Execute)(const Recipe&, const Data&, RenderGraphComputeContext&){nullptr};
        struct Instance final : Payload {
            const TemplateComputeFactory& Factory;
            Data& Frame;
            Instance(const TemplateComputeFactory& factory, Data& frame) : Factory(factory), Frame(frame) {}
            bool Prepare(RenderGraphPrepareContext& context) override { return !Factory.PrepareStage || Factory.PrepareStage(Factory.Value, Frame, context); }
            void Run(RenderGraphComputeContext& context) override {
                if (Factory.Execute) Factory.Execute(Factory.Value, Frame, context);
            }
        };
        unique_ptr<Payload> Instantiate(void* frame) const override { return make_unique<Instance>(*this, *static_cast<Data*>(frame)); }
    };
    template <class Data>
    struct RasterPayload final : Payload {
        Data Value{};
        bool (*PrepareStage)(Data&, RenderGraphPrepareContext&){nullptr};
        void (*Execute)(const Data&, RenderGraphRasterContext&){nullptr};
        bool Prepare(RenderGraphPrepareContext& context) override {
            return PrepareStage == nullptr || PrepareStage(Value, context);
        }
        void Run(RenderGraphRasterContext& context) override {
            if (Execute) Execute(Value, context);
        }
    };
    struct WorkPayload {
        virtual ~WorkPayload() = default;
        virtual bool Prepare(uint64_t mask) = 0;
    };
    struct TemplateWorkFactory {
        virtual ~TemplateWorkFactory() = default;
        virtual unique_ptr<WorkPayload> Instantiate(void* frame) const = 0;
    };
    template <class Data>
    struct TypedTemplateWorkFactory final : TemplateWorkFactory {
        bool (*Function)(Data&, uint64_t);
        explicit TypedTemplateWorkFactory(bool (*function)(Data&, uint64_t)) : Function(function) {}
        struct Instance final : WorkPayload {
            Data& Frame;
            bool (*Function)(Data&, uint64_t);
            Instance(Data& frame, bool (*function)(Data&, uint64_t)) : Frame(frame), Function(function) {}
            bool Prepare(uint64_t mask) override { return Function && Function(Frame, mask); }
        };
        unique_ptr<WorkPayload> Instantiate(void* frame) const override { return make_unique<Instance>(*static_cast<Data*>(frame), Function); }
    };
    template <class Data>
    struct TypedWorkPayload final : WorkPayload {
        Data Value;
        bool (*Function)(Data&, uint64_t);
        TypedWorkPayload(Data data, bool (*function)(Data&, uint64_t)) : Value(std::move(data)), Function(function) {}
        bool Prepare(uint64_t mask) override { return Function != nullptr && Function(Value, mask); }
    };
    template <class Data>
    struct ComputePayload final : Payload {
        Data Value{};
        bool (*PrepareStage)(Data&, RenderGraphPrepareContext&){nullptr};
        void (*Execute)(const Data&, RenderGraphComputeContext&){nullptr};
        bool Prepare(RenderGraphPrepareContext& context) override {
            return PrepareStage == nullptr || PrepareStage(Value, context);
        }
        void Run(RenderGraphComputeContext& context) override {
            if (Execute) Execute(Value, context);
        }
    };
    struct Impl;
    unique_ptr<Impl> _impl;
    RgPassHandle AddPass(std::string_view name, RgPassType type, std::source_location location);
    void SetPayload(RgPassHandle pass, unique_ptr<Payload> payload);
    uint32_t AddTemplateSlot(const void* type);
    void SetTemplateFactory(RgPassHandle pass, uint32_t slot, uint64_t generation, const void* type, shared_ptr<const TemplateFactory> factory);
    RgWorkHandle AddTemplateWorkFactory(std::string_view name, uint32_t slot, uint64_t generation, const void* type, shared_ptr<const TemplateWorkFactory> factory);
    bool BindTemplateSlot(const RenderGraphTemplateInstance& instance, uint32_t slot, uint64_t generation, const void* type, shared_ptr<void> data);
    bool BindExternalTexture(RgTextureValue slot, RenderExternalTexture& texture);
    bool BindExternalBuffer(RgBufferValue slot, RenderExternalBuffer& buffer);
    uint32_t MapTemplateHandle(const RenderGraphTemplateInstance& instance, uint32_t index, uint64_t generation, uint8_t kind, uint32_t version = UINT32_MAX) const;
    RgWorkHandle AddWorkPayload(std::string_view name, unique_ptr<WorkPayload> payload);
    void RequireWork(uint32_t pass, RgWorkHandle work, uint64_t mask);
    bool IsPreparingWork() const noexcept;
    RgTextureViewHandle UseTexture(uint32_t pass, RgTextureValue texture, RgTextureViewDesc view,
                                   render::TextureViewUsage usage, bool read, bool write, bool validAfter,
                                   render::ShaderStages uavWriteStages = render::ShaderStage::UNKNOWN);
    RgBufferValue UseBuffer(uint32_t pass, RgBufferValue buffer, RgBufferAccess access, bool read, bool write,
                            render::ShaderStages uavWriteStages = render::ShaderStage::UNKNOWN, render::BufferRange range = render::BufferRange::AllRange());
    RgIndirectArgumentsHandle AddIndirectArguments(uint32_t pass, RgBufferValue buffer,
                                                   RgIndirectCommand command, uint64_t offset, uint32_t count);
    Nullable<render::ComputePipelineState*> ResolveComputePipeline(uint32_t pass, ShaderProgram& program);
    Nullable<render::GraphicsPipelineState*> ResolveGraphicsPipeline(uint32_t pass, ShaderProgram& program, const MaterialPipelineState& state,
                                                                     const PrimitiveVertexLayout& layout, PrimitiveTopology topology);
    PreparedShaderGroup CreateParameterSet(uint32_t pass, ShaderProgram& program, uint32_t group,
                                           std::span<const RgParameterBinding> bindings);
    bool ValidateGeometryBuffer(uint32_t pass, Nullable<render::Buffer*> buffer, RgBufferAccess access);
    render::TextureView* ResolveView(uint32_t pass, RgTextureViewHandle handle) const;
    render::Buffer* ResolveBuffer(uint32_t pass, RgBufferValue handle) const;
    void ExecuteIndirect(uint32_t pass, RgIndirectArgumentsHandle handle, RgIndirectCommand expected,
                         render::GraphicsCommandEncoder* graphics, render::ComputeCommandEncoder* compute) noexcept;
    bool Prepare();
    bool ValidateReadyFrame();
};

template <class Data>
bool RenderGraphTemplateInstance::Bind(RgTemplateSlot<Data> slot, shared_ptr<Data> data) const {
    return _graph && _graph->BindTemplateSlot(*this, slot.Index, slot.Generation, &RenderGraph::TemplateType<Data>, std::move(data));
}

}  // namespace radray
