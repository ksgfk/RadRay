#pragma once

#include <compare>
#include <functional>
#include <limits>
#include <string_view>
#include <utility>
#include <variant>

#include <radray/types.h>
#include <radray/enum_flags.h>
#include <radray/render/common.h>

namespace radray::render {

enum class RGResourceType : uint8_t {
    Unknown,
    Texture,
    Buffer,
    IndirectArgs
};

enum class RGResourceFlag : uint32_t {
    None = 0x0,
    Persistent = 0x1,
    External = Persistent << 1,
    Output = External << 1,
    ForceRetain = Output << 1,
    Temporal = ForceRetain << 1,
    PassLocal = Temporal << 1
};

enum class RGAccessMode : uint8_t {
    Unknown,
    SampledRead,
    StorageRead,
    StorageWrite,
    ColorAttachmentWrite,
    DepthStencilRead,
    DepthStencilWrite,
    CopySource,
    CopyDestination,
    IndirectRead
};

}  // namespace radray::render

namespace radray {

template <>
struct is_flags<render::RGResourceFlag> : public std::true_type {};

}  // namespace radray

namespace radray::render {

using RGResourceFlags = EnumFlags<RGResourceFlag>;

}

namespace radray::render {

struct RGResourceHandle {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};

    constexpr static RGResourceHandle Invalid() noexcept { return RGResourceHandle{std::numeric_limits<uint32_t>::max()}; }

    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }

    constexpr auto operator<=>(const RGResourceHandle& rhs) const noexcept = default;
};

struct RGTextureRange {
    static constexpr uint32_t All = std::numeric_limits<uint32_t>::max();

    uint32_t BaseMipLevel{0};
    uint32_t MipLevelCount{All};
    uint32_t BaseArrayLayer{0};
    uint32_t ArrayLayerCount{All};

    constexpr auto operator<=>(const RGTextureRange& rhs) const noexcept = default;
};

struct RGBufferRange {
    static constexpr uint64_t All = std::numeric_limits<uint64_t>::max();

    uint64_t Offset{0};
    uint64_t Size{All};

    constexpr auto operator<=>(const RGBufferRange& rhs) const noexcept = default;
};

using RGSubresourceRange = std::variant<RGTextureRange, RGBufferRange>;

struct RGTextureDescriptor {
    TextureDimension Dim{TextureDimension::Dim2D};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t DepthOrArraySize{1};
    uint32_t MipLevels{1};
    uint32_t SampleCount{1};
    TextureFormat Format{TextureFormat::UNKNOWN};
    RGResourceFlags Flags{RGResourceFlag::None};
    std::string_view Name{};
};

struct RGBufferDescriptor {
    uint64_t Size{0};
    RGResourceFlags Flags{RGResourceFlag::None};
    std::string_view Name{};
};

struct RGIndirectArgsDescriptor {
    RGBufferDescriptor Buffer{};
};

using RGResourceDescriptor = std::variant<RGTextureDescriptor, RGBufferDescriptor, RGIndirectArgsDescriptor>;

struct VirtualResource {
    static constexpr uint32_t InvalidPassIndex = std::numeric_limits<uint32_t>::max();

    string Name{};
    RGResourceDescriptor Descriptor{RGTextureDescriptor{}};
    RGResourceFlags Flags{RGResourceFlag::None};
    RGAccessMode InitialMode{RGAccessMode::Unknown};
    uint32_t ScopePassId{InvalidPassIndex};

    constexpr RGResourceType GetType() const noexcept {
        return std::visit(
            [](const auto& desc) -> RGResourceType {
                using T = std::decay_t<decltype(desc)>;
                if constexpr (std::is_same_v<T, RGTextureDescriptor>) {
                    return RGResourceType::Texture;
                } else if constexpr (std::is_same_v<T, RGBufferDescriptor>) {
                    return RGResourceType::Buffer;
                } else if constexpr (std::is_same_v<T, RGIndirectArgsDescriptor>) {
                    return RGResourceType::IndirectArgs;
                } else {
                    return RGResourceType::Unknown;
                }
            },
            Descriptor);
    }

    constexpr bool IsPassLocal() const noexcept {
        return Flags.HasFlag(RGResourceFlag::PassLocal);
    }
};

struct ResourceAccessEdge {
    RGResourceHandle Handle{};
    RGAccessMode Mode{RGAccessMode::Unknown};
    RGSubresourceRange Range{RGTextureRange{}};
};

class PipelineLayout;

struct RGParameterBinding {
    string Name{};
    RGResourceHandle Handle{};
    RGSubresourceRange Range{RGTextureRange{}};
};

struct RGPassContext;
using RGPassExecuteFunc = std::function<void(RGPassContext&)>;

struct RGPassNode {
    uint32_t Id{0};
    string Name{};
    QueueType QueueClass{QueueType::Direct};
    vector<ResourceAccessEdge> Reads{};
    vector<ResourceAccessEdge> Writes{};
    const PipelineLayout* Layout{nullptr};
    vector<RGParameterBinding> ParameterBindings{};
    RGPassExecuteFunc ExecuteFunc{};
    bool IsAlive{false};
};

struct RGSubresourceLifetime {
    static constexpr uint32_t InvalidPassIndex = std::numeric_limits<uint32_t>::max();

    RGResourceHandle Handle{};
    RGSubresourceRange Range{RGTextureRange{}};
    uint32_t FirstPass{InvalidPassIndex};
    uint32_t LastPass{InvalidPassIndex};
    bool IsExternal{false};

    constexpr bool IsValid() const noexcept {
        return FirstPass != InvalidPassIndex && LastPass != InvalidPassIndex;
    }
};

struct RGBarrier {
    RGResourceHandle Handle{};
    RGSubresourceRange Range{RGTextureRange{}};
    RGAccessMode StateBefore{RGAccessMode::Unknown};
    RGAccessMode StateAfter{RGAccessMode::Unknown};
};

struct RGResourceAllocation {
};

struct CompiledGraph {
    vector<uint32_t> SortedPasses{};
    vector<uint32_t> CulledPasses{};
    vector<RGSubresourceLifetime> SubresourceLifetimes{};
    vector<std::pair<uint32_t, uint32_t>> DependencyEdges{};
    vector<vector<RGBarrier>> PassBarriers{};
    vector<RGResourceAllocation> Allocations{};
    string ErrorMessage{};
    bool Success{false};
};

class RGGraphBuilder;

class RGPassBuilder {
public:
    RGPassBuilder& ReadTexture(
        RGResourceHandle handle,
        const RGTextureRange& range = {},
        RGAccessMode mode = RGAccessMode::SampledRead);

    RGPassBuilder& ReadBuffer(
        RGResourceHandle handle,
        const RGBufferRange& range = {},
        RGAccessMode mode = RGAccessMode::SampledRead);

    RGResourceHandle WriteTexture(
        RGResourceHandle handle,
        const RGTextureRange& range = {},
        RGAccessMode mode = RGAccessMode::StorageWrite);

    RGResourceHandle WriteBuffer(
        RGResourceHandle handle,
        const RGBufferRange& range = {},
        RGAccessMode mode = RGAccessMode::StorageWrite);

    RGResourceHandle ReadWriteTexture(
        RGResourceHandle handle,
        const RGTextureRange& range = {},
        RGAccessMode readMode = RGAccessMode::StorageRead,
        RGAccessMode writeMode = RGAccessMode::StorageWrite);

    RGResourceHandle ReadWriteBuffer(
        RGResourceHandle handle,
        const RGBufferRange& range = {},
        RGAccessMode readMode = RGAccessMode::StorageRead,
        RGAccessMode writeMode = RGAccessMode::StorageWrite);

    RGPassBuilder& SetPipelineLayout(const PipelineLayout* layout);

    RGPassBuilder& SetResource(std::string_view name, RGResourceHandle handle);

    RGPassBuilder& SetResource(std::string_view name, RGResourceHandle handle, uint32_t mip, uint32_t slice);

    RGResourceHandle CreateLocalTexture(const RGTextureDescriptor& desc);

    RGResourceHandle CreateLocalBuffer(const RGBufferDescriptor& desc);

    RGPassBuilder& SetExecuteFunc(RGPassExecuteFunc func);

private:
    RGPassBuilder(RGGraphBuilder* graph, uint32_t passIndex) noexcept;

    RGGraphBuilder* _graph{nullptr};
    uint32_t _passIndex{std::numeric_limits<uint32_t>::max()};

    friend class RGGraphBuilder;
};

class RGGraphBuilder {
public:
    RGResourceHandle CreateTexture(const RGTextureDescriptor& desc);

    RGResourceHandle CreateBuffer(const RGBufferDescriptor& desc);

    RGResourceHandle CreateIndirectArgs(const RGBufferDescriptor& desc);

    RGResourceHandle ImportExternalTexture(
        std::string_view name,
        RGResourceFlags flags = RGResourceFlag::External | RGResourceFlag::Persistent,
        RGAccessMode initialMode = RGAccessMode::Unknown);

    RGResourceHandle ImportExternalBuffer(
        std::string_view name,
        RGResourceFlags flags = RGResourceFlag::External | RGResourceFlag::Persistent,
        RGAccessMode initialMode = RGAccessMode::Unknown);

    RGPassBuilder AddPass(std::string_view name, QueueType queueClass = QueueType::Direct);

    void MarkOutput(RGResourceHandle handle);
    void MarkOutput(RGResourceHandle handle, const RGTextureRange& range);
    void MarkOutput(RGResourceHandle handle, const RGBufferRange& range);

    void ForceRetain(RGResourceHandle handle);
    void ForceRetain(RGResourceHandle handle, const RGTextureRange& range);
    void ForceRetain(RGResourceHandle handle, const RGBufferRange& range);

    void ExportTemporal(RGResourceHandle handle);
    void ExportTemporal(RGResourceHandle handle, const RGTextureRange& range);
    void ExportTemporal(RGResourceHandle handle, const RGBufferRange& range);

    CompiledGraph Compile() const;

    string DumpRecording() const;

    string DumpCompiledGraph(const CompiledGraph& compiled) const;

    const vector<VirtualResource>& GetResources() const noexcept { return _resources; }

    const vector<RGPassNode>& GetPasses() const noexcept { return _passes; }

private:
    friend class RGPassBuilder;

    bool ValidateHandleIndex(RGResourceHandle handle) const noexcept;
    bool ValidatePassLocalAccess(uint32_t passIndex, RGResourceHandle handle, std::string_view operation) const noexcept;
    void ResolveReflectionDependencies();

    RGResourceHandle AddReadTextureEdge(uint32_t passIndex, RGResourceHandle handle, const RGTextureRange& range, RGAccessMode mode);
    RGResourceHandle AddReadBufferEdge(uint32_t passIndex, RGResourceHandle handle, const RGBufferRange& range, RGAccessMode mode);
    RGResourceHandle AddWriteTextureEdge(uint32_t passIndex, RGResourceHandle handle, const RGTextureRange& range, RGAccessMode mode);
    RGResourceHandle AddWriteBufferEdge(uint32_t passIndex, RGResourceHandle handle, const RGBufferRange& range, RGAccessMode mode);
    RGResourceHandle CreatePassLocalTexture(uint32_t passIndex, const RGTextureDescriptor& desc);
    RGResourceHandle CreatePassLocalBuffer(uint32_t passIndex, const RGBufferDescriptor& desc);

    void AddRootEdge(const ResourceAccessEdge& edge);
    ResourceAccessEdge MakeFullRangeEdge(RGResourceHandle handle) const;

private:
    vector<VirtualResource> _resources{};
    vector<RGPassNode> _passes{};
    vector<ResourceAccessEdge> _roots{};
};

constexpr bool RGIsReadAccess(RGAccessMode mode) noexcept {
    switch (mode) {
        case RGAccessMode::SampledRead:
        case RGAccessMode::StorageRead:
        case RGAccessMode::DepthStencilRead:
        case RGAccessMode::CopySource:
        case RGAccessMode::IndirectRead:
            return true;
        default:
            return false;
    }
}

constexpr bool RGIsWriteAccess(RGAccessMode mode) noexcept {
    switch (mode) {
        case RGAccessMode::StorageWrite:
        case RGAccessMode::ColorAttachmentWrite:
        case RGAccessMode::DepthStencilWrite:
        case RGAccessMode::CopyDestination:
            return true;
        default:
            return false;
    }
}

std::string_view format_as(RGResourceType v) noexcept;
std::string_view format_as(RGResourceFlag v) noexcept;
std::string_view format_as(RGAccessMode v) noexcept;

}  // namespace radray::render
