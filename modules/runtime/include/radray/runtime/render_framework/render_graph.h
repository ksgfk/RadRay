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
    bool CompilePlanReused{false};
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

class RenderGraphPassBuilder {
public:
    RgTextureViewHandle ReadTexture(RgTextureValue texture, const RgTextureViewDesc& view = {});
    RgBufferValue ReadBuffer(RgBufferValue buffer, RgBufferAccess access = RgBufferAccess::ShaderRead, render::BufferRange range = render::BufferRange::AllRange());
    RgBufferValue WriteBuffer(RgBufferValue buffer, RgBufferAccess access = RgBufferAccess::UnorderedAccess, render::BufferRange range = render::BufferRange::AllRange());
    RgBufferValue ReadWriteBuffer(RgBufferValue buffer, RgBufferAccess access = RgBufferAccess::UnorderedAccess, render::BufferRange range = render::BufferRange::AllRange());
    RgIndirectArgumentsHandle ReadIndirectArguments(RgBufferValue buffer, RgIndirectCommand command,
                                                    uint64_t offset = 0, uint32_t count = 1);
    void SetSideEffect();
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
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set, std::span<const render::ShaderParameterDynamicOffset> offsets = {}) noexcept { _encoder.BindShaderParameterSet(group, set, offsets); }
    void BindShaderParameterSet(const PreparedShaderGroup& group) noexcept { _encoder.BindShaderParameterSet(group.Group, group.Set.Get(), group.DynamicOffsets); }
    bool SetPushConstants(render::BindingHandle binding, std::span<const byte> data) noexcept { return _encoder.SetPushConstants(binding, data); }
    void SetViewport(Viewport viewport) noexcept { _encoder.SetViewport(viewport); }
    void SetScissor(Rect rect) noexcept { _encoder.SetScissor(rect); }
    /// Graph-owned or imported geometry must pass ValidateGeometryBuffer during the prepare stage.
    void BindVertexBuffers(std::span<const render::VertexBufferBinding> bindings) noexcept { _encoder.BindVertexBuffers(bindings); }
    void BindIndexBuffer(render::IndexBufferView view) noexcept { _encoder.BindIndexBuffer(view); }
    void BindGraphicsPipelineState(render::GraphicsPipelineState* pso) noexcept { _encoder.BindGraphicsPipelineState(pso); }
    void Draw(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance) noexcept {
        _encoder.Draw(vertices, instances, firstVertex, firstInstance);
    }
    void DrawIndexed(uint32_t indices, uint32_t instances, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept {
        _encoder.DrawIndexed(indices, instances, firstIndex, vertexOffset, firstInstance);
    }
    void DrawIndirect(RgIndirectArgumentsHandle arguments) noexcept;
    void DrawIndexedIndirect(RgIndirectArgumentsHandle arguments) noexcept;

private:
    friend class RenderGraphRasterContext;
    RenderGraphGraphicsCommands(RenderGraph& graph, uint32_t pass, render::GraphicsCommandEncoder& encoder)
        : _graph(graph), _pass(pass), _encoder(encoder) {}
    RenderGraph& _graph;
    uint32_t _pass;
    render::GraphicsCommandEncoder& _encoder;
};

class RenderGraphComputeCommands {
public:
    /// Persistent read-only flight-retained sets, or sets built by this pass during the prepare stage.
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set, std::span<const render::ShaderParameterDynamicOffset> offsets = {}) noexcept { _encoder.BindShaderParameterSet(group, set, offsets); }
    void BindShaderParameterSet(const PreparedShaderGroup& group) noexcept { _encoder.BindShaderParameterSet(group.Group, group.Set.Get(), group.DynamicOffsets); }
    bool SetPushConstants(render::BindingHandle binding, std::span<const byte> data) noexcept { return _encoder.SetPushConstants(binding, data); }
    void BindComputePipelineState(render::ComputePipelineState* pso) noexcept { _encoder.BindComputePipelineState(pso); }
    void Dispatch(uint32_t x, uint32_t y, uint32_t z) noexcept { _encoder.Dispatch(x, y, z); }
    void DispatchIndirect(RgIndirectArgumentsHandle arguments) noexcept;

private:
    friend class RenderGraphComputeContext;
    RenderGraphComputeCommands(RenderGraph& graph, uint32_t pass, render::ComputeCommandEncoder& encoder)
        : _graph(graph), _pass(pass), _encoder(encoder) {}
    RenderGraph& _graph;
    uint32_t _pass;
    render::ComputeCommandEncoder& _encoder;
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
    /// Realized attachment formats and sample count of this raster pass.
    const GraphicsPassState& PassState() const noexcept;
    render::TextureView* GetTextureView(RgTextureViewHandle handle) const;
    render::Buffer* GetBuffer(RgBufferValue handle) const;
    Nullable<render::GraphicsPipelineState*> ResolveGraphicsPipeline(ShaderProgram& program, const MaterialPipelineState& state,
                                                                    const PrimitiveVertexLayout& layout = {},
                                                                    PrimitiveTopology topology = PrimitiveTopology::TriangleList);
    Nullable<render::ComputePipelineState*> ResolveComputePipeline(ShaderProgram& program);
    /// Bindings reference view handles and buffer values this pass declared during setup; the access
    /// each one needs is derived from the shader declaration and cross-checked against that plan.
    PreparedShaderGroup CreateParameterSet(ShaderProgram& program, uint32_t group, std::span<const RgParameterBinding> bindings);
    /// Accepts persistent read-only assets; rejects graph-owned geometry without a matching
    /// Vertex/Index read declaration in this pass. Call once per distinct buffer, not per draw.
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
    /// Ports have a descriptor before expansion and exactly one connection before Compile.
    /// Their value may be read or advanced by a component before its producer is expanded.
    RgTexturePort DeclareTexturePort(const render::TextureDescriptor& desc, std::string_view name);
    RgBufferPort DeclareBufferPort(const render::BufferDescriptor& desc, std::string_view name);
    RgTextureValue Value(RgTexturePort port) const noexcept;
    RgBufferValue Value(RgBufferPort port) const noexcept;
    bool Connect(RgTexturePort input, RgTextureValue output);
    bool Connect(RgBufferPort input, RgBufferValue output);
    RgBufferValue UploadBuffer(std::string_view name, std::span<const byte> bytes, render::BufferUses usage);
    RgOperationTicket Track(RgPassHandle pass);
    RgOperationTicket ExportTexture(RgTextureValue value, render::TextureStates finalState,
                                    render::SubresourceRange range = {0, render::SubresourceRange::All, 0, render::SubresourceRange::All});
    RgOperationTicket ExportBuffer(RgBufferValue value, RgBufferAccess finalAccess,
                                   render::BufferRange range = render::BufferRange::AllRange());
    RgReadbackTicket ReadbackTexture(std::string_view name, RgTextureValue value,
                                     render::SubresourceRange range = {0, 1, 0, 1});
    RgReadbackTicket ReadbackBuffer(std::string_view name, RgBufferValue value, render::BufferRange range = render::BufferRange::AllRange());
    void Retain(shared_ptr<void> owner);

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
};

}  // namespace radray
