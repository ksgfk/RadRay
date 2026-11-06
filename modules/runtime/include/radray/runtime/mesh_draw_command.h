#pragma once

#include <limits>
#include <optional>
#include <span>
#include <type_traits>

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

// See docs/architecture/runtime.md for construction and resource lifetimes.
class MeshDrawCommandBindingStorage;

struct MeshDrawElement {
    render::IndexBufferView IndexBuffer{};
    uint32_t IndexCount{0};
    uint32_t FirstIndex{0};
    int32_t VertexOffset{0};
    uint32_t ElementIndex{0};
};

static_assert(sizeof(MeshDrawElement) == 32 && std::is_trivially_copyable_v<MeshDrawElement>);

struct MeshDrawElementInstances {
    uint32_t FirstInstance{0};
    uint32_t InstanceCount{1};
};

// Copies borrow the payload; only the owning list may destroy it.
struct MeshDrawCommand {
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    render::IndexBufferView IndexBuffer{};
    SparseSetHandle VertexBuffersHandle{};
    SparseSetHandle PayloadHandle{};
    SparseSetHandle ExtraElementsHandle{};
    uint32_t IndexCount{0};
    uint32_t FirstIndex{0};
    int32_t VertexOffset{0};
    // The inline geometry is the lowest set bit. Remaining elements are stored in bit order.
    uint32_t ValidElementMask{1};
    Nullable<const MeshDrawCommandBindingStorage*> BindingStorage{nullptr};
    uint8_t StencilReference{0};
};

static_assert(std::is_trivially_copyable_v<MeshDrawCommand> && std::is_trivially_destructible_v<MeshDrawCommand>);
static_assert(sizeof(MeshDrawCommand) == 80 && offsetof(MeshDrawCommand, BindingStorage) == 64);

using MeshParameterSetBindings = InlineVector<MeshShaderParameterBinding, 2>;

// Construction data; Store copies bytes, offsets and elements, and sorts vertex bindings.
struct MeshDrawCommandBindings {
    InlineVector<render::VertexBufferBinding, 4> VertexBuffers;
    MeshParameterSetBindings ParameterSets;
    std::optional<MeshPushConstantCommandBinding> PushConstants{};
    std::optional<uint32_t> InstanceStreamBinding{};
    InlineVector<MeshDrawElement, 6> ExtraElements;
    // Empty for a shared instance range; otherwise one entry per valid bit, including the primary.
    InlineVector<MeshDrawElementInstances, 7> ElementInstances;
};

struct MeshVertexBufferBindings {
    // Stored in strictly increasing Binding order; never split the primary stream from this array.
    InlineVector<render::VertexBufferBinding, 4> Bindings;
    std::optional<uint32_t> InstanceStreamBinding{};
};

struct MeshDrawCommandPayload {
    MeshParameterSetBindings ParameterSets;
    std::optional<MeshPushConstantCommandBinding> PushConstants{};
};

struct MeshDrawCommandElements {
    InlineVector<MeshDrawElement, 6> ExtraElements;
    InlineVector<MeshDrawElementInstances, 7> ElementInstances;
};

class MeshDrawCommandBindingStorage {
public:
    // Requires no existing payload. The caller transfers ownership to exactly one list or calls Destroy.
    void Store(MeshDrawCommand& command, const MeshDrawCommandBindings& bindings);
    void Destroy(const MeshDrawCommand& command) noexcept;
    void Clear() noexcept;
    Nullable<const MeshVertexBufferBindings*> GetVertexBuffers(SparseSetHandle handle) const noexcept;
    Nullable<const MeshDrawCommandElements*> GetExtraElements(SparseSetHandle handle) const noexcept;
    Nullable<const MeshDrawCommandPayload*> GetPayload(SparseSetHandle handle) const noexcept;

    // Immutable while commands borrow payloads. Addresses cannot survive pool mutations.
    SparseSet<MeshVertexBufferBindings> VertexBuffersPool;
    SparseSet<MeshDrawCommandPayload> Payloads;
    SparseSet<MeshDrawCommandElements> ExtraElementsPool;
};

struct CachedMeshDrawCommandInfo {
    MeshPassId Pass{};
    SparseSetHandle CommandHandle{};
    uint64_t BaseSortKey{0};
    uint32_t FirstInstance{0};
    uint32_t InstanceCount{1};
};

struct CachedPassMeshDrawList {
    CachedPassMeshDrawList() noexcept = default;
    CachedPassMeshDrawList(CachedPassMeshDrawList&& other) noexcept;
    CachedPassMeshDrawList& operator=(CachedPassMeshDrawList&& other) noexcept;
    CachedPassMeshDrawList(const CachedPassMeshDrawList&) = delete;
    CachedPassMeshDrawList& operator=(const CachedPassMeshDrawList&) = delete;
    // Transfers payload ownership and clears the initializer's payload reference.
    SparseSetHandle AddCommand(MeshDrawCommand& command);
    void DestroyCommand(SparseSetHandle handle) noexcept;
    void Clear() noexcept;

    SparseSet<MeshDrawCommand> Commands;
    MeshDrawCommandBindingStorage Bindings;
};

struct VisibleMeshDrawCommand {
    uint64_t SortKey{0};
    // Borrows either cached or finalized dynamic storage through recording; its address must stay stable.
    Nullable<const MeshDrawCommand*> CachedCommand{nullptr};
    uint32_t FirstInstance{0};
    uint32_t InstanceCount{1};
    uint32_t ElementMask{std::numeric_limits<uint32_t>::max()};
    // Gather/dynamic output preserves differing per-element defaults unless the respective override is set.
    bool UseElementFirstInstance{false};
    bool UseElementInstanceCount{false};
};

static_assert(sizeof(VisibleMeshDrawCommand) == 32 && std::is_trivially_copyable_v<VisibleMeshDrawCommand>);

struct MeshPassSubmitArgs {
    std::span<const MeshShaderParameterBinding> ParameterSets{};
    // May fill or replace only the slot declared by the command.
    std::optional<render::VertexBufferBinding> InstanceStream{};
};

bool ValidateVertexInput(const VertexInput& input, bool allowDeferredInstanceStream = false) noexcept;
bool ValidateMeshDrawCommand(const MeshDrawCommand& command, const MeshDrawCommandBindings& bindings,
                             const VisibleMeshDrawCommand& visibleCommand = {},
                             const MeshPassSubmitArgs& submitArgs = {}) noexcept;
bool ValidateMeshDrawCommand(const MeshDrawCommand& command, const MeshDrawCommandBindingStorage& storage,
                             const VisibleMeshDrawCommand& visibleCommand = {},
                             const MeshPassSubmitArgs& submitArgs = {}) noexcept;

// Debug preflights the whole list; Release trusts validated commands and submit arguments.
// State is deduplicated within this call. On failure, discard the recording (it may be partial).
// Pass boundaries, viewport/scissor and submission belong to the caller.
bool SubmitMeshDrawCommands(render::GraphicsCommandEncoder& encoder,
                            std::span<const VisibleMeshDrawCommand> visibleCommands,
                            const MeshPassSubmitArgs& submitArgs = {}) noexcept;

}  // namespace radray
