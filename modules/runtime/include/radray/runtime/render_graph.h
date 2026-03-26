#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

#include <radray/enum_flags.h>
#include <radray/types.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

class RenderGraph;
class RGCompiledGraph;
class RGPassContext;
class RGGraphicsPassContext;
class RGComputePassContext;
class RGRayTracingPassContext;
class RGGraphicsPassBuilder;
class RGComputePassBuilder;
class RGCopyPassBuilder;
class RGRayTracingPassBuilder;

enum class RGResourceKind : uint8_t {
    Buffer,
    Texture,
    AccelerationStructure
};

enum class RGPassKind : uint8_t {
    Graphics,
    Compute,
    Copy,
    RayTracing
};

enum class RGUseKind : uint8_t {
    VertexBufferRead,
    IndexBufferRead,
    IndirectBufferRead,
    ConstantBufferRead,
    StorageBufferRead,
    StorageBufferReadWrite,
    SampledTextureRead,
    StorageTextureRead,
    StorageTextureReadWrite,
    ColorAttachmentWrite,
    DepthStencilAttachmentWrite,
    AccelerationStructureRead,
    AccelerationStructureBuild,
    BuildScratchBuffer,
    ShaderTableRead,
    CopyBufferRead,
    CopyBufferWrite,
    CopyTextureRead,
    CopyTextureWrite
};

class RenderGraphException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct RGNodeId {
    uint32_t Value{std::numeric_limits<uint32_t>::max()};

    constexpr bool IsValid() const noexcept {
        return Value != std::numeric_limits<uint32_t>::max();
    }

    constexpr operator uint32_t() const noexcept { return Value; }

    constexpr static RGNodeId Invalid() noexcept {
        return {};
    }

    friend auto operator<=>(const RGNodeId& lhs, const RGNodeId& rhs) noexcept = default;
};

struct RGResourceHandle {
    RGNodeId Node{};

    constexpr bool IsValid() const noexcept { return Node.IsValid(); }
    constexpr operator RGNodeId() const noexcept { return Node; }

    constexpr static RGResourceHandle Invalid() noexcept {
        return {};
    }

    friend auto operator<=>(const RGResourceHandle& lhs, const RGResourceHandle& rhs) noexcept = default;
};

struct RGPassHandle {
    RGNodeId Node{};

    constexpr bool IsValid() const noexcept { return Node.IsValid(); }
    constexpr operator RGNodeId() const noexcept { return Node; }

    constexpr static RGPassHandle Invalid() noexcept {
        return {};
    }

    friend auto operator<=>(const RGPassHandle& lhs, const RGPassHandle& rhs) noexcept = default;
};

enum class RGPassFlag : uint32_t {
    None = 0x0,
    SideEffect = 0x1,
    NoCull = SideEffect << 1
};

template <>
struct is_flags<RGPassFlag> : public std::true_type {};

using RGPassFlags = EnumFlags<RGPassFlag>;

constexpr render::BufferRange RGWholeBufferRange() noexcept {
    return {0, std::numeric_limits<uint64_t>::max()};
}

constexpr render::SubresourceRange RGWholeSubresourceRange() noexcept {
    return render::SubresourceRange::AllSub();
}

struct RGBufferDesc {
    uint64_t Size{0};
    render::MemoryType Memory{render::MemoryType::Device};
    render::ResourceHints Hints{render::ResourceHint::None};
};

struct RGTextureDesc {
    render::TextureDimension Dim{render::TextureDimension::UNKNOWN};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t DepthOrArraySize{0};
    uint32_t MipLevels{0};
    uint32_t SampleCount{0};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
    render::MemoryType Memory{render::MemoryType::Device};
    render::ResourceHints Hints{render::ResourceHint::None};
};

struct RGAccelerationStructureDesc {
    render::AccelerationStructureType Type{render::AccelerationStructureType::BottomLevel};
    uint32_t MaxGeometryCount{0};
    uint32_t MaxInstanceCount{0};
    render::AccelerationStructureBuildFlags Flags{render::AccelerationStructureBuildFlag::None};
};

struct RGExternalBufferState {
    render::QueueType Queue{render::QueueType::Direct};
    render::BufferStates State{render::BufferState::Common};
    render::BufferRange Range{RGWholeBufferRange()};
};

struct RGExternalTextureState {
    render::QueueType Queue{render::QueueType::Direct};
    render::TextureStates State{render::TextureState::Common};
    render::SubresourceRange Range{RGWholeSubresourceRange()};
};

struct RGExternalAccelerationStructureState {
    render::QueueType Queue{render::QueueType::Direct};
    render::BufferStates State{render::BufferState::AccelerationStructureRead};
};

struct RGImportedBufferDesc {
    GpuResourceHandle External{};
    RGBufferDesc Desc{};
    RGExternalBufferState InitialState{};
};

struct RGImportedTextureDesc {
    GpuResourceHandle External{};
    RGTextureDesc Desc{};
    RGExternalTextureState InitialState{};
};

struct RGImportedAccelerationStructureDesc {
    GpuResourceHandle External{};
    RGAccelerationStructureDesc Desc{};
    RGExternalAccelerationStructureState InitialState{};
};

struct RGResourceOptions {
    bool AllowTransientAliasing{true};
};

struct RGBufferShaderAccessDesc {
    render::BufferRange Range{RGWholeBufferRange()};
    render::ShaderStages ShaderStages{render::ShaderStage::UNKNOWN};
};

struct RGTextureShaderAccessDesc {
    render::SubresourceRange Range{RGWholeSubresourceRange()};
    render::ShaderStages ShaderStages{render::ShaderStage::UNKNOWN};
};

struct RGTextureViewDesc {
    render::TextureDimension Dim{render::TextureDimension::UNKNOWN};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
    render::SubresourceRange Range{RGWholeSubresourceRange()};
    render::TextureViewUsages Usage{render::TextureViewUsage::UNKNOWN};
};

struct RGColorAttachmentInfo {
    render::LoadAction Load{render::LoadAction::Clear};
    render::StoreAction Store{render::StoreAction::Store};
    render::ColorClearValue ClearValue{};
};

struct RGDepthStencilAttachmentInfo {
    render::LoadAction DepthLoad{render::LoadAction::Clear};
    render::StoreAction DepthStore{render::StoreAction::Store};
    render::LoadAction StencilLoad{render::LoadAction::DontCare};
    render::StoreAction StencilStore{render::StoreAction::Discard};
    render::DepthStencilClearValue ClearValue{1.0f, 0};
};

struct RGColorAttachmentDesc {
    uint32_t Slot{0};
    render::SubresourceRange Range{RGWholeSubresourceRange()};
    RGColorAttachmentInfo Attachment{};
};

struct RGDepthStencilAttachmentDesc {
    render::SubresourceRange Range{RGWholeSubresourceRange()};
    RGDepthStencilAttachmentInfo Attachment{};
    bool ReadOnlyDepth{false};
    bool ReadOnlyStencil{false};
};

struct RGAccelerationStructureBuildDesc {
    RGResourceHandle ScratchBuffer{};
    render::BufferRange ScratchRange{RGWholeBufferRange()};
    render::AccelerationStructureBuildMode Mode{render::AccelerationStructureBuildMode::Build};
};

using RGResourceDescData = std::variant<RGBufferDesc, RGTextureDesc, RGAccelerationStructureDesc>;
using RGExternalStateData = std::variant<std::monostate, RGExternalBufferState, RGExternalTextureState, RGExternalAccelerationStructureState>;
using RGExportStateData = std::variant<RGExternalBufferState, RGExternalTextureState, RGExternalAccelerationStructureState>;
using RGUseData = std::variant<
    std::monostate,
    render::BufferRange,
    render::SubresourceRange,
    RGBufferShaderAccessDesc,
    RGTextureShaderAccessDesc,
    RGColorAttachmentDesc,
    RGDepthStencilAttachmentDesc,
    RGAccelerationStructureBuildDesc>;

struct RGResourceRecord {
    RGResourceHandle Handle{};
    string Name{};
    RGResourceKind Kind{RGResourceKind::Buffer};
    bool Imported{false};
    GpuResourceHandle External{};
    RGResourceOptions Options{};
    RGResourceDescData Desc{};
    RGExternalStateData InitialState{};
};

struct RGPassRecord {
    RGPassHandle Handle{};
    string Name{};
    RGPassKind Kind{RGPassKind::Graphics};
    render::QueueType Queue{render::QueueType::Direct};
    RGPassFlags Flags{};
};

struct RGUseRecord {
    RGPassHandle Pass{};
    RGResourceHandle Resource{};
    RGUseKind Kind{RGUseKind::VertexBufferRead};
    string Name{};
    RGUseData Data{};
};

struct RGExportRecord {
    RGResourceHandle Resource{};
    RGResourceKind ExpectedKind{RGResourceKind::Buffer};
    RGExportStateData State{};
};

using RGGraphicsPassExecuteCallback = std::function<void(RGGraphicsPassContext&)>;
using RGComputePassExecuteCallback = std::function<void(RGComputePassContext&)>;
using RGRayTracingPassExecuteCallback = std::function<void(RGRayTracingPassContext&)>;

struct RGPassOptionsBase {
    RGPassFlags Flags{};
};

struct RGGraphicsPassOptions : RGPassOptionsBase {
    RGGraphicsPassExecuteCallback Execute{};
};

struct RGComputePassOptions : RGPassOptionsBase {
    render::QueueType Queue{render::QueueType::Compute};
    RGComputePassExecuteCallback Execute{};
};

struct RGCopyPassOptions : RGPassOptionsBase {
    render::QueueType Queue{render::QueueType::Copy};
};

struct RGRayTracingPassOptions : RGPassOptionsBase {
    RGRayTracingPassExecuteCallback Execute{};
};

enum class RGValidationSeverity : uint8_t {
    Warning,
    Error
};

enum class RGValidationIssueCode : uint8_t {
    MissingResource,
    MissingPass,
    MissingImportedExternalHandle,
    DuplicateExport,
    InvalidResourceUseForKind,
    InvalidPassUseForKind,
    InvalidExportForKind,
    InvalidBuildScratchBuffer
};

struct RGValidationIssue {
    RGValidationSeverity Severity{RGValidationSeverity::Error};
    RGValidationIssueCode Code{RGValidationIssueCode::MissingResource};
    string Message{};
    std::optional<RGResourceHandle> Resource{};
    std::optional<RGPassHandle> Pass{};
};

struct RGValidationResult {
    vector<RGValidationIssue> Issues{};

    bool IsValid() const noexcept;
};

struct RGExecutionEnvironment {
    render::Device* Device{nullptr};
    render::CommandBuffer* CommandBuffer{nullptr};
    Nullable<GpuFrameContext*> FrameContext{nullptr};
};

class RGCompiledGraph {};

class RGPassContext {
public:
    RenderGraph& GetGraph() const noexcept;
    const RGCompiledGraph& GetCompiledGraph() const noexcept;
    RGPassHandle GetPass() const noexcept;
    const RGExecutionEnvironment& GetEnvironment() const noexcept;
    render::Device& GetDevice() const noexcept;
    render::CommandBuffer& GetCommandBuffer() const noexcept;
    Nullable<GpuFrameContext*> GetFrameContext() const noexcept;

    render::Buffer* ResolveBuffer(RGResourceHandle buffer) const noexcept;
    render::Texture* ResolveTexture(RGResourceHandle texture) const noexcept;
    render::TextureView* ResolveTextureView(
        RGResourceHandle texture,
        const RGTextureViewDesc& desc) const noexcept;
    render::AccelerationStructure* ResolveAccelerationStructure(
        RGResourceHandle accelerationStructure) const noexcept;
    render::AccelerationStructureView* ResolveAccelerationStructureView(
        RGResourceHandle accelerationStructure) const noexcept;

protected:
    RGPassContext(
        RenderGraph* graph,
        const RGCompiledGraph* compiledGraph,
        const RGExecutionEnvironment* environment,
        RGPassHandle pass) noexcept;

    RenderGraph* _graph{nullptr};
    const RGCompiledGraph* _compiledGraph{nullptr};
    const RGExecutionEnvironment* _environment{nullptr};
    RGPassHandle _pass{};

    friend class RenderGraph;
};

class RGGraphicsPassContext : public RGPassContext {
public:
    render::GraphicsCommandEncoder& GetEncoder() const noexcept;

private:
    RGGraphicsPassContext(
        RenderGraph* graph,
        const RGCompiledGraph* compiledGraph,
        const RGExecutionEnvironment* environment,
        RGPassHandle pass,
        render::GraphicsCommandEncoder* encoder) noexcept;

    render::GraphicsCommandEncoder* _encoder{nullptr};

    friend class RenderGraph;
};

class RGComputePassContext : public RGPassContext {
public:
    render::ComputeCommandEncoder& GetEncoder() const noexcept;

private:
    RGComputePassContext(
        RenderGraph* graph,
        const RGCompiledGraph* compiledGraph,
        const RGExecutionEnvironment* environment,
        RGPassHandle pass,
        render::ComputeCommandEncoder* encoder) noexcept;

    render::ComputeCommandEncoder* _encoder{nullptr};

    friend class RenderGraph;
};

class RGRayTracingPassContext : public RGPassContext {
public:
    render::RayTracingCommandEncoder& GetEncoder() const noexcept;

private:
    RGRayTracingPassContext(
        RenderGraph* graph,
        const RGCompiledGraph* compiledGraph,
        const RGExecutionEnvironment* environment,
        RGPassHandle pass,
        render::RayTracingCommandEncoder* encoder) noexcept;

    render::RayTracingCommandEncoder* _encoder{nullptr};

    friend class RenderGraph;
};

class RGGraphicsPassBuilder {
public:
    RGPassHandle GetHandle() const noexcept { return _pass; }

    RGGraphicsPassBuilder& ReadVertexBuffer(
        RGResourceHandle buffer,
        render::BufferRange range = RGWholeBufferRange(),
        std::string_view name = {});

    RGGraphicsPassBuilder& ReadIndexBuffer(
        RGResourceHandle buffer,
        render::BufferRange range = RGWholeBufferRange(),
        std::string_view name = {});

    RGGraphicsPassBuilder& ReadIndirectBuffer(
        RGResourceHandle buffer,
        render::BufferRange range = RGWholeBufferRange(),
        std::string_view name = {});

    RGGraphicsPassBuilder& ReadConstantBuffer(
        RGResourceHandle buffer,
        const RGBufferShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGGraphicsPassBuilder& ReadStorageBuffer(
        RGResourceHandle buffer,
        const RGBufferShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGGraphicsPassBuilder& ReadWriteStorageBuffer(
        RGResourceHandle buffer,
        const RGBufferShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGGraphicsPassBuilder& ReadSampledTexture(
        RGResourceHandle texture,
        const RGTextureShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGGraphicsPassBuilder& ReadStorageTexture(
        RGResourceHandle texture,
        const RGTextureShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGGraphicsPassBuilder& ReadWriteStorageTexture(
        RGResourceHandle texture,
        const RGTextureShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGGraphicsPassBuilder& BindColorAttachment(
        RGResourceHandle texture,
        const RGColorAttachmentDesc& desc = {},
        std::string_view name = {});

    RGGraphicsPassBuilder& BindDepthStencilAttachment(
        RGResourceHandle texture,
        const RGDepthStencilAttachmentDesc& desc = {},
        std::string_view name = {});

private:
    RGGraphicsPassBuilder(RenderGraph* graph, RGPassHandle pass) noexcept;

    RenderGraph* _graph{nullptr};
    RGPassHandle _pass{};

    friend class RenderGraph;
};

class RGComputePassBuilder {
public:
    RGPassHandle GetHandle() const noexcept { return _pass; }

    RGComputePassBuilder& ReadConstantBuffer(
        RGResourceHandle buffer,
        const RGBufferShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGComputePassBuilder& ReadStorageBuffer(
        RGResourceHandle buffer,
        const RGBufferShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGComputePassBuilder& ReadWriteStorageBuffer(
        RGResourceHandle buffer,
        const RGBufferShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGComputePassBuilder& ReadSampledTexture(
        RGResourceHandle texture,
        const RGTextureShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGComputePassBuilder& ReadStorageTexture(
        RGResourceHandle texture,
        const RGTextureShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGComputePassBuilder& ReadWriteStorageTexture(
        RGResourceHandle texture,
        const RGTextureShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGComputePassBuilder& ReadAccelerationStructure(
        RGResourceHandle accelerationStructure,
        std::string_view name = {});

private:
    RGComputePassBuilder(RenderGraph* graph, RGPassHandle pass) noexcept;

    RenderGraph* _graph{nullptr};
    RGPassHandle _pass{};

    friend class RenderGraph;
};

class RGRayTracingPassBuilder {
public:
    RGPassHandle GetHandle() const noexcept { return _pass; }

    RGRayTracingPassBuilder& ReadShaderTableBuffer(
        RGResourceHandle buffer,
        render::BufferRange range = RGWholeBufferRange(),
        std::string_view name = {});

    RGRayTracingPassBuilder& ReadConstantBuffer(
        RGResourceHandle buffer,
        const RGBufferShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGRayTracingPassBuilder& ReadStorageBuffer(
        RGResourceHandle buffer,
        const RGBufferShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGRayTracingPassBuilder& ReadWriteStorageBuffer(
        RGResourceHandle buffer,
        const RGBufferShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGRayTracingPassBuilder& ReadSampledTexture(
        RGResourceHandle texture,
        const RGTextureShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGRayTracingPassBuilder& ReadStorageTexture(
        RGResourceHandle texture,
        const RGTextureShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGRayTracingPassBuilder& ReadWriteStorageTexture(
        RGResourceHandle texture,
        const RGTextureShaderAccessDesc& desc = {},
        std::string_view name = {});

    RGRayTracingPassBuilder& ReadAccelerationStructure(
        RGResourceHandle accelerationStructure,
        std::string_view name = {});

    RGRayTracingPassBuilder& BuildAccelerationStructure(
        RGResourceHandle accelerationStructure,
        const RGAccelerationStructureBuildDesc& desc,
        std::string_view name = {});

private:
    RGRayTracingPassBuilder(RenderGraph* graph, RGPassHandle pass) noexcept;

    RenderGraph* _graph{nullptr};
    RGPassHandle _pass{};

    friend class RenderGraph;
};

class RGCopyPassBuilder {
public:
    RGPassHandle GetHandle() const noexcept { return _pass; }

    RGCopyPassBuilder& Copy(
        RGResourceHandle src,
        render::BufferRange srcRange,
        RGResourceHandle dst,
        render::BufferRange dstRange = RGWholeBufferRange(),
        std::string_view name = {});

    RGCopyPassBuilder& CopyBufferToTexture(
        RGResourceHandle src,
        render::BufferRange srcRange,
        RGResourceHandle dst,
        render::SubresourceRange dstRange = RGWholeSubresourceRange(),
        std::string_view name = {});

    RGCopyPassBuilder& CopyTextureToBuffer(
        RGResourceHandle src,
        render::SubresourceRange srcRange,
        RGResourceHandle dst,
        render::BufferRange dstRange = RGWholeBufferRange(),
        std::string_view name = {});

private:
    RGCopyPassBuilder(RenderGraph* graph, RGPassHandle pass) noexcept;

    RenderGraph* _graph{nullptr};
    RGPassHandle _pass{};

    friend class RenderGraph;
};

class RenderGraph {
public:
    RenderGraph();
    ~RenderGraph();
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&) noexcept;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph& operator=(RenderGraph&&) noexcept;

    RGResourceHandle AddBuffer(
        std::string_view name,
        const RGBufferDesc& desc,
        RGResourceOptions options = {});

    RGResourceHandle AddTexture(
        std::string_view name,
        const RGTextureDesc& desc,
        RGResourceOptions options = {});

    RGResourceHandle AddAccelerationStructure(
        std::string_view name,
        const RGAccelerationStructureDesc& desc,
        RGResourceOptions options = {});

    RGResourceHandle ImportBuffer(
        std::string_view name,
        const RGImportedBufferDesc& desc,
        RGResourceOptions options = {});

    RGResourceHandle ImportTexture(
        std::string_view name,
        const RGImportedTextureDesc& desc,
        RGResourceOptions options = {});

    RGResourceHandle ImportAccelerationStructure(
        std::string_view name,
        const RGImportedAccelerationStructureDesc& desc,
        RGResourceOptions options = {});

    void ExportBuffer(
        RGResourceHandle buffer,
        const RGExternalBufferState& finalState);

    void ExportTexture(
        RGResourceHandle texture,
        const RGExternalTextureState& finalState);

    void ExportAccelerationStructure(
        RGResourceHandle accelerationStructure,
        const RGExternalAccelerationStructureState& finalState);

    RGGraphicsPassBuilder AddGraphicsPass(
        std::string_view name,
        RGGraphicsPassOptions options = {});

    RGComputePassBuilder AddComputePass(
        std::string_view name,
        RGComputePassOptions options = {});

    RGCopyPassBuilder AddCopyPass(
        std::string_view name,
        RGCopyPassOptions options = {});

    RGRayTracingPassBuilder AddRayTracingPass(
        std::string_view name,
        RGRayTracingPassOptions options = {});

    RGValidationResult Validate() const;
    string ToGraphviz() const;

    RGCompiledGraph Compile() const;
    void Execute(
        const RGCompiledGraph& compiledGraph,
        const RGExecutionEnvironment& environment);

private:
    void AppendUseRecord(
        RGPassHandle pass,
        RGResourceHandle resource,
        RGUseKind kind,
        string name,
        RGUseData data);

    const RGResourceRecord* FindResource(RGResourceHandle handle) const noexcept;
    const RGPassRecord* FindPass(RGPassHandle handle) const noexcept;

    vector<RGResourceRecord> _resources{};
    vector<RGPassRecord> _passes{};
    vector<RGUseRecord> _uses{};
    vector<RGExportRecord> _exports{};
    uint32_t _nextResourceId{0};
    uint32_t _nextPassId{0};

    friend class RGGraphicsPassBuilder;
    friend class RGComputePassBuilder;
    friend class RGCopyPassBuilder;
    friend class RGRayTracingPassBuilder;
};

}  // namespace radray
