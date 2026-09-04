#pragma once

#include <source_location>
#include <radray/runtime/render_framework/render_resource_pool.h>
#include <radray/runtime/shader_program.h>

namespace radray {

template <class Tag>
struct RgHandle {
    uint32_t Index{UINT32_MAX};
    uint64_t Generation{0};
    bool IsValid() const noexcept { return Index != UINT32_MAX && Generation != 0; }
    friend bool operator==(const RgHandle&, const RgHandle&) = default;
};
using RgTextureHandle = RgHandle<struct RgTextureTag>;
using RgBufferHandle = RgHandle<struct RgBufferTag>;
using RgTextureViewHandle = RgHandle<struct RgTextureViewTag>;
using RgPassHandle = RgHandle<struct RgPassTag>;

enum class RenderGraphExternalAccess : uint8_t { ReadOnly,
                                                 ReadWrite,
                                                 ObservableOutput };
enum class RgPassType : uint8_t { Raster,
                                  Compute,
                                  Copy };
enum class RgBufferAccess : uint8_t { Vertex,
                                      Index,
                                      Constant,
                                      ShaderRead,
                                      UnorderedAccess,
                                      CopySource,
                                      CopyDestination,
                                      HostRead };

struct RenderExternalTexture {
    render::Texture* Texture;
    render::TextureDescriptor Desc;
    std::span<render::TextureStates> SubresourceStates;
    std::span<uint8_t> ContentValid;
    Nullable<render::TextureView*> ColorAttachmentView{nullptr};
    bool Written{false};
    Nullable<vector<PooledTextureView>*> PersistentViews{nullptr};
};
struct RenderExternalBuffer {
    render::Buffer* Buffer;
    render::BufferDescriptor Desc;
    render::BufferStates State;
    bool ContentValid{false};
    bool Written{false};
};
struct RgTextureViewDesc {
    render::TextureDimension Dimension{render::TextureDimension::UNKNOWN};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
    render::SubresourceRange Range{0, render::SubresourceRange::All, 0, render::SubresourceRange::All};
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
struct RenderGraphDiagnostic {
    string Code, Graph, Pass, Resource, Message, File;
    uint32_t Line{0};
};
struct RenderGraphPassReport {
    string Name, File;
    uint32_t Line{0};
    RgPassType Type;
    bool Live{false}, Executed{false};
    vector<uint32_t> DataDependencies{}, HazardDependencies{};
    string LivenessReason{};
};
struct RenderGraphResourceReport {
    string Name, Descriptor;
    bool Texture{false}, External{false};
    uint64_t PhysicalId{0};
    int32_t FirstUse{-1}, LastUse{-1};
};
struct RenderGraphBarrierReport {
    uint32_t Pass, Resource, Subresource, Before, After;
    bool Uav{false};
};
struct RenderGraphExecutionReport {
    string Name;
    uint32_t DeclaredPasses{0}, LivePasses{0}, CulledPasses{0}, Textures{0}, Buffers{0}, PhysicalAllocations{0};
    uint32_t TransitionBarriers{0}, UavBarriers{0};
    RenderResourcePoolStats Pool;
    vector<RenderGraphPassReport> Passes;
    vector<RenderGraphResourceReport> Resources;
    vector<RenderGraphBarrierReport> Barriers;
    vector<RenderGraphDiagnostic> Diagnostics;
    string ToJson() const;
    string ToDot() const;
    string ToText() const;
};
struct RenderGraphExecutionResult {
    bool Success{false};
    bool CommandsRecorded{false};
};

class RenderGraph;
class RenderGraphRasterContext;
class RenderGraphComputeContext;

class RenderGraphPassBuilder {
public:
    RgTextureViewHandle ReadTexture(RgTextureHandle texture, const RgTextureViewDesc& view = {});
    RgBufferHandle ReadBuffer(RgBufferHandle buffer, RgBufferAccess access = RgBufferAccess::ShaderRead);
    RgBufferHandle WriteBuffer(RgBufferHandle buffer, RgBufferAccess access = RgBufferAccess::UnorderedAccess);
    RgBufferHandle ReadWriteBuffer(RgBufferHandle buffer, RgBufferAccess access = RgBufferAccess::UnorderedAccess);
    void SetSideEffect();

protected:
    friend class RenderGraph;
    RenderGraphPassBuilder(RenderGraph& graph, uint32_t pass) : _graph(graph), _pass(pass) {}
    RenderGraph& _graph;
    uint32_t _pass;
};
class RenderGraphRasterBuilder : public RenderGraphPassBuilder {
public:
    using RenderGraphPassBuilder::RenderGraphPassBuilder;
    RgTextureViewHandle SetColorAttachment(uint32_t slot, RgTextureHandle texture, const RgColorAttachmentDesc& desc = {});
    RgTextureViewHandle SetDepthAttachment(RgTextureHandle texture, const RgDepthAttachmentDesc& desc = {});
};
class RenderGraphComputeBuilder : public RenderGraphPassBuilder {
public:
    using RenderGraphPassBuilder::RenderGraphPassBuilder;
    RgTextureViewHandle WriteTexture(RgTextureHandle texture, const RgTextureViewDesc& view = {});
    RgTextureViewHandle ReadWriteTexture(RgTextureHandle texture, const RgTextureViewDesc& view = {});
};

class RenderGraphGraphicsCommands {
public:
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set, std::span<const render::ShaderParameterDynamicOffset> offsets = {}) noexcept { _encoder.BindShaderParameterSet(group, set, offsets); }
    bool SetPushConstants(render::BindingHandle binding, std::span<const byte> data) noexcept { return _encoder.SetPushConstants(binding, data); }
    void SetViewport(Viewport viewport) noexcept { _encoder.SetViewport(viewport); }
    void SetScissor(Rect rect) noexcept { _encoder.SetScissor(rect); }
    void BindVertexBuffers(std::span<const render::VertexBufferBinding> bindings) noexcept { _encoder.BindVertexBuffers(bindings); }
    void BindIndexBuffer(render::IndexBufferView view) noexcept { _encoder.BindIndexBuffer(view); }
    void BindGraphicsPipelineState(render::GraphicsPipelineState* pso) noexcept { _encoder.BindGraphicsPipelineState(pso); }
    void Draw(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance) noexcept { _encoder.Draw(vertices, instances, firstVertex, firstInstance); }
    void DrawIndexed(uint32_t indices, uint32_t instances, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept { _encoder.DrawIndexed(indices, instances, firstIndex, vertexOffset, firstInstance); }
    void DrawIndirect(render::Buffer* arguments, uint64_t offset, uint32_t count = 1) noexcept { _encoder.DrawIndirect(arguments, offset, count); }
    void DrawIndexedIndirect(render::Buffer* arguments, uint64_t offset, uint32_t count = 1) noexcept { _encoder.DrawIndexedIndirect(arguments, offset, count); }

private:
    friend class RenderGraphRasterContext;
    explicit RenderGraphGraphicsCommands(render::GraphicsCommandEncoder& encoder) : _encoder(encoder) {}
    render::GraphicsCommandEncoder& _encoder;
};

class RenderGraphComputeCommands {
public:
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set, std::span<const render::ShaderParameterDynamicOffset> offsets = {}) noexcept { _encoder.BindShaderParameterSet(group, set, offsets); }
    bool SetPushConstants(render::BindingHandle binding, std::span<const byte> data) noexcept { return _encoder.SetPushConstants(binding, data); }
    void BindComputePipelineState(render::ComputePipelineState* pso) noexcept { _encoder.BindComputePipelineState(pso); }
    void Dispatch(uint32_t x, uint32_t y, uint32_t z) noexcept { _encoder.Dispatch(x, y, z); }
    void DispatchIndirect(render::Buffer* arguments, uint64_t offset) noexcept { _encoder.DispatchIndirect(arguments, offset); }

private:
    friend class RenderGraphComputeContext;
    explicit RenderGraphComputeCommands(render::ComputeCommandEncoder& encoder) : _encoder(encoder) {}
    render::ComputeCommandEncoder& _encoder;
};

class RenderGraphRasterContext {
public:
    RenderGraphGraphicsCommands& Encoder() noexcept { return _encoder; }
    render::TextureView* GetTextureView(RgTextureViewHandle handle) const;
    render::Buffer* GetBuffer(RgBufferHandle handle) const;
    const GraphicsPassState& PassState() const noexcept;

private:
    friend class RenderGraph;
    RenderGraphRasterContext(RenderGraph& graph, uint32_t pass, render::GraphicsCommandEncoder& encoder)
        : _graph(graph), _pass(pass), _encoder(encoder) {}
    RenderGraph& _graph;
    uint32_t _pass;
    RenderGraphGraphicsCommands _encoder;
};
class RenderGraphComputeContext {
public:
    RenderGraphComputeCommands& Encoder() noexcept { return _encoder; }
    render::TextureView* GetTextureView(RgTextureViewHandle handle) const;
    render::Buffer* GetBuffer(RgBufferHandle handle) const;

private:
    friend class RenderGraph;
    RenderGraphComputeContext(RenderGraph& graph, uint32_t pass, render::ComputeCommandEncoder& encoder)
        : _graph(graph), _pass(pass), _encoder(encoder) {}
    RenderGraph& _graph;
    uint32_t _pass;
    RenderGraphComputeCommands _encoder;
};

class RenderGraph {
public:
    RenderGraph(render::Device& device, RenderResourcePool& pool, render::RenderPassRegistry& registry, std::string_view name);
    ~RenderGraph();
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RgTextureHandle CreateTexture(const render::TextureDescriptor& desc, std::string_view name, std::source_location location = std::source_location::current());
    RgBufferHandle CreateBuffer(const render::BufferDescriptor& desc, std::string_view name, std::source_location location = std::source_location::current());
    RgTextureHandle ImportTexture(RenderExternalTexture& texture, std::string_view name, RenderGraphExternalAccess access, std::source_location location = std::source_location::current());
    RgBufferHandle ImportBuffer(RenderExternalBuffer& buffer, std::string_view name, RenderGraphExternalAccess access, std::source_location location = std::source_location::current());

    template <class Data, class Setup>
    RgPassHandle AddRasterPass(std::string_view name, Setup&& setup, void (*execute)(const Data&, RenderGraphRasterContext&),
                               std::source_location location = std::source_location::current()) {
        auto payload = make_unique<RasterPayload<Data>>();
        payload->Execute = execute;
        const auto pass = AddPass(name, RgPassType::Raster, location);
        if (!pass.IsValid()) return pass;
        RenderGraphRasterBuilder builder(*this, pass.Index);
        setup(payload->Value, builder);
        SetPayload(pass, std::move(payload));
        return pass;
    }
    template <class Data, class Setup>
    RgPassHandle AddComputePass(std::string_view name, Setup&& setup, void (*execute)(const Data&, RenderGraphComputeContext&),
                                std::source_location location = std::source_location::current()) {
        auto payload = make_unique<ComputePayload<Data>>();
        payload->Execute = execute;
        const auto pass = AddPass(name, RgPassType::Compute, location);
        if (!pass.IsValid()) return pass;
        RenderGraphComputeBuilder builder(*this, pass.Index);
        setup(payload->Value, builder);
        SetPayload(pass, std::move(payload));
        return pass;
    }
    RgPassHandle AddCopyBufferPass(std::string_view name, RgBufferHandle source, RgBufferHandle destination,
                                   uint64_t size, uint64_t sourceOffset = 0, uint64_t destinationOffset = 0,
                                   std::source_location location = std::source_location::current());
    RgPassHandle AddCopyTexturePass(std::string_view name, RgTextureHandle source, RgTextureHandle destination,
                                    render::SubresourceRange sourceRange = {0, 1, 0, 1},
                                    render::SubresourceRange destinationRange = {0, 1, 0, 1},
                                    std::source_location location = std::source_location::current());
    RgPassHandle AddCopyTextureToBufferPass(std::string_view name, RgTextureHandle source, RgBufferHandle destination,
                                            render::SubresourceRange range = {0, 1, 0, 1}, uint64_t destinationOffset = 0,
                                            std::source_location location = std::source_location::current());
    /// Freezes setup and validates/culls the graph without creating native resources or recording commands.
    bool Compile();
    const RenderGraphExecutionReport& GetReport() const noexcept;
    bool WasWritten(RgTextureHandle handle) const noexcept;
    uint64_t GetGeneration() const noexcept;

private:
    friend class RenderPipelineContext;
    friend class RenderGraphPassBuilder;
    friend class RenderGraphRasterBuilder;
    friend class RenderGraphComputeBuilder;
    friend class RenderGraphRasterContext;
    friend class RenderGraphComputeContext;
    friend struct RenderGraphTestDriver;
    RenderGraph(render::Device& device, RenderResourcePool& pool, render::RenderPassRegistry& registry, std::string_view name, uint64_t& generation);
    RenderGraphExecutionResult Execute(render::CommandBuffer& command);
    struct Payload {
        virtual ~Payload() = default;
        virtual void Run(RenderGraphRasterContext&) {}
        virtual void Run(RenderGraphComputeContext&) {}
    };
    template <class Data>
    struct RasterPayload final : Payload {
        Data Value{};
        void (*Execute)(const Data&, RenderGraphRasterContext&){nullptr};
        void Run(RenderGraphRasterContext& context) override {
            if (Execute) Execute(Value, context);
        }
    };
    template <class Data>
    struct ComputePayload final : Payload {
        Data Value{};
        void (*Execute)(const Data&, RenderGraphComputeContext&){nullptr};
        void Run(RenderGraphComputeContext& context) override {
            if (Execute) Execute(Value, context);
        }
    };
    struct Impl;
    unique_ptr<Impl> _impl;
    RgPassHandle AddPass(std::string_view name, RgPassType type, std::source_location location);
    void SetPayload(RgPassHandle pass, unique_ptr<Payload> payload);
    RgTextureViewHandle UseTexture(uint32_t pass, RgTextureHandle texture, RgTextureViewDesc view,
                                   render::TextureViewUsage usage, bool read, bool write, bool validAfter);
    RgBufferHandle UseBuffer(uint32_t pass, RgBufferHandle buffer, RgBufferAccess access, bool read, bool write);
    render::TextureView* ResolveView(uint32_t pass, RgTextureViewHandle handle) const;
    render::Buffer* ResolveBuffer(uint32_t pass, RgBufferHandle handle) const;
};

}  // namespace radray
