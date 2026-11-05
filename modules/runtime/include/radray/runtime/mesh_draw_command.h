#pragma once

#include <optional>
#include <span>

#include <radray/inline_vector.h>
#include <radray/sparse_set.h>
#include <radray/render/rhi.h>
#include <radray/runtime/vertex_input.h>

namespace radray {

enum class MeshDrawCommandSource : uint8_t {
    Cached,
    Dynamic
};

struct MeshPassId {
    uint32_t Value{0};

    friend bool operator==(MeshPassId, MeshPassId) = default;
};

struct MeshShaderParameterBinding {
    uint32_t GroupIndex{0};
    Nullable<render::ShaderParameterSet*> ParameterSet{nullptr};
    InlineVector<render::ShaderParameterDynamicOffset, 2> DynamicOffsets;
};

struct MeshPushConstantBinding {
    uint32_t PushConstantId{0};
    uint32_t Offset{0};
    // Borrowed only during command construction; the command copies these bytes.
    std::span<const byte> Data{};
};

struct MeshPushConstantCommandBinding {
    uint32_t PushConstantId{0};
    uint32_t Offset{0};
    InlineVector<byte, 64> Data;
};

// See docs/architecture/runtime.md for command construction and resource lifetimes.
// Binding descriptions are owned; RHI objects are borrowed through command allocator reset.
struct MeshDrawCommand {
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    InlineVector<render::VertexBufferBinding, 4> VertexBuffers;
    std::optional<uint32_t> InstanceStreamBinding{};
    render::IndexBufferView IndexBuffer{};
    InlineVector<MeshShaderParameterBinding, 2> ParameterSets;
    std::optional<MeshPushConstantCommandBinding> PushConstants{};
    uint64_t BaseSortKey{0};
    uint32_t IndexCount{0};
    uint32_t FirstIndex{0};
    int32_t VertexOffset{0};
    uint32_t FirstInstance{0};
    uint32_t InstanceCount{1};
    uint8_t StencilReference{0};
};

struct CachedMeshDrawCommandInfo {
    MeshPassId Pass{};
    uint32_t ElementIndex{0};
    SparseSetHandle CommandHandle{};
    uint64_t BaseSortKey{0};
    uint32_t FirstInstance{0};
    uint32_t InstanceCount{1};
};

struct CachedPassMeshDrawList {
    SparseSet<MeshDrawCommand> Commands;
};

struct VisibleMeshDrawCommand {
    MeshDrawCommandSource Source{MeshDrawCommandSource::Cached};
    SparseSetHandle CommandHandle{};
    uint64_t SortKey{0};
    uint32_t FirstInstance{0};
    uint32_t InstanceCount{1};
};

static_assert(sizeof(VisibleMeshDrawCommand) == 32);

struct MeshPassSubmitArgs {
    std::span<const MeshShaderParameterBinding> ParameterSets{};
    // May fill or replace only the slot declared by MeshDrawCommand::InstanceStreamBinding.
    std::optional<render::VertexBufferBinding> InstanceStream{};
};

bool ValidateVertexInput(const VertexInput& input, bool allowDeferredInstanceStream = false) noexcept;
bool ValidateMeshDrawCommand(const MeshDrawCommand& command, const VisibleMeshDrawCommand& visibleCommand = {},
                             const MeshPassSubmitArgs& submitArgs = {}) noexcept;

// Dynamic handles use their frame-local array index and generation zero.
Nullable<const MeshDrawCommand*> ResolveVisibleMeshDrawCommand(
    const VisibleMeshDrawCommand& visibleCommand, Nullable<const CachedPassMeshDrawList*> cachedDrawList,
    std::span<const MeshDrawCommand> dynamicCommands = {}) noexcept;

// Snapshot cached handles, like dynamic handles, use frame-local indices and generation zero.
Nullable<const MeshDrawCommand*> ResolveVisibleMeshDrawCommand(
    const VisibleMeshDrawCommand& visibleCommand, std::span<const MeshDrawCommand> cachedCommands,
    std::span<const MeshDrawCommand> dynamicCommands = {}) noexcept;

// Debug preflights the whole list; Release trusts validated commands and submit arguments.
// State is deduplicated within this call. On failure, discard the recording (it may be partial).
// Pass boundaries, viewport/scissor and submission belong to the caller.
bool SubmitMeshDrawCommands(render::GraphicsCommandEncoder& encoder,
                            std::span<const VisibleMeshDrawCommand> visibleCommands,
                            Nullable<const CachedPassMeshDrawList*> cachedDrawList,
                            std::span<const MeshDrawCommand> dynamicCommands = {},
                            const MeshPassSubmitArgs& submitArgs = {}) noexcept;

bool SubmitMeshDrawCommands(render::GraphicsCommandEncoder& encoder,
                            std::span<const VisibleMeshDrawCommand> visibleCommands,
                            std::span<const MeshDrawCommand> cachedCommands,
                            std::span<const MeshDrawCommand> dynamicCommands = {},
                            const MeshPassSubmitArgs& submitArgs = {}) noexcept;

}  // namespace radray
