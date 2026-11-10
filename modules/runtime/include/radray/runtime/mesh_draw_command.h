#pragma once

// 命令构建与资源生命周期见 docs/architecture/runtime.md。

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

class MeshPassId {
public:
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
    // 仅在命令构建期间借用；命令会复制这些字节。
    std::span<const byte> Data{};
};

struct MeshPushConstantCommandBinding {
    uint32_t PushConstantId{0};
    uint32_t Offset{0};
    InlineVector<byte, 64> Data;
};

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

// 副本借用绑定；仅所有者可以释放其持有的引用。
struct alignas(64) MeshDrawCommand {
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    render::IndexBufferView IndexBuffer{};
    Nullable<const MeshDrawCommandBindingStorage*> BindingStorage{nullptr};
    uint32_t IndexCount{0};
    uint32_t FirstIndex{0};
    int32_t VertexOffset{0};
    // 内联几何对应最低有效位；其余元素按掩码位序保存。
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

// 命令构建数据；Store 复制字节、偏移与元素，并对顶点绑定排序。
struct MeshDrawCommandBindings {
    InlineVector<render::VertexBufferBinding, 4> VertexBuffers;
    MeshParameterSetBindings ParameterSets;
    std::optional<MeshPushConstantCommandBinding> PushConstants{};
    std::optional<uint32_t> InstanceStreamBinding{};
    InlineVector<MeshDrawElement, 6> ExtraElements;
    // 共享实例区间时为空；否则每个有效位各有一项，包括主元素。
    InlineVector<MeshDrawElementInstances, 7> ElementInstances;
    uint32_t ContiguousMergeMask{0};
    bool AllElementsSharePrimaryIndexBuffer{false};
};

struct MeshVertexBufferBindings {
    // 按 Binding 严格递增保存；不得将主顶点流从此数组中拆出。
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
    // Store 获取一个引用，仅其所有者可以调用 Destroy；副本借用该引用。
    void Store(MeshDrawCommand& command, const MeshDrawCommandBindings& bindings);
    void Destroy(const MeshDrawCommand& command) noexcept;
    void Clear() noexcept;
    // 索引不含代数，随所属引用失效；返回的地址不得跨池修改保存。
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
    friend class Scene;
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
    // 录制期间借用缓存或已固定的动态命令存储；被借用地址必须保持稳定。
    Nullable<const MeshDrawCommand*> CachedCommand{nullptr};
    uint32_t FirstInstance{0};
    uint32_t InstanceCount{1};
    uint32_t ElementMask{std::numeric_limits<uint32_t>::max()};
    // 收集和动态输出保留各元素不同的默认值，除非设置了对应覆盖项。
    bool UseElementFirstInstance{false};
    bool UseElementInstanceCount{false};
};

static_assert(sizeof(VisibleMeshDrawCommand) == 32 && std::is_trivially_copyable_v<VisibleMeshDrawCommand>);

struct MeshPassSubmitArgs {
    std::span<const MeshShaderParameterBinding> ParameterSets{};
    // 仅可填充或替换命令已声明的实例流绑定槽。
    std::optional<render::VertexBufferBinding> InstanceStream{};
};

bool ValidateVertexInput(const VertexInput& input, bool allowDeferredInstanceStream = false) noexcept;
bool ValidateMeshDrawCommand(const MeshDrawCommand& command, const MeshDrawCommandBindings& bindings,
                             const VisibleMeshDrawCommand& visibleCommand = {},
                             const MeshPassSubmitArgs& submitArgs = {}) noexcept;
bool ValidateMeshDrawCommand(const MeshDrawCommand& command, const MeshDrawCommandBindingStorage& storage,
                             const VisibleMeshDrawCommand& visibleCommand = {},
                             const MeshPassSubmitArgs& submitArgs = {}) noexcept;

// Debug 预检整个列表；Release 依赖命令与提交参数已经过校验。
// 本次调用内复用已绑定状态；失败时须丢弃可能仅完成部分录制的命令。
// Pass 边界、视口、裁剪与提交由调用方负责。
bool SubmitMeshDrawCommands(render::GraphicsCommandEncoder& encoder,
                            std::span<const VisibleMeshDrawCommand> visibleCommands,
                            const MeshPassSubmitArgs& submitArgs = {}) noexcept;

}  // namespace radray
