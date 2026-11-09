#include <radray/runtime/mesh_draw_command.h>

#include <algorithm>
#include <bit>
#include <utility>
#include <limits>

#include <radray/profiler.h>

namespace radray {
namespace {

bool ValidateVertexBuffer(const render::VertexBufferView& view) noexcept {
    if (!view.Target || !view.Target->IsValid() || view.Size == 0 || view.Size > std::numeric_limits<uint32_t>::max()) return false;
    const auto desc = view.Target->GetDesc();
    return desc.Usage.HasFlag(render::BufferUse::Vertex) && view.Offset <= desc.Size && view.Size <= desc.Size - view.Offset;
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

bool SameVertexBufferView(const render::VertexBufferView& a, const render::VertexBufferView& b) noexcept {
    return a.Target == b.Target && a.Offset == b.Offset && a.Size == b.Size;
}

bool SameIndexBufferView(const render::IndexBufferView& a, const render::IndexBufferView& b) noexcept {
    return a.Target == b.Target && a.Offset == b.Offset && a.Stride == b.Stride;
}

bool SameParameterBinding(const MeshShaderParameterBinding& a, const MeshShaderParameterBinding& b) noexcept {
    return a.GroupIndex == b.GroupIndex && a.ParameterSet.Get() == b.ParameterSet.Get() && a.DynamicOffsets == b.DynamicOffsets;
}

struct MeshDrawCommandStateCache {
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    std::optional<render::IndexBufferView> IndexBuffer{};
    std::optional<uint8_t> StencilReference{};
    array<render::VertexBufferView, 32> BoundVertexSlots{};
    uint32_t BoundVertexMask{0};
    Nullable<const MeshDrawCommandBindingStorage*> LastVertexStorage{nullptr};
    uint32_t LastVertexBuffersIndex{kInvalidBindingIndex};
    Nullable<const MeshDrawCommandBindingStorage*> LastPayloadStorage{nullptr};
    uint32_t LastPayloadIndex{kInvalidBindingIndex};
    Nullable<const MeshPushConstantCommandBinding*> LastPushConstants{nullptr};
    InlineVector<MeshShaderParameterBinding, 4> CommandParameterSets;

    void OnPipelineChanged(render::GraphicsPipelineState* pipeline) noexcept {
        Pipeline = pipeline;
        BoundVertexMask = 0;
        LastVertexStorage = nullptr;
        LastVertexBuffersIndex = kInvalidBindingIndex;
        LastPayloadStorage = nullptr;
        LastPayloadIndex = kInvalidBindingIndex;
        LastPushConstants = nullptr;
        CommandParameterSets.clear();
    }

    bool UpdateVertexBuffer(const render::VertexBufferBinding& binding) noexcept {
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

}  // 匿名命名空间

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
    return hash;
}

bool SameBinding(const MeshVertexBufferBindings& a, const MeshVertexBufferBindings& b) noexcept {
    return a.InstanceStreamBinding == b.InstanceStreamBinding && a.Bindings.size() == b.Bindings.size() &&
           std::equal(a.Bindings.begin(), a.Bindings.end(), b.Bindings.begin(), [](const auto& x, const auto& y) {
               return x.Binding == y.Binding && SameVertexBufferView(x.View, y.View);
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

uint32_t BindingBucket(uint32_t index, uint32_t maxBucket) noexcept {
    if (index == kInvalidBindingIndex) return 0;
    return (1 + (index < maxBucket ? index : static_cast<uint32_t>(MixBindingHash(index) % maxBucket))) & maxBucket;
}

}  // 匿名命名空间

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
    if (!bindings.VertexBuffers.empty() || bindings.InstanceStreamBinding) {
        MeshVertexBufferBindings vertices{bindings.VertexBuffers, bindings.InstanceStreamBinding};
        std::sort(vertices.Bindings.begin(), vertices.Bindings.end(), [](const auto& a, const auto& b) { return a.Binding < b.Binding; });
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
    _pipelineBuckets.clear();
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
uint32_t MeshDrawCommandBindingStorage::GetStateSortKey(const MeshDrawCommand& command) {
    const auto [pipeline, inserted] = _pipelineBuckets.try_emplace(command.Pipeline.Get(), static_cast<uint32_t>(_pipelineBuckets.size()));
    return (BindingBucket(pipeline->second, 255) << 24) | (BindingBucket(command.PayloadIndex, 4095) << 12) | BindingBucket(command.VertexBuffersIndex, 4095);
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
    InlineVector<render::VertexAttribute, 8> attributes;
    for (const auto& attribute : input.Attributes) {
        attributes.push_back({attribute.BufferBinding, attribute.Offset, attribute.Semantic,
                              attribute.SemanticIndex, attribute.Format, attribute.Location});
    }
    if (!render::ValidateVertexInputState({input.Layouts, {attributes.data(), attributes.size()}})) return false;
    size_t instanceLayouts = 0;
    size_t deferredStreams = 0;
    for (const auto& layout : input.Layouts) {
        if (layout.StepMode != render::VertexStepMode::Vertex && layout.StepMode != render::VertexStepMode::Instance) return false;
        if (layout.StepMode == render::VertexStepMode::Instance) ++instanceLayouts;
        const auto stream = std::find_if(input.Streams.begin(), input.Streams.end(), [&](const VertexInputStream& value) {
            return value.Binding == layout.Binding;
        });
        if (stream == input.Streams.end()) {
            if (allowDeferredInstanceStream && layout.StepMode == render::VertexStepMode::Instance) {
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
            std::none_of(input.Layouts.begin(), input.Layouts.end(), [&](const render::VertexBufferLayout& layout) { return layout.Binding == stream.Binding; })) return false;
        for (size_t previous = 0; previous < index; ++previous) {
            if (input.Streams[previous].Binding == stream.Binding) return false;
        }
    }
    return true;
}

namespace {

// 调用方已经校验整个 Pass 共用的参数。
bool ValidateMeshDrawCommandImpl(const MeshDrawCommand& command, std::span<const render::VertexBufferBinding> vertexBuffers,
                                 std::span<const MeshShaderParameterBinding> parameterSets,
                                 Nullable<const MeshPushConstantCommandBinding*> pushConstants, std::optional<uint32_t> instanceStreamBinding,
                                 const MeshPassSubmitArgs& submitArgs, bool validateGeometry = true) noexcept {
    const auto& indices = command.IndexBuffer;
    if (!command.Pipeline || !command.Pipeline->IsValid()) return false;
    if (validateGeometry) {
        if (!indices.Target || !indices.Target->IsValid() || command.IndexCount == 0 ||
            (indices.Stride != 2 && indices.Stride != 4) || indices.Offset % indices.Stride != 0) return false;
        const auto desc = indices.Target->GetDesc();
        const uint64_t end = uint64_t{indices.Offset} + (uint64_t{command.FirstIndex} + command.IndexCount) * indices.Stride;
        if (!desc.Usage.HasFlag(render::BufferUse::Index) || end > desc.Size) return false;
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
    if (instanceStreamBinding && !hasDefaultInstanceStream &&
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
                      std::span<const MeshDrawElementInstances> instances, const VisibleMeshDrawCommand& visible, bool validateGeometry = true) noexcept {
    if (command.ValidElementMask == 0 || extraElements.size() + 1 != static_cast<size_t>(std::popcount(command.ValidElementMask)) ||
        (!instances.empty() && instances.size() != extraElements.size() + 1)) return false;
    uint32_t remaining = command.ValidElementMask & (command.ValidElementMask - 1);
    for (const auto& element : extraElements) {
        if (element.ElementIndex != static_cast<uint32_t>(std::countr_zero(remaining))) return false;
        remaining &= remaining - 1;
        if (!validateGeometry) continue;
        const auto& indices = element.IndexBuffer;
        if (!indices.Target || !indices.Target->IsValid() || element.IndexCount == 0 ||
            (indices.Stride != 2 && indices.Stride != 4) || indices.Offset % indices.Stride != 0) return false;
        const auto desc = indices.Target->GetDesc();
        if (!desc.Usage.HasFlag(render::BufferUse::Index) ||
            uint64_t{indices.Offset} + (uint64_t{element.FirstIndex} + element.IndexCount) * indices.Stride > desc.Size) return false;
    }
    for (const auto& instance : instances) {
        if (instance.InstanceCount != 0 && uint64_t{instance.FirstInstance} + instance.InstanceCount - 1 > std::numeric_limits<uint32_t>::max()) return false;
    }
    uint32_t mask = visible.ElementMask & command.ValidElementMask;
    while (mask != 0) {
        const uint32_t bit = static_cast<uint32_t>(std::countr_zero(mask));
        mask &= mask - 1;
        const auto ordinal = static_cast<uint32_t>(std::popcount(command.ValidElementMask & ((uint32_t{1} << bit) - 1)));
        const auto first = !instances.empty() && visible.UseElementFirstInstance ? instances[ordinal].FirstInstance : visible.FirstInstance;
        const auto count = !instances.empty() && visible.UseElementInstanceCount ? instances[ordinal].InstanceCount : visible.InstanceCount;
        if (count != 0 && uint64_t{first} + count - 1 > std::numeric_limits<uint32_t>::max()) return false;
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
    }
    return ValidateMeshDrawCommandImpl(command,
                                       vertices ? std::span<const render::VertexBufferBinding>{vertices->Bindings.data(), vertices->Bindings.size()} : std::span<const render::VertexBufferBinding>{},
                                       payload ? std::span<const MeshShaderParameterBinding>{payload->ParameterSets.data(), payload->ParameterSets.size()} : std::span<const MeshShaderParameterBinding>{},
                                       payload && payload->PushConstants ? &*payload->PushConstants : nullptr,
                                       vertices ? vertices->InstanceStreamBinding : std::nullopt, submitArgs) &&
           ValidateElements(command, elements ? std::span<const MeshDrawElement>{elements->ExtraElements.data(), elements->ExtraElements.size()} : std::span<const MeshDrawElement>{},
                            elements ? std::span<const MeshDrawElementInstances>{elements->ElementInstances.data(), elements->ElementInstances.size()} : std::span<const MeshDrawElementInstances>{}, visibleCommand);
}

}  // 匿名命名空间

bool MeshDrawCommandBindingStorage::ValidateConstruction(const MeshDrawCommand& command, const MeshDrawCommandBindings& bindings, const VisibleMeshDrawCommand& visible, bool validateElements) noexcept {
    return ValidateMeshDrawCommandImpl(command, {bindings.VertexBuffers.data(), bindings.VertexBuffers.size()}, {bindings.ParameterSets.data(), bindings.ParameterSets.size()}, bindings.PushConstants ? &*bindings.PushConstants : nullptr, bindings.InstanceStreamBinding, {}, validateElements) &&
           ValidateElements(command, {bindings.ExtraElements.data(), bindings.ExtraElements.size()}, {bindings.ElementInstances.data(), bindings.ElementInstances.size()}, visible, validateElements);
}

bool ValidateMeshDrawCommand(const MeshDrawCommand& command, const MeshDrawCommandBindings& bindings,
                             const VisibleMeshDrawCommand& visibleCommand, const MeshPassSubmitArgs& submitArgs) noexcept {
    return ValidateSubmitArgs(submitArgs) && ValidateMeshDrawCommandImpl(command, {bindings.VertexBuffers.data(), bindings.VertexBuffers.size()}, {bindings.ParameterSets.data(), bindings.ParameterSets.size()}, bindings.PushConstants ? &*bindings.PushConstants : nullptr, bindings.InstanceStreamBinding, submitArgs) &&
           ValidateElements(command, {bindings.ExtraElements.data(), bindings.ExtraElements.size()}, {bindings.ElementInstances.data(), bindings.ElementInstances.size()}, visibleCommand);
}

bool ValidateMeshDrawCommand(const MeshDrawCommand& command, const MeshDrawCommandBindingStorage& storage,
                             const VisibleMeshDrawCommand& visibleCommand, const MeshPassSubmitArgs& submitArgs) noexcept {
    return (!command.BindingStorage || command.BindingStorage.Get() == &storage) &&
           ValidateSubmitArgs(submitArgs) && ValidateStoredMeshDrawCommand(command, visibleCommand, submitArgs);
}

bool SubmitMeshDrawCommands(render::GraphicsCommandEncoder& encoder,
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
        const bool pipelineChanged = stateCache.Pipeline.Get() != command.Pipeline.Get();
        if (pipelineChanged) {
            encoder.BindGraphicsPipelineState(command.Pipeline.Get());
            stateCache.OnPipelineChanged(command.Pipeline.Get());
            for (const auto& binding : submitArgs.ParameterSets) {
                encoder.BindShaderParameterSet(binding.GroupIndex, binding.ParameterSet.Get(), {binding.DynamicOffsets.data(), binding.DynamicOffsets.size()});
            }
            if (submitArgs.InstanceStream) {
                stateCache.UpdateVertexBuffer(*submitArgs.InstanceStream);
                encoder.BindVertexBuffers(std::span{&*submitArgs.InstanceStream, 1});
            }
        }
        if (stateCache.LastVertexStorage != storage || stateCache.LastVertexBuffersIndex != command.VertexBuffersIndex) {
            if (command.VertexBuffersIndex != kInvalidBindingIndex) {
                const auto& vertices = *storage->GetVertexBuffers(command.VertexBuffersIndex);
                const std::span<const render::VertexBufferBinding> bindings{vertices.Bindings.data(), vertices.Bindings.size()};
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
            }
            stateCache.LastVertexStorage = storage;
            stateCache.LastVertexBuffersIndex = command.VertexBuffersIndex;
        }
        if (!stateCache.StencilReference || *stateCache.StencilReference != command.StencilReference) {
            encoder.SetStencilReference(command.StencilReference);
            stateCache.StencilReference = command.StencilReference;
        }
        if (stateCache.LastPayloadStorage != storage || stateCache.LastPayloadIndex != command.PayloadIndex) {
            const auto payload = command.PayloadIndex != kInvalidBindingIndex ? storage->GetPayload(command.PayloadIndex) : nullptr;
            if (payload) {
                for (const auto& binding : payload->ParameterSets) {
                    if (stateCache.UpdateParameterSet(binding)) encoder.BindShaderParameterSet(binding.GroupIndex, binding.ParameterSet.Get(), {binding.DynamicOffsets.data(), binding.DynamicOffsets.size()});
                }
            }
            stateCache.LastPushConstants = payload && payload->PushConstants ? &*payload->PushConstants : nullptr;
            stateCache.LastPayloadStorage = storage;
            stateCache.LastPayloadIndex = command.PayloadIndex;
        }
        if (stateCache.LastPushConstants) {
            const auto& push = *stateCache.LastPushConstants;
            if (!encoder.SetPushConstants(push.PushConstantId, push.Offset, {push.Data.data(), push.Data.size()})) return false;
        }
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
            if (!(boundCommandIndexBuffer && elements && elements->AllElementsSharePrimaryIndexBuffer) &&
                (!stateCache.IndexBuffer || !SameIndexBufferView(*stateCache.IndexBuffer, element.IndexBuffer))) {
                encoder.BindIndexBuffer(element.IndexBuffer);
                stateCache.IndexBuffer = element.IndexBuffer;
            }
            boundCommandIndexBuffer = true;
            auto indexCount = element.IndexCount;
            auto nextOrdinal = ordinal;
            if (instanceCount == 1 && elements) {
                while (bit < 31 && (elements->ContiguousMergeMask & (uint32_t{1} << bit)) && (mask & (uint32_t{1} << (bit + 1)))) {
                    indexCount += elements->ExtraElements[nextOrdinal++].IndexCount;
                    mask &= ~(uint32_t{1} << ++bit);
                }
            }
            encoder.DrawIndexed(indexCount, instanceCount, element.FirstIndex, element.VertexOffset, firstInstance);
        }
    }
    return true;
}

}  // namespace radray
