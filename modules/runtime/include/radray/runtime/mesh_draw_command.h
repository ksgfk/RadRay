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
struct VisibleMeshDrawCommand;

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

inline constexpr uint32_t kInvalidBindingIndex = std::numeric_limits<uint32_t>::max();

enum class MeshPassSortMode : uint8_t { StateFirst,
                                        DepthFirst };

constexpr uint64_t ComposeMeshDrawSortKey(MeshPassSortMode mode, uint32_t stateSortKey, uint32_t visibilitySortKey) noexcept {
    return mode == MeshPassSortMode::DepthFirst ? (uint64_t{visibilitySortKey} << 32) | stateSortKey : (uint64_t{stateSortKey} << 32) | visibilitySortKey;
}

// Copies borrow the bindings; only the owner may release its reference.
struct alignas(64) MeshDrawCommand {
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    render::IndexBufferView IndexBuffer{};
    Nullable<const MeshDrawCommandBindingStorage*> BindingStorage{nullptr};
    uint32_t IndexCount{0};
    uint32_t FirstIndex{0};
    int32_t VertexOffset{0};
    // The inline geometry is the lowest set bit. Remaining elements are stored in bit order.
    uint32_t ValidElementMask{1};
    uint32_t VertexBuffersIndex{kInvalidBindingIndex};
    uint32_t PayloadIndex{kInvalidBindingIndex};
    uint32_t ExtraElementsIndex{kInvalidBindingIndex};
    uint8_t StencilReference{0};
    uint8_t Padding[3]{};
};

static_assert(std::is_trivially_copyable_v<MeshDrawCommand> && std::is_trivially_destructible_v<MeshDrawCommand>);
static_assert(sizeof(MeshDrawCommand) == 64 && alignof(MeshDrawCommand) == 64);

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
    uint32_t ContiguousMergeMask{0};
    bool AllElementsSharePrimaryIndexBuffer{false};
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
    uint32_t ContiguousMergeMask{0};
    bool AllElementsSharePrimaryIndexBuffer{false};
};

template <typename T>
struct InternedBindingEntry {
    T Value{};
    uint64_t Hash{0};
    uint32_t RefCount{0};
};

class MeshDrawCommandBindingStorage {
public:
    MeshDrawCommandBindingStorage() noexcept = default;
    MeshDrawCommandBindingStorage(MeshDrawCommandBindingStorage&&) noexcept = default;
    MeshDrawCommandBindingStorage& operator=(MeshDrawCommandBindingStorage&&) noexcept = default;
    MeshDrawCommandBindingStorage(const MeshDrawCommandBindingStorage&) = delete;
    MeshDrawCommandBindingStorage& operator=(const MeshDrawCommandBindingStorage&) = delete;
    // Store acquires one reference; only its owner may Destroy it. Copies borrow that reference.
    void Store(MeshDrawCommand& command, const MeshDrawCommandBindings& bindings);
    void Destroy(const MeshDrawCommand& command) noexcept;
    void Clear() noexcept;
    // Indices have no generation and expire with the owned reference. Addresses cannot survive pool mutations.
    Nullable<const MeshVertexBufferBindings*> GetVertexBuffers(uint32_t index) const noexcept;
    Nullable<const MeshDrawCommandElements*> GetExtraElements(uint32_t index) const noexcept;
    Nullable<const MeshDrawCommandPayload*> GetPayload(uint32_t index) const noexcept;
    uint32_t GetStateSortKey(const MeshDrawCommand& command);
    size_t GetVertexBufferCount() const noexcept { return _vertices.Entries.size() - _vertices.Free.size(); }
    size_t GetPayloadCount() const noexcept { return _payloads.Entries.size() - _payloads.Free.size(); }
    size_t GetExtraElementsCount() const noexcept { return _elements.size() - _freeElements.size(); }

private:
    friend class MeshPassProcessor;
    friend class CachedPassMeshDrawList;
    void Reserve(uint32_t commands);
    static bool ValidateConstruction(const MeshDrawCommand& command, const MeshDrawCommandBindings& bindings, const VisibleMeshDrawCommand& visible, bool validateElements) noexcept;

    template <typename T>
    struct Pool {
        vector<InternedBindingEntry<T>> Entries;
        vector<uint32_t> Free;
        unordered_map<uint64_t, vector<uint32_t>> Lookup;
    };
    Pool<MeshVertexBufferBindings> _vertices;
    Pool<MeshDrawCommandPayload> _payloads;
    vector<std::optional<MeshDrawCommandElements>> _elements;
    vector<uint32_t> _freeElements;
    unordered_map<render::GraphicsPipelineState*, uint32_t> _pipelineBuckets;
};

struct PassBatchCacheSlot {
    uint32_t CommandDenseIndex{kInvalidBindingIndex};
    uint32_t StateSortKey{0};
    uint32_t FirstInstance{0};
    uint32_t InstanceCount{1};
};
static_assert(sizeof(PassBatchCacheSlot) == 16);

class CachedPassMeshDrawList {
public:
    CachedPassMeshDrawList() noexcept = default;
    CachedPassMeshDrawList(CachedPassMeshDrawList&& other) noexcept;
    CachedPassMeshDrawList& operator=(CachedPassMeshDrawList&& other) noexcept;
    CachedPassMeshDrawList(const CachedPassMeshDrawList&) = delete;
    CachedPassMeshDrawList& operator=(const CachedPassMeshDrawList&) = delete;
    std::span<const MeshDrawCommand> GetCommands() const noexcept { return _commands; }
    const MeshDrawCommandBindingStorage& GetBindings() const noexcept { return _bindings; }
    Nullable<const PassBatchCacheSlot*> GetBatchSlot(uint32_t batchIndex) const noexcept;
    Nullable<const MeshDrawCommand*> GetCommand(uint32_t batchIndex) const noexcept;
    MeshPassSortMode GetSortMode() const noexcept { return _sortMode; }

private:
    friend struct Scene;
    friend class CachedPassMeshDrawListContext;
    MeshDrawCommand& AddCommand(uint32_t batchIndex, MeshDrawCommand& command);
    void DestroyCommand(uint32_t batchIndex) noexcept;
    void Clear() noexcept;
    void Reserve(uint32_t batches);

    vector<MeshDrawCommand> _commands;
    vector<uint32_t> _commandBatchIndices;
    vector<PassBatchCacheSlot> _batchSlots;
    MeshDrawCommandBindingStorage _bindings;
    MeshPassSortMode _sortMode{MeshPassSortMode::StateFirst};
    bool _registered{false};
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
