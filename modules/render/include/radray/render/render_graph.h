#pragma once

#include <string_view>
#include <functional>
#include <optional>

#include <radray/types.h>
#include <radray/render/common.h>

namespace radray::render::rg {

// ============================================================
// Handle — Index + Version (SSA-style versioning)
// ============================================================

struct RGTextureHandle {
    uint32_t Index{~0u};
    uint32_t Version{0};
    bool IsValid() const noexcept { return Index != ~0u; }
};

struct RGBufferHandle {
    uint32_t Index{~0u};
    uint32_t Version{0};
    bool IsValid() const noexcept { return Index != ~0u; }
};

// ============================================================
// Transient resource descriptors
// ============================================================

struct RGTextureDescriptor {
    TextureDimension Dim{TextureDimension::Dim2D};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t DepthOrArraySize{1};
    uint32_t MipLevels{1};
    uint32_t SampleCount{1};
    TextureFormat Format{TextureFormat::UNKNOWN};
    TextureUses Usage{TextureUse::UNKNOWN};
    std::string_view Name{};
};

struct RGBufferDescriptor {
    uint64_t Size{0};
    BufferUses Usage{BufferUse::UNKNOWN};
    std::string_view Name{};
};

// ============================================================
// Resource usage enums
// ============================================================

enum class RGAccessFlags : uint8_t {
    Read = 0x01,
    Write = 0x02,
    ReadWrite = Read | Write
};

enum class RGResourceUsage : uint8_t {
    ShaderResource,
    UnorderedAccess,
    CopySource,
    CopyDestination,
    ColorAttachment,
    DepthStencilRead,
    DepthStencilWrite,
    Vertex,
    Index,
    Indirect,
    AccelerationStructureInput,
    AccelerationStructureRead,
};

// ============================================================
// Pass enums
// ============================================================

enum class RGPassType : uint8_t {
    Raster,
    Compute,
    Copy,
    RayTracing
};

enum class RGPassFlags : uint8_t {
    None = 0x0,
    NeverCull = 0x1,
};

// ============================================================
// Compiled graph structures
// ============================================================

struct RGCompiledPass {
    uint32_t OriginalPassIndex;
    radray::vector<ResourceBarrierDescriptor> PreBarriers;
    // Raster pass attachment info — resolved to real TextureView* at execute time
    struct RGColorAttachmentDesc {
        uint32_t TextureResourceIndex;
        LoadAction Load{LoadAction::DontCare};
        StoreAction Store{StoreAction::Store};
        ColorClearValue ClearValue{};
    };
    struct RGDepthStencilAttachmentDesc {
        uint32_t TextureResourceIndex;
        LoadAction DepthLoad{};
        StoreAction DepthStore{};
        LoadAction StencilLoad{};
        StoreAction StencilStore{};
        DepthStencilClearValue ClearValue{};
    };
    radray::vector<RGColorAttachmentDesc> ColorAttachments;
    std::optional<RGDepthStencilAttachmentDesc> DepthStencilAttachment;
    RGPassType Type;
};

struct RGTransientTextureRequest {
    uint32_t ResourceIndex;
    TextureDescriptor GpuDesc;
};

struct RGTransientBufferRequest {
    uint32_t ResourceIndex;
    BufferDescriptor GpuDesc;
};

// ============================================================
// Forward declarations
// ============================================================

class RenderGraph;
struct RGPassData;

// ============================================================
// RGCompiledGraph — output of Compile(), input to RGExecutor
// ============================================================

struct RGCompiledGraph {
    radray::vector<RGCompiledPass> Passes;
    radray::vector<RGTransientTextureRequest> TransientTextures;
    radray::vector<RGTransientBufferRequest> TransientBuffers;

    // Internal reference back to RenderGraph data (pass lambdas, resource info)
    // Lifetime must not exceed the RenderGraph that produced this
    RenderGraph* _graph{nullptr};
};

// ============================================================
// RGPassBuilder — used in pass setup lambdas
// ============================================================

class RGPassBuilder {
public:
    explicit RGPassBuilder(RGPassData& passData, RenderGraph& graph) noexcept;

    RGTextureHandle UseTexture(RGTextureHandle handle, RGAccessFlags access, RGResourceUsage usage);
    RGBufferHandle UseBuffer(RGBufferHandle handle, RGAccessFlags access, RGResourceUsage usage);

    void SetColorAttachment(uint32_t index, RGTextureHandle handle,
                            LoadAction load, StoreAction store,
                            ColorClearValue clearValue = {});
    void SetDepthStencilAttachment(RGTextureHandle handle,
                                   LoadAction depthLoad, StoreAction depthStore,
                                   LoadAction stencilLoad = LoadAction::DontCare,
                                   StoreAction stencilStore = StoreAction::Discard,
                                   DepthStencilClearValue clearValue = {});

    void SetFlags(RGPassFlags flags);

private:
    RGPassData& _passData;
    RenderGraph& _graph;
};

// ============================================================
// RGPassContext — execution context passed to pass exec lambdas
// ============================================================

class RGPassContext {
public:
    Texture* GetTexture(RGTextureHandle handle) const;
    Buffer* GetBuffer(RGBufferHandle handle) const;

    GraphicsCommandEncoder* GetGraphicsEncoder() const { return _graphicsEnc; }
    ComputeCommandEncoder* GetComputeEncoder() const { return _computeEnc; }
    RayTracingCommandEncoder* GetRayTracingEncoder() const { return _rtEnc; }
    CommandBuffer* GetCommandBuffer() const { return _cmdBuffer; }

    void SetGraphicsEncoder(GraphicsCommandEncoder* enc) { _graphicsEnc = enc; }
    void SetComputeEncoder(ComputeCommandEncoder* enc) { _computeEnc = enc; }
    void SetRayTracingEncoder(RayTracingCommandEncoder* enc) { _rtEnc = enc; }
    void SetCommandBuffer(CommandBuffer* buf) { _cmdBuffer = buf; }

    // Resource table setup (used by RGExecutor)
    void SetResourceTables(Texture** textures, uint32_t texCount,
                           Buffer** buffers, uint32_t bufCount) {
        _textures = textures;
        _textureCount = texCount;
        _buffers = buffers;
        _bufferCount = bufCount;
    }

private:
    Texture** _textures{nullptr};
    Buffer** _buffers{nullptr};
    uint32_t _textureCount{0};
    uint32_t _bufferCount{0};

    GraphicsCommandEncoder* _graphicsEnc{nullptr};
    ComputeCommandEncoder* _computeEnc{nullptr};
    RayTracingCommandEncoder* _rtEnc{nullptr};
    CommandBuffer* _cmdBuffer{nullptr};
};

// ============================================================
// RenderGraph — declarative pass/resource definition + Compile
// ============================================================

class RenderGraph {
public:
    explicit RenderGraph(Device* device);
    ~RenderGraph();

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&) = delete;
    RenderGraph& operator=(RenderGraph&&) = delete;

    // --- Define virtual resources ---
    RGTextureHandle CreateTexture(const RGTextureDescriptor& desc);
    RGBufferHandle CreateBuffer(const RGBufferDescriptor& desc);

    // --- Import external resources ---
    RGTextureHandle ImportTexture(Texture* texture, TextureStates currentState);
    RGBufferHandle ImportBuffer(Buffer* buffer, BufferStates currentState);
    RGTextureHandle ImportSwapChain(SwapChain* swapChain, TextureStates currentState);

    // --- Add pass ---
    template <typename PassData, typename SetupFunc, typename ExecFunc>
    PassData& AddPass(std::string_view name, RGPassType type,
                      SetupFunc&& setup, ExecFunc&& exec);

    // --- Compile ---
    RGCompiledGraph Compile();

    // Pass data accessor (used by template and executor)
    void* GetPassData(uint32_t passIndex);
    const void* GetPassData(uint32_t passIndex) const;

    // Exec lambda invoker (used by executor)
    void InvokePassExec(uint32_t passIndex, RGPassContext& ctx) const;

    struct Impl;

private:
    uint32_t AddPassInternal(std::string_view name, RGPassType type,
                             size_t passDataSize, size_t passDataAlign,
                             std::function<void(RGPassBuilder&, void*)> setupFn,
                             std::function<void(const void*, RGPassContext&)> execFn);

    unique_ptr<Impl> _impl;

    friend class RGPassBuilder;
};

// ============================================================
// Template implementation of AddPass
// ============================================================

template <typename PassData, typename SetupFunc, typename ExecFunc>
PassData& RenderGraph::AddPass(std::string_view name, RGPassType type,
                               SetupFunc&& setup, ExecFunc&& exec) {
    auto setupFn = [s = std::forward<SetupFunc>(setup)](RGPassBuilder& builder, void* data) {
        s(builder, *static_cast<PassData*>(data));
    };
    auto execFn = [e = std::forward<ExecFunc>(exec)](const void* data, RGPassContext& ctx) {
        e(*static_cast<const PassData*>(data), ctx);
    };
    uint32_t passIndex = AddPassInternal(
        name, type,
        sizeof(PassData), alignof(PassData),
        std::move(setupFn), std::move(execFn));
    return *static_cast<PassData*>(GetPassData(passIndex));
}

}  // namespace radray::render::rg
