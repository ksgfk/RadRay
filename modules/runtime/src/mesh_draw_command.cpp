#include <radray/runtime/mesh_draw_command.h>

#include <algorithm>
#include <bit>
#include <utility>
#include <limits>

#include <radray/profiler.h>

namespace radray {
namespace {

bool ValidateVertexBuffer(const gpu::VertexBufferView& view) noexcept {
    if (!view.Target || !view.Target->IsValid() || view.Size == 0 || view.Size > std::numeric_limits<uint32_t>::max()) return false;
    const auto desc = view.Target->GetDesc();
    return desc.Usage.HasFlag(gpu::BufferUse::Vertex) && view.Offset <= desc.Size && view.Size <= desc.Size - view.Offset;
}

bool ValidateParameterSets(std::span<const MeshShaderParameterBinding> bindings) noexcept {
    for (size_t index = 0; index < bindings.size(); ++index) {
        const auto& binding = bindings[index];
        if (!binding.ParameterSet || !binding.ParameterSet->IsValid()) return false;
        for (size_t previous = 0; previous < index; ++previous) {
            if (bindings[previous].GroupIndex == binding.GroupIndex) return false;
        }
        for (size_t offset = 0; offset < binding.DynamicOffsets.size(); ++offset) {
            for (size_t previous = 0; previous < offset; ++previous) {
                if (binding.DynamicOffsets[previous].BindingId == binding.DynamicOffsets[offset].BindingId &&
                    binding.DynamicOffsets[previous].ArrayIndex == binding.DynamicOffsets[offset].ArrayIndex) return false;
            }
        }
    }
    return true;
}

bool ValidateSubmitArgs(const MeshPassSubmitArgs& submitArgs) noexcept {
    return ValidateParameterSets(submitArgs.ParameterSets) &&
           (!submitArgs.InstanceStream || ValidateVertexBuffer(submitArgs.InstanceStream->View));
}

bool SameVertexBufferView(const gpu::VertexBufferView& a, const gpu::VertexBufferView& b) noexcept {
    return a.Target == b.Target && a.Offset == b.Offset && a.Size == b.Size;
}

bool SameIndexBufferView(const gpu::IndexBufferView& a, const gpu::IndexBufferView& b) noexcept {
    return a.Target == b.Target && a.Offset == b.Offset && a.Stride == b.Stride;
}

bool SameParameterBinding(const MeshShaderParameterBinding& a, const MeshShaderParameterBinding& b) noexcept {
    return a.GroupIndex == b.GroupIndex && a.ParameterSet.Get() == b.ParameterSet.Get() && a.DynamicOffsets == b.DynamicOffsets;
}

bool SamePushConstants(const MeshPushConstantCommandBinding& a, const MeshPushConstantCommandBinding& b) noexcept {
    return a.PushConstantId == b.PushConstantId && a.Offset == b.Offset && a.Data == b.Data;
}

struct MeshDrawCommandStateCache {
    Nullable<gpu::GraphicsPipelineState*> Pipeline{nullptr};
    Nullable<gpu::PipelineLayout*> Layout{nullptr};
    std::optional<gpu::IndexBufferView> IndexBuffer{};
    std::optional<uint8_t> StencilReference{};
    array<gpu::VertexBufferView, 32> BoundVertexSlots{};
    uint32_t BoundVertexMask{0};
    Nullable<const MeshDrawCommandBindingStorage*> LastVertexStorage{nullptr};
    uint32_t LastVertexBuffersIndex{kInvalidBindingIndex};
    Nullable<const MeshDrawCommandBindingStorage*> LastPayloadStorage{nullptr};
    uint32_t LastPayloadIndex{kInvalidBindingIndex};
    Nullable<const MeshPushConstantCommandBinding*> BoundPushConstants{nullptr};
    InlineVector<MeshShaderParameterBinding, 4> CommandParameterSets;

    void OnPipelineChanged(gpu::GraphicsPipelineState* pipeline, Nullable<gpu::PipelineLayout*> layout, bool sameLayout) noexcept {
        Pipeline = pipeline;
        Layout = layout;
        BoundVertexMask = 0;
        LastVertexStorage = nullptr;
        LastVertexBuffersIndex = kInvalidBindingIndex;
        if (!sameLayout) {
            LastPayloadStorage = nullptr;
            LastPayloadIndex = kInvalidBindingIndex;
            BoundPushConstants = nullptr;
            CommandParameterSets.clear();
        }
    }

    bool UpdateVertexBuffer(const gpu::VertexBufferBinding& binding) noexcept {
        if (binding.Binding >= BoundVertexSlots.size()) return true;
        const auto bit = uint32_t{1} << binding.Binding;
        auto& slot = BoundVertexSlots[binding.Binding];
        if ((BoundVertexMask & bit) && SameVertexBufferView(slot, binding.View)) return false;
        slot = binding.View;
        BoundVertexMask |= bit;
        return true;
    }

    bool UpdateParameterSet(const MeshShaderParameterBinding& binding) {
        const auto found = std::find_if(CommandParameterSets.begin(), CommandParameterSets.end(), [&](const MeshShaderParameterBinding& cached) {
            return cached.GroupIndex == binding.GroupIndex;
        });
        if (found != CommandParameterSets.end()) {
            if (SameParameterBinding(*found, binding)) return false;
            *found = binding;
        } else {
            CommandParameterSets.push_back(binding);
        }
        return true;
    }
};

}  // namespace

namespace {

uint64_t MixBindingHash(uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

void HashField(uint64_t& hash, uint64_t value) noexcept { hash = MixBindingHash(hash ^ MixBindingHash(value)); }

uint64_t HashBinding(const MeshVertexBufferBindings& value) noexcept {
    uint64_t hash = value.Bindings.size();
    HashField(hash, value.InstanceStreamBinding ? uint64_t{*value.InstanceStreamBinding} + 1 : 0);
    for (const auto& binding : value.Bindings) {
        HashField(hash, binding.Binding);
        HashField(hash, reinterpret_cast<uintptr_t>(binding.View.Target));
        HashField(hash, binding.View.Offset);
        HashField(hash, binding.View.Size);
    }
    HashField(hash, value.StreamLayouts.size());
    for (const auto& layout : value.StreamLayouts) {
        HashField(hash, layout.Binding);
        HashField(hash, layout.ArrayStride);
        HashField(hash, static_cast<uint64_t>(layout.StepMode));
    }
    return hash;
}

bool SameBinding(const MeshVertexBufferBindings& a, const MeshVertexBufferBindings& b) noexcept {
    return a.InstanceStreamBinding == b.InstanceStreamBinding &&
           a.Bindings.size() == b.Bindings.size() &&
           a.StreamLayouts.size() == b.StreamLayouts.size() &&
           std::equal(a.Bindings.begin(), a.Bindings.end(), b.Bindings.begin(), [](const auto& x, const auto& y) {
               return x.Binding == y.Binding && SameVertexBufferView(x.View, y.View);
           }) &&
           std::equal(a.StreamLayouts.begin(), a.StreamLayouts.end(), b.StreamLayouts.begin(), [](const auto& x, const auto& y) {
               return x.Binding == y.Binding && x.ArrayStride == y.ArrayStride && x.StepMode == y.StepMode;
           });
}

uint64_t HashBinding(const MeshDrawCommandPayload& value) noexcept {
    uint64_t hash = value.ParameterSets.size();
    for (const auto& binding : value.ParameterSets) {
        HashField(hash, binding.GroupIndex);
        HashField(hash, reinterpret_cast<uintptr_t>(binding.ParameterSet.Get()));
        HashField(hash, binding.DynamicOffsets.size());
        for (const auto& offset : binding.DynamicOffsets) {
            HashField(hash, offset.BindingId);
            HashField(hash, offset.ArrayIndex);
            HashField(hash, offset.Offset);
        }
    }
    HashField(hash, value.PushConstants.has_value());
    if (value.PushConstants) {
        const auto& push = *value.PushConstants;
        HashField(hash, push.PushConstantId);
        HashField(hash, push.Offset);
        HashField(hash, push.Data.size());
        for (const auto byte : push.Data) HashField(hash, std::to_integer<uint8_t>(byte));
    }
    return hash;
}

bool SameBinding(const MeshDrawCommandPayload& a, const MeshDrawCommandPayload& b) noexcept {
    if (a.ParameterSets.size() != b.ParameterSets.size() ||
        !std::equal(a.ParameterSets.begin(), a.ParameterSets.end(), b.ParameterSets.begin(), SameParameterBinding) ||
        a.PushConstants.has_value() != b.PushConstants.has_value()) return false;
    return !a.PushConstants || (a.PushConstants->PushConstantId == b.PushConstants->PushConstantId &&
                                a.PushConstants->Offset == b.PushConstants->Offset && a.PushConstants->Data == b.PushConstants->Data);
}

template <typename Pool, typename T>
uint32_t InternBinding(Pool& pool, T value) {
    const auto hash = HashBinding(value);
    auto& candidates = pool.Lookup[hash];
    for (const auto index : candidates) {
        auto& entry = pool.Entries[index];
        if (SameBinding(entry.Value, value)) {
            RADRAY_ASSERT(entry.RefCount < std::numeric_limits<uint32_t>::max());
            ++entry.RefCount;
            return index;
        }
    }
    RADRAY_ASSERT(pool.Entries.size() < kInvalidBindingIndex);
    const auto index = pool.Free.empty() ? static_cast<uint32_t>(pool.Entries.size()) : pool.Free.back();
    if (pool.Free.empty())
        pool.Entries.emplace_back();
    else
        pool.Free.pop_back();
    pool.Entries[index] = {std::move(value), hash, 1};
    candidates.push_back(index);
    return index;
}

template <typename Pool>
void ReleaseBinding(Pool& pool, uint32_t index) noexcept {
    if (index == kInvalidBindingIndex) return;
    auto& entry = pool.Entries[index];
    RADRAY_ASSERT(entry.RefCount != 0);
    if (--entry.RefCount != 0) return;
    auto found = pool.Lookup.find(entry.Hash);
    std::erase(found->second, index);
    if (found->second.empty()) pool.Lookup.erase(found);
    entry.Value = {};
    pool.Free.push_back(index);
}

uint32_t BucketFromHash(uint64_t hash, uint32_t maxBucket) noexcept {
    return 1u + static_cast<uint32_t>(MixBindingHash(hash) % maxBucket);
}

}  // namespace

void MeshDrawCommandBindingStorage::Reserve(uint32_t commands) {
    _vertices.Entries.reserve(commands);
    _vertices.Free.reserve(commands);
    _payloads.Entries.reserve(commands);
    _payloads.Free.reserve(commands);
    _elements.reserve(commands);
    _freeElements.reserve(commands);
}

void MeshDrawCommandBindingStorage::Store(MeshDrawCommand& command, const MeshDrawCommandBindings& bindings) {
    RADRAY_ASSERT(command.VertexBuffersIndex == kInvalidBindingIndex && command.PayloadIndex == kInvalidBindingIndex && command.ExtraElementsIndex == kInvalidBindingIndex && !command.BindingStorage);
    if (!bindings.VertexBuffers.empty() || !bindings.VertexStreamLayouts.empty() || bindings.InstanceStreamBinding) {
        MeshVertexBufferBindings vertices{bindings.VertexBuffers, bindings.VertexStreamLayouts, bindings.InstanceStreamBinding};
        std::sort(vertices.Bindings.begin(), vertices.Bindings.end(), [](const auto& a, const auto& b) { return a.Binding < b.Binding; });
        std::sort(vertices.StreamLayouts.begin(), vertices.StreamLayouts.end(), [](const auto& a, const auto& b) { return a.Binding < b.Binding; });
        command.VertexBuffersIndex = InternBinding(_vertices, std::move(vertices));
    }
    if (!bindings.ParameterSets.empty() || bindings.PushConstants) {
        command.PayloadIndex = InternBinding(_payloads, MeshDrawCommandPayload{bindings.ParameterSets, bindings.PushConstants});
    }
    if (!bindings.ExtraElements.empty() || !bindings.ElementInstances.empty()) {
        RADRAY_ASSERT(_elements.size() < kInvalidBindingIndex);
        const auto index = _freeElements.empty() ? static_cast<uint32_t>(_elements.size()) : _freeElements.back();
        if (_freeElements.empty())
            _elements.emplace_back();
        else
            _freeElements.pop_back();
        _elements[index] = MeshDrawCommandElements{bindings.ExtraElements, bindings.ElementInstances, bindings.ContiguousMergeMask, bindings.AllElementsSharePrimaryIndexBuffer};
        command.ExtraElementsIndex = index;
    }
    if (command.VertexBuffersIndex != kInvalidBindingIndex || command.PayloadIndex != kInvalidBindingIndex || command.ExtraElementsIndex != kInvalidBindingIndex) command.BindingStorage = this;
}

void MeshDrawCommandBindingStorage::Destroy(const MeshDrawCommand& command) noexcept {
    RADRAY_ASSERT(!command.BindingStorage || command.BindingStorage.Get() == this);
    ReleaseBinding(_vertices, command.VertexBuffersIndex);
    ReleaseBinding(_payloads, command.PayloadIndex);
    if (command.ExtraElementsIndex != kInvalidBindingIndex) {
        RADRAY_ASSERT(_elements[command.ExtraElementsIndex]);
        _elements[command.ExtraElementsIndex].reset();
        _freeElements.push_back(command.ExtraElementsIndex);
    }
}

void MeshDrawCommandBindingStorage::Clear() noexcept {
    _vertices.Entries.clear();
    _vertices.Free.clear();
    _vertices.Lookup.clear();
    _payloads.Entries.clear();
    _payloads.Free.clear();
    _payloads.Lookup.clear();
    _elements.clear();
    _freeElements.clear();
}

Nullable<const MeshVertexBufferBindings*> MeshDrawCommandBindingStorage::GetVertexBuffers(uint32_t index) const noexcept {
    return index < _vertices.Entries.size() && _vertices.Entries[index].RefCount ? &_vertices.Entries[index].Value : nullptr;
}
Nullable<const MeshDrawCommandElements*> MeshDrawCommandBindingStorage::GetExtraElements(uint32_t index) const noexcept {
    return index < _elements.size() && _elements[index] ? &*_elements[index] : nullptr;
}
Nullable<const MeshDrawCommandPayload*> MeshDrawCommandBindingStorage::GetPayload(uint32_t index) const noexcept {
    return index < _payloads.Entries.size() && _payloads.Entries[index].RefCount ? &_payloads.Entries[index].Value : nullptr;
}
uint32_t MeshDrawCommandBindingStorage::GetStateSortKey(const MeshDrawCommand& command) const noexcept {
    const auto layout = command.Pipeline ? command.Pipeline->GetLayout() : Nullable<gpu::PipelineLayout*>{nullptr};
    const uint32_t layoutBucket = layout ? BucketFromHash(reinterpret_cast<uintptr_t>(layout.Get()), 15u) : 0u;
    const uint32_t pipelineBucket = command.Pipeline ? BucketFromHash(reinterpret_cast<uintptr_t>(command.Pipeline.Get()), 511u) : 0u;
    const uint32_t payloadBucket =
        (command.PayloadIndex != kInvalidBindingIndex && command.PayloadIndex < _payloads.Entries.size() && _payloads.Entries[command.PayloadIndex].RefCount != 0)
            ? BucketFromHash(_payloads.Entries[command.PayloadIndex].Hash, 511u)
            : 0u;
    const bool hasVertices =
        command.VertexBuffersIndex != kInvalidBindingIndex && command.VertexBuffersIndex < _vertices.Entries.size() && _vertices.Entries[command.VertexBuffersIndex].RefCount != 0;
    const bool hasIndex = command.IndexBuffer.Target != nullptr;
    uint32_t geometryBucket = 0u;
    if (hasVertices || hasIndex) {
        uint64_t geoHash = hasVertices ? _vertices.Entries[command.VertexBuffersIndex].Hash : 0u;
        HashField(geoHash, hasIndex ? 1u : 0u);
        if (hasIndex) {
            HashField(geoHash, reinterpret_cast<uintptr_t>(command.IndexBuffer.Target));
            HashField(geoHash, command.IndexBuffer.Offset);
            HashField(geoHash, command.IndexBuffer.Stride);
        }
        geometryBucket = BucketFromHash(geoHash, 255u);
    }
    const uint32_t stencilBucket =
        command.StencilReference == 0 ? 0u : 1u + static_cast<uint32_t>((command.StencilReference - 1u) % 3u);
    return (layoutBucket << 28) | (pipelineBucket << 19) | (payloadBucket << 10) | (geometryBucket << 2) | stencilBucket;
}

CachedPassMeshDrawList::CachedPassMeshDrawList(CachedPassMeshDrawList&& other) noexcept { *this = std::move(other); }
CachedPassMeshDrawList& CachedPassMeshDrawList::operator=(CachedPassMeshDrawList&& other) noexcept {
    if (this != &other) {
        _commands = std::move(other._commands);
        _commandBatchIndices = std::move(other._commandBatchIndices);
        _batchSlots = std::move(other._batchSlots);
        _bindings = std::move(other._bindings);
        _sortMode = other._sortMode;
        _registered = other._registered;
        for (auto& command : _commands)
            if (command.BindingStorage) command.BindingStorage = &_bindings;
        other.Clear();
    }
    return *this;
}
void CachedPassMeshDrawList::Reserve(uint32_t batches) {
    _commands.reserve(batches);
    _commandBatchIndices.reserve(batches);
    _batchSlots.reserve(batches);
    _bindings.Reserve(batches);
}
Nullable<const PassBatchCacheSlot*> CachedPassMeshDrawList::GetBatchSlot(uint32_t batchIndex) const noexcept {
    return batchIndex < _batchSlots.size() ? &_batchSlots[batchIndex] : nullptr;
}
Nullable<const MeshDrawCommand*> CachedPassMeshDrawList::GetCommand(uint32_t batchIndex) const noexcept {
    const auto slot = GetBatchSlot(batchIndex);
    return slot && slot->CommandDenseIndex != kInvalidBindingIndex ? &_commands[slot->CommandDenseIndex] : nullptr;
}
MeshDrawCommand& CachedPassMeshDrawList::AddCommand(uint32_t batchIndex, MeshDrawCommand& command) {
    RADRAY_ASSERT(!command.BindingStorage || command.BindingStorage.Get() == &_bindings);
    if (_batchSlots.size() <= batchIndex) _batchSlots.resize(size_t{batchIndex} + 1);
    RADRAY_ASSERT(_batchSlots[batchIndex].CommandDenseIndex == kInvalidBindingIndex);
    _batchSlots[batchIndex].CommandDenseIndex = static_cast<uint32_t>(_commands.size());
    _commandBatchIndices.push_back(batchIndex);
    auto& result = _commands.emplace_back(command);
    command.BindingStorage = nullptr;
    command.PayloadIndex = command.VertexBuffersIndex = command.ExtraElementsIndex = kInvalidBindingIndex;
    return result;
}
void CachedPassMeshDrawList::DestroyCommand(uint32_t batchIndex) noexcept {
    if (batchIndex >= _batchSlots.size()) return;
    auto& slot = _batchSlots[batchIndex];
    if (slot.CommandDenseIndex == kInvalidBindingIndex) return;
    const auto index = slot.CommandDenseIndex;
    _bindings.Destroy(_commands[index]);
    if (index != _commands.size() - 1) {
        _commands[index] = _commands.back();
        const auto movedBatch = _commandBatchIndices.back();
        _commandBatchIndices[index] = movedBatch;
        _batchSlots[movedBatch].CommandDenseIndex = index;
    }
    _commands.pop_back();
    _commandBatchIndices.pop_back();
    slot = {};
}
void CachedPassMeshDrawList::Clear() noexcept {
    _commands.clear();
    _commandBatchIndices.clear();
    _batchSlots.clear();
    _bindings.Clear();
    _registered = false;
}

bool ValidateVertexInput(const VertexInput& input, bool allowDeferredInstanceStream) noexcept {
    if (!ValidateVertexInputLayout(input)) return false;
    size_t instanceLayouts = 0;
    size_t deferredStreams = 0;
    for (const auto& layout : input.Layouts) {
        if (layout.StepMode != gpu::VertexStepMode::Vertex && layout.StepMode != gpu::VertexStepMode::Instance) return false;
        if (layout.StepMode == gpu::VertexStepMode::Instance) ++instanceLayouts;
        const auto stream = std::find_if(input.Streams.begin(), input.Streams.end(), [&](const VertexInputStream& value) {
            return value.Binding == layout.Binding;
        });
        if (stream == input.Streams.end()) {
            if (allowDeferredInstanceStream && layout.StepMode == gpu::VertexStepMode::Instance) {
                ++deferredStreams;
                continue;
            }
            return false;
        }
        if (stream->Size < layout.ArrayStride) return false;
    }
    if (deferredStreams != 0 && instanceLayouts > 1) return false;
    for (size_t index = 0; index < input.Streams.size(); ++index) {
        const auto& stream = input.Streams[index];
        if (!ValidateVertexBuffer({stream.Buffer.Get(), stream.Offset, stream.Size}) ||
            std::none_of(input.Layouts.begin(), input.Layouts.end(), [&](const gpu::VertexBufferLayout& layout) { return layout.Binding == stream.Binding; })) return false;
        for (size_t previous = 0; previous < index; ++previous) {
            if (input.Streams[previous].Binding == stream.Binding) return false;
        }
    }
    return true;
}

namespace {

bool StreamFitsRange(uint64_t viewSize, uint32_t stride, uint32_t first, uint32_t count) noexcept {
    if (count == 0) return true;
    const uint64_t end = uint64_t{first} + count;
    if (stride == 0) return viewSize > 0;
    return end <= viewSize / stride;
}

Nullable<const gpu::VertexBufferView*> FindActiveVertexView(std::span<const gpu::VertexBufferBinding> vertexBuffers,
                                                            const MeshPassSubmitArgs& submitArgs,
                                                            uint32_t binding) noexcept {
    if (submitArgs.InstanceStream && submitArgs.InstanceStream->Binding == binding) {
        return &submitArgs.InstanceStream->View;
    }
    for (const auto& entry : vertexBuffers) {
        if (entry.Binding == binding) return &entry.View;
    }
    return nullptr;
}

// 调用方已经校验整个 Pass 共用的参数。
bool ValidateMeshDrawCommandImpl(const MeshDrawCommand& command, std::span<const gpu::VertexBufferBinding> vertexBuffers,
                                 std::span<const MeshVertexStreamLayout> streamLayouts,
                                 std::span<const MeshShaderParameterBinding> parameterSets,
                                 Nullable<const MeshPushConstantCommandBinding*> pushConstants, std::optional<uint32_t> instanceStreamBinding,
                                 const MeshPassSubmitArgs& submitArgs, bool validateGeometry = true, bool allowDeferredInstanceStream = false) noexcept {
    const auto& indices = command.IndexBuffer;
    if (!command.Pipeline || !command.Pipeline->IsValid()) return false;
    if (validateGeometry) {
        if (indices.Target != nullptr) {
            if (!indices.Target->IsValid() || command.IndexCount == 0 ||
                (indices.Stride != 2 && indices.Stride != 4) || indices.Offset % indices.Stride != 0) return false;
            const auto desc = indices.Target->GetDesc();
            const uint64_t end = uint64_t{indices.Offset} + (uint64_t{command.FirstIndex} + command.IndexCount) * indices.Stride;
            if (!desc.Usage.HasFlag(gpu::BufferUse::Index) || end > desc.Size) return false;
        } else {
            if (indices.Offset != 0 || indices.Stride != 0 || command.VertexOffset != 0 || command.IndexCount == 0) return false;
        }
    }
    for (size_t index = 0; index < streamLayouts.size(); ++index) {
        const auto& layout = streamLayouts[index];
        if (layout.StepMode != gpu::VertexStepMode::Vertex && layout.StepMode != gpu::VertexStepMode::Instance) return false;
        for (size_t previous = 0; previous < index; ++previous) {
            if (streamLayouts[previous].Binding == layout.Binding) return false;
        }
    }
    bool hasDefaultInstanceStream = false;
    for (size_t index = 0; index < vertexBuffers.size(); ++index) {
        const auto& binding = vertexBuffers[index];
        const bool isInstanceStream = instanceStreamBinding && *instanceStreamBinding == binding.Binding;
        hasDefaultInstanceStream |= isInstanceStream;
        if (!(isInstanceStream && submitArgs.InstanceStream && submitArgs.InstanceStream->Binding == binding.Binding) && !ValidateVertexBuffer(binding.View)) return false;
        for (size_t previous = 0; previous < index; ++previous) {
            if (vertexBuffers[previous].Binding == binding.Binding) return false;
        }
    }
    if (instanceStreamBinding && !hasDefaultInstanceStream && !allowDeferredInstanceStream &&
        (!submitArgs.InstanceStream || submitArgs.InstanceStream->Binding != *instanceStreamBinding)) return false;
    if (submitArgs.InstanceStream && (!instanceStreamBinding || submitArgs.InstanceStream->Binding != *instanceStreamBinding)) return false;
    if (!ValidateParameterSets({parameterSets.data(), parameterSets.size()})) return false;
    for (const auto& binding : parameterSets) {
        for (const auto& passBinding : submitArgs.ParameterSets) {
            if (binding.GroupIndex == passBinding.GroupIndex) return false;
        }
    }
    if (pushConstants) {
        const auto& push = *pushConstants;
        if (push.Data.empty() || push.Offset % 4 != 0 || push.Data.size() % 4 != 0 ||
            push.Data.size() > std::numeric_limits<uint32_t>::max() - push.Offset) return false;
    }
    return true;
}

bool ValidateElements(const MeshDrawCommand& command, std::span<const MeshDrawElement> extraElements,
                      std::span<const MeshDrawElementInstances> instances,
                      std::span<const gpu::VertexBufferBinding> vertexBuffers,
                      std::span<const MeshVertexStreamLayout> streamLayouts,
                      const VisibleMeshDrawCommand& visible, const MeshPassSubmitArgs& submitArgs,
                      bool validateGeometry = true, bool allowDeferredInstanceStream = false) noexcept {
    if (command.ValidElementMask == 0 || extraElements.size() + 1 != static_cast<size_t>(std::popcount(command.ValidElementMask)) ||
        (!instances.empty() && instances.size() != extraElements.size() + 1)) return false;
    const bool indexed = command.IndexBuffer.Target != nullptr;
    uint32_t remaining = command.ValidElementMask & (command.ValidElementMask - 1);
    for (const auto& element : extraElements) {
        if (element.ElementIndex != static_cast<uint32_t>(std::countr_zero(remaining))) return false;
        remaining &= remaining - 1;
        if (!validateGeometry) continue;
        const auto& indices = element.IndexBuffer;
        if (indexed) {
            if (!indices.Target || !indices.Target->IsValid() || element.IndexCount == 0 ||
                (indices.Stride != 2 && indices.Stride != 4) || indices.Offset % indices.Stride != 0) return false;
            const auto desc = indices.Target->GetDesc();
            if (!desc.Usage.HasFlag(gpu::BufferUse::Index) ||
                uint64_t{indices.Offset} + (uint64_t{element.FirstIndex} + element.IndexCount) * indices.Stride > desc.Size) return false;
        } else {
            if (indices.Target != nullptr || indices.Offset != 0 || indices.Stride != 0 || element.VertexOffset != 0 || element.IndexCount == 0) return false;
        }
    }
    for (const auto& instance : instances) {
        if (instance.InstanceCount != 0 && uint64_t{instance.FirstInstance} + instance.InstanceCount - 1 > std::numeric_limits<uint32_t>::max()) return false;
    }
    if (!indexed && !streamLayouts.empty()) {
        for (const auto& layout : streamLayouts) {
            if (layout.StepMode != gpu::VertexStepMode::Vertex) continue;
            const auto view = FindActiveVertexView(vertexBuffers, submitArgs, layout.Binding);
            if (!view) return false;
            if (!StreamFitsRange(view->Size, layout.ArrayStride, command.FirstIndex, command.IndexCount)) return false;
            for (const auto& element : extraElements) {
                if (!StreamFitsRange(view->Size, layout.ArrayStride, element.FirstIndex, element.IndexCount)) return false;
            }
        }
    }
    uint32_t mask = visible.ElementMask & command.ValidElementMask;
    while (mask != 0) {
        const uint32_t bit = static_cast<uint32_t>(std::countr_zero(mask));
        mask &= mask - 1;
        const auto ordinal = static_cast<uint32_t>(std::popcount(command.ValidElementMask & ((uint32_t{1} << bit) - 1)));
        const auto first = !instances.empty() && visible.UseElementFirstInstance ? instances[ordinal].FirstInstance : visible.FirstInstance;
        const auto count = !instances.empty() && visible.UseElementInstanceCount ? instances[ordinal].InstanceCount : visible.InstanceCount;
        if (count != 0 && uint64_t{first} + count - 1 > std::numeric_limits<uint32_t>::max()) return false;
        if (!indexed && validateGeometry && !allowDeferredInstanceStream && count != 0 && !streamLayouts.empty()) {
            for (const auto& layout : streamLayouts) {
                if (layout.StepMode != gpu::VertexStepMode::Instance) continue;
                const auto view = FindActiveVertexView(vertexBuffers, submitArgs, layout.Binding);
                if (!view || !StreamFitsRange(view->Size, layout.ArrayStride, first, count)) return false;
            }
        }
    }
    return true;
}

bool ValidateStoredMeshDrawCommand(const MeshDrawCommand& command,
                                   const VisibleMeshDrawCommand& visibleCommand, const MeshPassSubmitArgs& submitArgs) noexcept {
    const auto storage = command.BindingStorage;
    const auto vertices = storage ? storage->GetVertexBuffers(command.VertexBuffersIndex) : nullptr;
    const auto payload = storage ? storage->GetPayload(command.PayloadIndex) : nullptr;
    const auto elements = storage ? storage->GetExtraElements(command.ExtraElementsIndex) : nullptr;
    if (((command.VertexBuffersIndex != kInvalidBindingIndex) && !vertices) || ((command.PayloadIndex != kInvalidBindingIndex) && !payload) ||
        ((command.ExtraElementsIndex != kInvalidBindingIndex) && !elements)) return false;
    if (vertices) {
        for (size_t index = 1; index < vertices->Bindings.size(); ++index) {
            if (vertices->Bindings[index - 1].Binding >= vertices->Bindings[index].Binding) return false;
        }
        for (size_t index = 1; index < vertices->StreamLayouts.size(); ++index) {
            if (vertices->StreamLayouts[index - 1].Binding >= vertices->StreamLayouts[index].Binding) return false;
        }
    }
    const std::span<const gpu::VertexBufferBinding> vertexBuffers =
        vertices ? std::span<const gpu::VertexBufferBinding>{vertices->Bindings.data(), vertices->Bindings.size()} : std::span<const gpu::VertexBufferBinding>{};
    const std::span<const MeshVertexStreamLayout> streamLayouts =
        vertices ? std::span<const MeshVertexStreamLayout>{vertices->StreamLayouts.data(), vertices->StreamLayouts.size()} : std::span<const MeshVertexStreamLayout>{};
    return ValidateMeshDrawCommandImpl(command, vertexBuffers, streamLayouts,
                                       payload ? std::span<const MeshShaderParameterBinding>{payload->ParameterSets.data(), payload->ParameterSets.size()} : std::span<const MeshShaderParameterBinding>{},
                                       payload && payload->PushConstants ? &*payload->PushConstants : nullptr,
                                       vertices ? vertices->InstanceStreamBinding : std::nullopt, submitArgs) &&
           ValidateElements(command, elements ? std::span<const MeshDrawElement>{elements->ExtraElements.data(), elements->ExtraElements.size()} : std::span<const MeshDrawElement>{},
                            elements ? std::span<const MeshDrawElementInstances>{elements->ElementInstances.data(), elements->ElementInstances.size()} : std::span<const MeshDrawElementInstances>{},
                            vertexBuffers, streamLayouts, visibleCommand, submitArgs);
}

}  // namespace

bool ValidateMeshBatchElements(const MeshBatch& batch, bool allowDeferredInstanceStream) noexcept {
    if (batch.Elements.size() > 32) return false;
    const auto firstActive = std::find_if(batch.Elements.begin(), batch.Elements.end(), [](const MeshBatchElement& element) {
        return element.IndexCount != 0 && element.InstanceCount != 0;
    });
    if (firstActive == batch.Elements.end()) return true;
    if (!batch.VertexInput || !ValidateVertexInput(*batch.VertexInput, allowDeferredInstanceStream)) return false;
    const bool indexed = firstActive->IndexBuffer != nullptr;
    for (const auto& element : batch.Elements) {
        if (element.IndexCount == 0 || element.InstanceCount == 0) continue;
        if (uint64_t{element.FirstInstance} + element.InstanceCount - 1 > std::numeric_limits<uint32_t>::max()) return false;
        if (indexed) {
            if (!element.IndexBuffer || !element.IndexBuffer->IsValid() ||
                (element.IndexStride != 2 && element.IndexStride != 4) ||
                element.IndexBufferOffset % element.IndexStride != 0) return false;
            const auto desc = element.IndexBuffer->GetDesc();
            if (!desc.Usage.HasFlag(gpu::BufferUse::Index) ||
                uint64_t{element.IndexBufferOffset} + (uint64_t{element.FirstIndex} + element.IndexCount) * element.IndexStride > desc.Size) return false;
        } else {
            if (element.IndexBuffer != nullptr || element.IndexBufferOffset != 0 || element.VertexOffset != 0) return false;
            for (const auto& layout : batch.VertexInput->Layouts) {
                const auto stream = std::find_if(batch.VertexInput->Streams.begin(), batch.VertexInput->Streams.end(), [&](const VertexInputStream& value) {
                    return value.Binding == layout.Binding;
                });
                if (layout.StepMode == gpu::VertexStepMode::Vertex) {
                    if (stream == batch.VertexInput->Streams.end() ||
                        !StreamFitsRange(stream->Size, layout.ArrayStride, element.FirstIndex, element.IndexCount)) return false;
                } else if (layout.StepMode == gpu::VertexStepMode::Instance) {
                    if (allowDeferredInstanceStream) continue;
                    if (stream == batch.VertexInput->Streams.end() ||
                        !StreamFitsRange(stream->Size, layout.ArrayStride, element.FirstInstance, element.InstanceCount)) return false;
                }
            }
        }
    }
    return true;
}

bool MeshDrawCommandBindingStorage::ValidateConstruction(const MeshDrawCommand& command, const MeshDrawCommandBindings& bindings, const VisibleMeshDrawCommand& visible, bool validateElements, bool allowDeferredInstanceStream) noexcept {
    const std::span<const gpu::VertexBufferBinding> vertexBuffers{bindings.VertexBuffers.data(), bindings.VertexBuffers.size()};
    const std::span<const MeshVertexStreamLayout> streamLayouts{bindings.VertexStreamLayouts.data(), bindings.VertexStreamLayouts.size()};
    return ValidateMeshDrawCommandImpl(command, vertexBuffers, streamLayouts, {bindings.ParameterSets.data(), bindings.ParameterSets.size()}, bindings.PushConstants ? &*bindings.PushConstants : nullptr, bindings.InstanceStreamBinding, {}, validateElements, allowDeferredInstanceStream) &&
           ValidateElements(command, {bindings.ExtraElements.data(), bindings.ExtraElements.size()}, {bindings.ElementInstances.data(), bindings.ElementInstances.size()}, vertexBuffers, streamLayouts, visible, {}, validateElements, allowDeferredInstanceStream);
}

bool ValidateMeshDrawCommand(const MeshDrawCommand& command, const MeshDrawCommandBindings& bindings,
                             const VisibleMeshDrawCommand& visibleCommand, const MeshPassSubmitArgs& submitArgs) noexcept {
    const std::span<const gpu::VertexBufferBinding> vertexBuffers{bindings.VertexBuffers.data(), bindings.VertexBuffers.size()};
    const std::span<const MeshVertexStreamLayout> streamLayouts{bindings.VertexStreamLayouts.data(), bindings.VertexStreamLayouts.size()};
    return ValidateSubmitArgs(submitArgs) &&
           ValidateMeshDrawCommandImpl(command, vertexBuffers, streamLayouts, {bindings.ParameterSets.data(), bindings.ParameterSets.size()}, bindings.PushConstants ? &*bindings.PushConstants : nullptr, bindings.InstanceStreamBinding, submitArgs) &&
           ValidateElements(command, {bindings.ExtraElements.data(), bindings.ExtraElements.size()}, {bindings.ElementInstances.data(), bindings.ElementInstances.size()}, vertexBuffers, streamLayouts, visibleCommand, submitArgs);
}

bool ValidateMeshDrawCommand(const MeshDrawCommand& command, const MeshDrawCommandBindingStorage& storage,
                             const VisibleMeshDrawCommand& visibleCommand, const MeshPassSubmitArgs& submitArgs) noexcept {
    return (!command.BindingStorage || command.BindingStorage.Get() == &storage) &&
           ValidateSubmitArgs(submitArgs) && ValidateStoredMeshDrawCommand(command, visibleCommand, submitArgs);
}

namespace {

bool BindMeshDrawCommandState(gpu::GraphicsCommandEncoder& encoder,
                              MeshDrawCommandStateCache& stateCache,
                              const MeshDrawCommand& command,
                              const MeshPassSubmitArgs& submitArgs) noexcept {
    const auto storage = command.BindingStorage;
    const bool pipelineChanged = stateCache.Pipeline.Get() != command.Pipeline.Get();
    if (pipelineChanged) {
        const auto newLayout = command.Pipeline->GetLayout();
        const bool sameLayout = stateCache.Pipeline != nullptr && newLayout != nullptr && stateCache.Layout.Get() == newLayout.Get();
        encoder.BindGraphicsPipelineState(command.Pipeline.Get());
        stateCache.OnPipelineChanged(command.Pipeline.Get(), newLayout, sameLayout);
        if (!sameLayout) {
            for (const auto& binding : submitArgs.ParameterSets) {
                encoder.BindShaderParameterSet(binding.GroupIndex, binding.ParameterSet.Get(), {binding.DynamicOffsets.data(), binding.DynamicOffsets.size()});
            }
        }
        if (submitArgs.InstanceStream) {
            stateCache.UpdateVertexBuffer(*submitArgs.InstanceStream);
            encoder.BindVertexBuffers(std::span{&*submitArgs.InstanceStream, 1});
        }
    }
    if (command.VertexBuffersIndex != kInvalidBindingIndex &&
        (stateCache.LastVertexStorage != storage || stateCache.LastVertexBuffersIndex != command.VertexBuffersIndex)) {
        const auto& vertices = *storage->GetVertexBuffers(command.VertexBuffersIndex);
        const std::span<const gpu::VertexBufferBinding> bindings{vertices.Bindings.data(), vertices.Bindings.size()};
        size_t runStart = bindings.size();
        for (size_t index = 0; index < bindings.size(); ++index) {
            const auto& binding = bindings[index];
            const bool dirty = (!submitArgs.InstanceStream || submitArgs.InstanceStream->Binding != binding.Binding) && stateCache.UpdateVertexBuffer(binding);
            if (runStart != bindings.size() && (!dirty || binding.Binding - bindings[index - 1].Binding != 1)) {
                encoder.BindVertexBuffers(bindings.subspan(runStart, index - runStart));
                runStart = bindings.size();
            }
            if (dirty && runStart == bindings.size()) runStart = index;
        }
        if (runStart != bindings.size()) encoder.BindVertexBuffers(bindings.subspan(runStart));
        stateCache.LastVertexStorage = storage;
        stateCache.LastVertexBuffersIndex = command.VertexBuffersIndex;
    }
    if (!stateCache.StencilReference || *stateCache.StencilReference != command.StencilReference) {
        encoder.SetStencilReference(command.StencilReference);
        stateCache.StencilReference = command.StencilReference;
    }
    if (command.PayloadIndex != kInvalidBindingIndex &&
        (stateCache.LastPayloadStorage != storage || stateCache.LastPayloadIndex != command.PayloadIndex)) {
        const auto payload = storage->GetPayload(command.PayloadIndex);
        if (payload) {
            for (const auto& binding : payload->ParameterSets) {
                if (stateCache.UpdateParameterSet(binding)) encoder.BindShaderParameterSet(binding.GroupIndex, binding.ParameterSet.Get(), {binding.DynamicOffsets.data(), binding.DynamicOffsets.size()});
            }
            if (payload->PushConstants) {
                const auto& push = *payload->PushConstants;
                if (!stateCache.BoundPushConstants ||
                    (stateCache.BoundPushConstants.Get() != &push && !SamePushConstants(*stateCache.BoundPushConstants, push))) {
                    if (!encoder.SetPushConstants(push.PushConstantId, push.Offset, {push.Data.data(), push.Data.size()})) return false;
                    stateCache.BoundPushConstants = &push;
                }
            }
        }
        stateCache.LastPayloadStorage = storage;
        stateCache.LastPayloadIndex = command.PayloadIndex;
    }
    return true;
}

bool ValidateCommandElementStructure(const MeshDrawCommand& command, Nullable<const MeshDrawCommandElements*> elements) noexcept {
    if (command.ExtraElementsIndex != kInvalidBindingIndex && !elements) return false;
    const std::span<const MeshDrawElement> extraElements =
        elements ? std::span<const MeshDrawElement>{elements->ExtraElements.data(), elements->ExtraElements.size()}
                 : std::span<const MeshDrawElement>{};
    const std::span<const MeshDrawElementInstances> instances =
        elements ? std::span<const MeshDrawElementInstances>{elements->ElementInstances.data(), elements->ElementInstances.size()}
                 : std::span<const MeshDrawElementInstances>{};
    if (command.ValidElementMask == 0 ||
        extraElements.size() + 1 != static_cast<size_t>(std::popcount(command.ValidElementMask)) ||
        (!instances.empty() && instances.size() != extraElements.size() + 1)) return false;
    uint32_t remaining = command.ValidElementMask & (command.ValidElementMask - 1);
    for (const auto& element : extraElements) {
        if (element.ElementIndex != static_cast<uint32_t>(std::countr_zero(remaining))) return false;
        remaining &= remaining - 1;
    }
    return true;
}

template <typename EmitFn>
bool ForEachExpandedMeshDrawElement(std::span<const VisibleMeshDrawCommand> visibleCommands, EmitFn&& emit) {
    if (visibleCommands.size() > std::numeric_limits<uint32_t>::max()) return false;
    for (size_t commandIndex = 0; commandIndex < visibleCommands.size(); ++commandIndex) {
        const auto& visible = visibleCommands[commandIndex];
        if (visible.InstanceCount == 0 && !visible.UseElementInstanceCount) continue;
        const auto commandPtr = visible.CachedCommand;
        if (!commandPtr) return false;
        const auto& command = *commandPtr;
        const auto storage = command.BindingStorage;
        const auto elements = (command.ExtraElementsIndex != kInvalidBindingIndex && storage)
                                  ? storage->GetExtraElements(command.ExtraElementsIndex)
                                  : nullptr;
        if (!ValidateCommandElementStructure(command, elements)) return false;
        uint32_t mask = visible.ElementMask & command.ValidElementMask;
        if (mask == 0) continue;
        while (mask != 0) {
            uint32_t bit = static_cast<uint32_t>(std::countr_zero(mask));
            mask &= mask - 1;
            const auto ordinal = static_cast<uint32_t>(std::popcount(command.ValidElementMask & ((uint32_t{1} << bit) - 1)));
            const MeshDrawElement primary{command.IndexBuffer, command.IndexCount, command.FirstIndex, command.VertexOffset, bit};
            const auto& element = ordinal == 0 ? primary : elements->ExtraElements[ordinal - 1];
            uint32_t firstInstance = visible.FirstInstance;
            uint32_t instanceCount = visible.InstanceCount;
            if (elements && !elements->ElementInstances.empty()) {
                if (visible.UseElementFirstInstance) firstInstance = elements->ElementInstances[ordinal].FirstInstance;
                if (visible.UseElementInstanceCount) instanceCount = elements->ElementInstances[ordinal].InstanceCount;
            }
            if (instanceCount == 0) continue;
            auto indexCount = element.IndexCount;
            auto nextOrdinal = ordinal;
            if (instanceCount == 1 && elements) {
                while (bit < 31 && (elements->ContiguousMergeMask & (uint32_t{1} << bit)) && (mask & (uint32_t{1} << (bit + 1)))) {
                    indexCount += elements->ExtraElements[nextOrdinal++].IndexCount;
                    mask &= ~(uint32_t{1} << ++bit);
                }
            }
            if (!emit(static_cast<uint32_t>(commandIndex), command, element, indexCount, instanceCount, firstInstance)) return false;
        }
    }
    return true;
}

bool CanBatchUseIndirect(const MeshPassIndirectBatch& batch, const gpu::GpuDeviceCapabilities& capabilities) noexcept {
    const bool deviceCanDrawIndirect = capabilities.Features.IndirectDraw && capabilities.Limits.MaxDrawIndirectCount >= 1;
    return deviceCanDrawIndirect && (!batch.HasNonZeroFirstInstance || capabilities.Features.IndirectFirstInstance);
}

}  // namespace

void PreparedMeshPassIndirectDraws::Clear() noexcept {
    Batches.clear();
    IndexedArguments.clear();
    NonIndexedArguments.clear();
}

bool SubmitMeshDrawCommands(gpu::GraphicsCommandEncoder& encoder,
                            std::span<const VisibleMeshDrawCommand> visibleCommands,
                            const MeshPassSubmitArgs& submitArgs) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.MeshDraw.Submit");
    RADRAY_PROFILE_PLOT("RadRay.MeshDraw.SubmitCommands", static_cast<int64_t>(visibleCommands.size()));
    if (!encoder.IsValid()) return false;
#if RADRAY_IS_DEBUG
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.MeshDraw.Validate");
        if (!ValidateSubmitArgs(submitArgs)) return false;
        for (const auto& visible : visibleCommands) {
            const auto command = visible.CachedCommand;
            if (!command || !ValidateStoredMeshDrawCommand(*command, visible, submitArgs)) return false;
        }
    }
#endif
    MeshDrawCommandStateCache stateCache;
    for (const auto& visible : visibleCommands) {
        if (visible.InstanceCount == 0 && !visible.UseElementInstanceCount) continue;
        const auto commandPtr = visible.CachedCommand;
        if (!commandPtr) return false;
        const auto& command = *commandPtr;
        uint32_t mask = visible.ElementMask & command.ValidElementMask;
        if (mask == 0) continue;
        const auto storage = command.BindingStorage;
        const auto elements = (command.ExtraElementsIndex != kInvalidBindingIndex) ? storage->GetExtraElements(command.ExtraElementsIndex) : nullptr;
        if (!BindMeshDrawCommandState(encoder, stateCache, command, submitArgs)) return false;
        bool boundCommandIndexBuffer = false;
        while (mask != 0) {
            uint32_t bit = static_cast<uint32_t>(std::countr_zero(mask));
            mask &= mask - 1;
            const auto ordinal = static_cast<uint32_t>(std::popcount(command.ValidElementMask & ((uint32_t{1} << bit) - 1)));
            const MeshDrawElement primary{command.IndexBuffer, command.IndexCount, command.FirstIndex, command.VertexOffset, bit};
            const auto& element = ordinal == 0 ? primary : elements->ExtraElements[ordinal - 1];
            uint32_t firstInstance = visible.FirstInstance;
            uint32_t instanceCount = visible.InstanceCount;
            if (elements && !elements->ElementInstances.empty()) {
                if (visible.UseElementFirstInstance) firstInstance = elements->ElementInstances[ordinal].FirstInstance;
                if (visible.UseElementInstanceCount) instanceCount = elements->ElementInstances[ordinal].InstanceCount;
            }
            if (instanceCount == 0) continue;
            if (element.IndexBuffer.Target != nullptr) {
                if (!(boundCommandIndexBuffer && elements && elements->AllElementsSharePrimaryIndexBuffer) &&
                    (!stateCache.IndexBuffer || !SameIndexBufferView(*stateCache.IndexBuffer, element.IndexBuffer))) {
                    encoder.BindIndexBuffer(element.IndexBuffer);
                    stateCache.IndexBuffer = element.IndexBuffer;
                }
                boundCommandIndexBuffer = true;
            }
            auto indexCount = element.IndexCount;
            auto nextOrdinal = ordinal;
            if (instanceCount == 1 && elements) {
                while (bit < 31 && (elements->ContiguousMergeMask & (uint32_t{1} << bit)) && (mask & (uint32_t{1} << (bit + 1)))) {
                    indexCount += elements->ExtraElements[nextOrdinal++].IndexCount;
                    mask &= ~(uint32_t{1} << ++bit);
                }
            }
            if (element.IndexBuffer.Target != nullptr) {
                encoder.DrawIndexed(indexCount, instanceCount, element.FirstIndex, element.VertexOffset, firstInstance);
            } else {
                encoder.Draw(indexCount, instanceCount, element.FirstIndex, firstInstance);
            }
        }
    }
    return true;
}

bool CountMeshPassIndirectDraws(std::span<const VisibleMeshDrawCommand> visibleCommands,
                                MeshPassIndirectDrawCounts& out) noexcept {
    out = {};
    const bool ok = ForEachExpandedMeshDrawElement(
        visibleCommands,
        [&](uint32_t, const MeshDrawCommand&, const MeshDrawElement& element, uint32_t, uint32_t, uint32_t) noexcept {
            if (element.IndexBuffer.Target != nullptr) {
                if (out.IndexedArgumentCount == std::numeric_limits<uint32_t>::max()) return false;
                ++out.IndexedArgumentCount;
            } else {
                if (out.NonIndexedArgumentCount == std::numeric_limits<uint32_t>::max()) return false;
                ++out.NonIndexedArgumentCount;
            }
            return true;
        });
    if (!ok) out = {};
    return ok;
}

bool BuildMeshPassIndirectDraws(std::span<const VisibleMeshDrawCommand> visibleCommands,
                                PreparedMeshPassIndirectDraws& out) {
    out.Clear();
    const bool ok = ForEachExpandedMeshDrawElement(
        visibleCommands,
        [&](uint32_t commandIndex, const MeshDrawCommand& command, const MeshDrawElement& element,
            uint32_t indexCount, uint32_t instanceCount, uint32_t firstInstance) {
            const bool indexed = element.IndexBuffer.Target != nullptr;
            if (indexed) {
                if (out.IndexedArguments.size() >= std::numeric_limits<uint32_t>::max()) return false;
            } else {
                if (out.NonIndexedArguments.size() >= std::numeric_limits<uint32_t>::max()) return false;
            }
            bool canMerge = false;
            if (!out.Batches.empty()) {
                const auto& prev = out.Batches.back();
                const auto& prevCommand = *visibleCommands[prev.CommandIndex].CachedCommand;
                const bool sameVertices =
                    (command.VertexBuffersIndex == kInvalidBindingIndex && prevCommand.VertexBuffersIndex == kInvalidBindingIndex) ||
                    (command.BindingStorage.Get() == prevCommand.BindingStorage.Get() && command.VertexBuffersIndex == prevCommand.VertexBuffersIndex);
                const bool samePayload =
                    (command.PayloadIndex == kInvalidBindingIndex && prevCommand.PayloadIndex == kInvalidBindingIndex) ||
                    (command.BindingStorage.Get() == prevCommand.BindingStorage.Get() && command.PayloadIndex == prevCommand.PayloadIndex);
                const bool sameIndex = (indexed == prev.Indexed) && (!indexed || SameIndexBufferView(element.IndexBuffer, prev.IndexBuffer));
                canMerge = command.Pipeline.Get() == prevCommand.Pipeline.Get() &&
                           command.StencilReference == prevCommand.StencilReference &&
                           sameVertices && samePayload && sameIndex;
            }
            const uint32_t argIndex = static_cast<uint32_t>(indexed ? out.IndexedArguments.size() : out.NonIndexedArguments.size());
            if (indexed) {
                out.IndexedArguments.push_back({indexCount, instanceCount, element.FirstIndex, element.VertexOffset, firstInstance});
            } else {
                out.NonIndexedArguments.push_back({indexCount, instanceCount, element.FirstIndex, firstInstance});
            }
            if (canMerge) {
                auto& batch = out.Batches.back();
                ++batch.ArgumentCount;
                batch.HasNonZeroFirstInstance |= (firstInstance != 0);
            } else {
                out.Batches.push_back({
                    .CommandIndex = commandIndex,
                    .Indexed = indexed,
                    .IndexBuffer = indexed ? element.IndexBuffer : gpu::IndexBufferView{},
                    .FirstArgument = argIndex,
                    .ArgumentCount = 1,
                    .HasNonZeroFirstInstance = firstInstance != 0});
            }
            return true;
        });
    if (!ok) out.Clear();
    return ok;
}

bool SubmitMeshPassIndirectDraws(gpu::GraphicsCommandEncoder& encoder,
                                 std::span<const VisibleMeshDrawCommand> visibleCommands,
                                 const PreparedMeshPassIndirectDraws& prepared,
                                 RDGBufferSlice indexedArgBuffer,
                                 RDGBufferSlice nonIndexedArgBuffer,
                                 const gpu::GpuDeviceCapabilities& capabilities,
                                 const MeshPassSubmitArgs& submitArgs) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.MeshDraw.SubmitIndirect");
    RADRAY_PROFILE_PLOT("RadRay.MeshDraw.SubmitCommands", static_cast<int64_t>(visibleCommands.size()));
    if (!encoder.IsValid()) return false;
#if RADRAY_IS_DEBUG
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.MeshDraw.Validate");
        if (!ValidateSubmitArgs(submitArgs)) return false;
        for (const auto& visible : visibleCommands) {
            const auto command = visible.CachedCommand;
            if (!command || !ValidateStoredMeshDrawCommand(*command, visible, submitArgs)) return false;
        }
    }
#endif
    for (const auto& batch : prepared.Batches) {
        if (batch.CommandIndex >= visibleCommands.size() || batch.ArgumentCount == 0) return false;
        if (!visibleCommands[batch.CommandIndex].CachedCommand) return false;
        const size_t cpuCount = batch.Indexed ? prepared.IndexedArguments.size() : prepared.NonIndexedArguments.size();
        if (uint64_t{batch.FirstArgument} + batch.ArgumentCount > cpuCount) return false;
        if (CanBatchUseIndirect(batch, capabilities)) {
            const RDGBufferSlice& slice = batch.Indexed ? indexedArgBuffer : nonIndexedArgBuffer;
            const uint64_t stride = batch.Indexed ? sizeof(gpu::DrawIndexedIndirectArguments) : sizeof(gpu::DrawIndirectArguments);
            if (!slice.Buffer || !slice.Buffer->IsValid()) return false;
            const auto desc = slice.Buffer->GetDesc();
            if (!desc.Usage.HasFlag(gpu::BufferUse::Indirect) || (slice.Offset % 4) != 0) return false;
            if (slice.Offset > desc.Size || slice.Size > desc.Size - slice.Offset) return false;
            if (uint64_t{batch.FirstArgument} + batch.ArgumentCount > slice.Size / stride) return false;
        }
    }

    MeshDrawCommandStateCache stateCache;
    for (const auto& batch : prepared.Batches) {
        const auto& command = *visibleCommands[batch.CommandIndex].CachedCommand;
        if (!BindMeshDrawCommandState(encoder, stateCache, command, submitArgs)) return false;
        if (batch.Indexed) {
            if (!stateCache.IndexBuffer || !SameIndexBufferView(*stateCache.IndexBuffer, batch.IndexBuffer)) {
                encoder.BindIndexBuffer(batch.IndexBuffer);
                stateCache.IndexBuffer = batch.IndexBuffer;
            }
        }
        if (CanBatchUseIndirect(batch, capabilities)) {
            const RDGBufferSlice& slice = batch.Indexed ? indexedArgBuffer : nonIndexedArgBuffer;
            const uint64_t stride = batch.Indexed ? sizeof(gpu::DrawIndexedIndirectArguments) : sizeof(gpu::DrawIndirectArguments);
            const uint32_t maxChunk = capabilities.Limits.MaxDrawIndirectCount;
            uint32_t consumed = 0;
            while (consumed < batch.ArgumentCount) {
                const uint32_t chunk = std::min(batch.ArgumentCount - consumed, maxChunk);
                const uint64_t byteOffset = slice.Offset + (uint64_t{batch.FirstArgument} + consumed) * stride;
                if (batch.Indexed) {
                    encoder.DrawIndexedIndirect(slice.Buffer.Get(), byteOffset, chunk);
                } else {
                    encoder.DrawIndirect(slice.Buffer.Get(), byteOffset, chunk);
                }
                consumed += chunk;
            }
        } else if (batch.Indexed) {
            for (uint32_t i = 0; i < batch.ArgumentCount; ++i) {
                const auto& arg = prepared.IndexedArguments[batch.FirstArgument + i];
                encoder.DrawIndexed(arg.IndexCount, arg.InstanceCount, arg.FirstIndex, arg.VertexOffset, arg.FirstInstance);
            }
        } else {
            for (uint32_t i = 0; i < batch.ArgumentCount; ++i) {
                const auto& arg = prepared.NonIndexedArguments[batch.FirstArgument + i];
                encoder.Draw(arg.VertexCount, arg.InstanceCount, arg.FirstVertex, arg.FirstInstance);
            }
        }
    }
    return true;
}

size_t MeshDrawCommandBindingStorage::GetVertexBufferCount() const noexcept {
    return _vertices.Entries.size() - _vertices.Free.size();
}

size_t MeshDrawCommandBindingStorage::GetPayloadCount() const noexcept {
    return _payloads.Entries.size() - _payloads.Free.size();
}

size_t MeshDrawCommandBindingStorage::GetExtraElementsCount() const noexcept {
    return _elements.size() - _freeElements.size();
}

std::span<const MeshDrawCommand> CachedPassMeshDrawList::GetCommands() const noexcept {
    return _commands;
}

const MeshDrawCommandBindingStorage& CachedPassMeshDrawList::GetBindings() const noexcept {
    return _bindings;
}

MeshPassSortMode CachedPassMeshDrawList::GetSortMode() const noexcept {
    return _sortMode;
}

}  // namespace radray
