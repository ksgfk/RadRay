#include <radray/runtime/render_framework/frame_draw_resources.h>

#include "renderer_list_preparation.h"

#include <algorithm>
#include <cstring>
#include <radray/hash.h>
#include <radray/profiler.h>
#include <radray/runtime/shader_program.h>

namespace radray {

namespace {
size_t IdentityHash(const FrameCBufferIdentity& id) noexcept {
    HashCode hash;
    hash.Add(reinterpret_cast<uintptr_t>(id.Source.Get()));
    hash.Add(reinterpret_cast<uintptr_t>(id.WireType.Get()));
    hash.Add(id.Context);
    hash.Add(id.Generation);
    hash.Add(id.Revision);
    hash.Add(id.HistoryRevision);
    hash.Add(reinterpret_cast<uintptr_t>(id.HistoryOwner.Get()));
    return hash.ToHashCode();
}
size_t NativeHash(uint32_t domain, uint32_t row, uint64_t generation, uint32_t group) noexcept {
    HashCode hash;
    hash.Add(domain);
    hash.Add(row);
    hash.Add(generation);
    hash.Add(group);
    return hash.ToHashCode();
}
}  // namespace

void FrameDrawResources::Lookup::Reset() noexcept {
    _count = 0;
    if (++_epoch == 0) {
        for (auto& slot : _slots) slot.Epoch = 0;
        ++_epoch;
    }
}
void FrameDrawResources::Lookup::Insert(size_t hash, uint32_t index) {
    if ((_count + 1) * 2 > _slots.size()) {
        auto previous = std::move(_slots);
        _slots.resize(std::max<size_t>(32, previous.size() * 2));
        _count = 0;
        for (const auto& slot : previous)
            if (slot.Epoch == _epoch) Insert(slot.Hash, slot.Index);
    }
    size_t slot = hash & (_slots.size() - 1);
    while (_slots[slot].Epoch == _epoch) slot = (slot + 1) & (_slots.size() - 1);
    _slots[slot] = {_epoch, hash, index};
    ++_count;
}

FrameDrawBindingId FrameDrawResources::InternBinding(std::span<const FrameShaderGroupId> groups) {
    if (groups.empty() || _drawBindings.size() >= UINT32_MAX) return {};
    HashCode hash;
    for (auto group : groups) {
        if (group.Value >= _readyGroups.size()) return {};
        hash.Add(group.Value);
    }
    const auto code = hash.ToHashCode();
    const auto found = _bindingLookup.Find(code, [&](uint32_t index) {
        const auto& candidate = _drawBindings[index];
        return candidate.size() == groups.size() && std::equal(candidate.begin(), candidate.end(), groups.begin());
    });
    if (found != UINT32_MAX) return {found};
    const FrameDrawBindingId id{static_cast<uint32_t>(_drawBindings.size())};
    auto& stored = _drawBindings.emplace_back();
    for (auto group : groups) stored.push_back(group);
    _bindingLookup.Insert(code, id.Value);
    return id;
}

FrameCBufferIdentity FrameDrawResources::InternValues(const void* wireType, std::span<const byte> bytes) {
    if (bytes.empty() || _activeValues >= UINT32_MAX) return {};
    HashCode hash;
    hash.Add(reinterpret_cast<uintptr_t>(wireType));
    for (const auto value : bytes) hash.Add(std::to_integer<uint8_t>(value));
    const auto code = hash.ToHashCode();
    uint32_t index = _valueLookup.Find(code, [&](uint32_t candidate) {
        const auto& value = _values[candidate];
        return value.Wire.Get() == wireType && value.Bytes.size() == bytes.size() && std::equal(value.Bytes.begin(), value.Bytes.end(), bytes.begin());
    });
    if (index == UINT32_MAX) {
        index = static_cast<uint32_t>(_activeValues++);
        if (index == _values.size()) _values.emplace_back();
        _values[index].Wire = wireType;
        _values[index].Bytes.assign(bytes.begin(), bytes.end());
        _valueLookup.Insert(code, index);
    }
    return {this, wireType, uint64_t{index} + 1};
}

bool FrameDrawResources::HasCBufferValues(FrameCBufferIdentity identity, uint32_t row) const noexcept {
    const auto index = _domainLookup.Find(IdentityHash(identity), [&](uint32_t candidate) { return _sharedBufferDomains[candidate].Identity == identity; });
    return index != UINT32_MAX && row < _sharedBufferDomains[index].Rows.size() && _sharedBufferDomains[index].Rows[row].Epoch == _bufferEpoch;
}

size_t FrameDrawResources::GetCacheCapacityBytes() const noexcept {
    size_t bytes = _setLookup.CapacityBytes() + _domainLookup.CapacityBytes() + _nativeLookup.CapacityBytes() + _bindingLookup.CapacityBytes() + _valueLookup.CapacityBytes();
    bytes += _setCache.capacity() * sizeof(CachedSet) + _sharedBufferDomains.capacity() * sizeof(SharedBufferDomain);
    bytes += _nativeGroups.capacity() * sizeof(NativeGroup) + _readyGroups.capacity() * sizeof(PreparedShaderGroup);
    bytes += _drawBindings.capacity() * sizeof(InlineVector<FrameShaderGroupId, 3>) + _values.capacity() * sizeof(StoredValues);
    bytes += _dynamicOnlySets.capacity() * sizeof(DynamicOnlySet) + _bindingScratch.capacity() * sizeof(FrameBufferBinding);
    bytes += _sets.capacity() * sizeof(unique_ptr<render::ShaderParameterSet>);
    bytes += _keyScratch.Buffers.capacity() * sizeof(FrameBufferBinding) + _keyScratch.Textures.capacity() * sizeof(TextureBinding) + _keyScratch.Samplers.capacity() * sizeof(SamplerBinding);
    for (const auto& entry : _setCache) bytes += entry.Key.Buffers.capacity() * sizeof(FrameBufferBinding) + entry.Key.Textures.capacity() * sizeof(TextureBinding) + entry.Key.Samplers.capacity() * sizeof(SamplerBinding);
    for (const auto& domain : _sharedBufferDomains) bytes += domain.Rows.capacity() * sizeof(SharedBufferSlice);
    for (const auto& value : _values) bytes += value.Bytes.capacity();
    for (const auto& value : _drawBindings)
        if (value.capacity() > value.inline_capacity) bytes += value.capacity() * sizeof(FrameShaderGroupId);
    for (const auto& value : _readyGroups)
        if (value.DynamicOffsets.capacity() > value.DynamicOffsets.inline_capacity) bytes += value.DynamicOffsets.capacity() * sizeof(render::ShaderParameterDynamicOffset);
    if (_preparation) bytes += _preparation->CapacityBytes();
    return bytes;
}

PreparedRendererList::Workspace& FrameDrawResources::GetPreparationWorkspace() const {
    if (!_preparation) _preparation = make_unique<PreparedRendererList::Workspace>();
    return *_preparation;
}

FrameDrawResources::FrameDrawResources(render::Device* device, DynamicCBufferArena::Descriptor descriptor)
    : _device(device), _descriptor(std::move(descriptor)) {
    _descriptor.Alignment = std::max(_descriptor.Alignment, std::max<uint64_t>(device->GetDetail().CBufferAlignment, 1));
}

FrameDrawResources::~FrameDrawResources() noexcept = default;

void FrameDrawResources::ClearSets() noexcept {
    if (_preparation) _preparation->BeginFrame();
    _readyGroups.clear();
    _activeSets = 0;
    _setLookup.Reset();
    _domainLookup.Reset();
    _nativeLookup.Reset();
    _bindingLookup.Reset();
    _valueLookup.Reset();
    _nativeGroups.clear();
    _drawBindings.clear();
    _activeValues = 0;
    _dynamicOnlySets.clear();
    _lastDynamicOnlySet = 0;
    _sets.clear();
    _activeBufferDomains = 0;
    if (++_bufferEpoch == 0) {
        _sharedBufferDomains.clear();
        ++_bufferEpoch;
    }
}

bool FrameDrawResources::BeginFrame(HostWriteBatch& hostWrites) noexcept {
    ClearSets();
    _stats = {};
    if (!_arena || _hostWrites.Get() != &hostWrites) {
        _arena = make_unique<DynamicCBufferArena>(_device, &hostWrites, _descriptor);
        _hostWrites = &hostWrites;
    } else {
        _arena->Reset();
    }
    return _arena->IsValid();
}

size_t FrameDrawResources::FrameSetKeyHash::operator()(const FrameSetKey& key) const noexcept {
    size_t result = std::hash<render::PipelineLayout*>{}(key.Layout);
    const auto mix = [&](size_t value) { result ^= value + size_t{0x9e3779b9u} + (result << 6) + (result >> 2); };
    mix(key.Group);
    for (const auto& buffer : key.Buffers) {
        mix(buffer.BufferIndex);
        mix(std::hash<render::Buffer*>{}(buffer.Value.Target));
        mix(std::hash<uint64_t>{}(buffer.Value.Range.Offset));
        mix(std::hash<uint64_t>{}(buffer.Value.Range.Size));
    }
    for (const auto& texture : key.Textures) {
        mix(texture.Parameter);
        mix(texture.Element);
        mix(std::hash<render::TextureView*>{}(texture.View));
    }
    for (const auto& sampler : key.Samplers) {
        mix(sampler.Parameter);
        mix(sampler.Element);
        mix(std::hash<render::Sampler*>{}(sampler.Sampler));
    }
    return result;
}

const ShaderParameterGroupRecipe& FrameDrawResources::GetRecipe(ShaderProgram& program, uint32_t group) {
    const auto before = program.GetParameterGroupRecipeCount();
    const auto& recipe = program.GetOrCreateParameterGroupRecipe(group);
    _stats.RecipeBuilds += program.GetParameterGroupRecipeCount() - before;
    return recipe;
}

bool FrameDrawResources::UploadBuffer(
    const ShaderParameterBufferLayout& buffer, uint32_t index, bool dynamic, std::span<const byte> bytes,
    vector<FrameBufferBinding>& bindings, PreparedShaderGroup& out) {
    if (bytes.empty() || bytes.size() != buffer.Size) return false;
    auto reservation = _arena->Reserve(bytes.size());
    if (!reservation.IsValid()) return false;
    std::memcpy(reservation.Data(), bytes.data(), bytes.size());
    _stats.BufferBytesCopied += bytes.size();
    const auto allocation = reservation.Commit(bytes.size());
    if (!allocation.IsValid() || allocation.Offset > std::numeric_limits<uint32_t>::max()) return false;
    bindings.push_back({index, {allocation.Target, {dynamic ? 0 : allocation.Offset, buffer.Size}}});
    if (dynamic) out.DynamicOffsets.push_back({buffer.Binding, static_cast<uint32_t>(allocation.Offset)});
    return true;
}

std::optional<PreparedShaderGroup> FrameDrawResources::PrepareSharedCBufferGroup(
    ShaderProgram& program, uint32_t group, FrameCBufferIdentity identity, uint32_t row, std::span<const byte> bytes) {
    const auto id = PrepareSharedCBufferGroupId(program, group, identity, row, bytes);
    if (!id.IsValid()) return std::nullopt;
    return GetGroup(id);
}

FrameShaderGroupId FrameDrawResources::PrepareSharedCBufferGroupId(
    ShaderProgram& program, uint32_t group, FrameCBufferIdentity identity, uint32_t row, std::span<const byte> bytes) {
    const auto& recipe = GetRecipe(program, group);
    if (recipe.TextureCount != 0 || recipe.SamplerCount != 0) return {};
    return PrepareGroupId(program, group, identity, row, bytes);
}

FrameShaderGroupId FrameDrawResources::PrepareGroupId(
    ShaderProgram& program, uint32_t group, FrameCBufferIdentity identity, uint32_t row, std::span<const byte> bytes,
    std::span<const MaterialTextureFrameData> textures, std::span<const MaterialSamplerFrameData> samplers) {
    if (!_arena || program.GetDevice() != _device || !identity.Source || !identity.WireType ||
        bytes.size() > UINT32_MAX || row == UINT32_MAX) return {};
    const auto domainHash = IdentityHash(identity);
    uint32_t domainIndex = _domainLookup.Find(domainHash, [&](uint32_t index) { return _sharedBufferDomains[index].Identity == identity; });
    if (domainIndex == UINT32_MAX) {
        if (_activeBufferDomains >= UINT32_MAX) return {};
        domainIndex = static_cast<uint32_t>(_activeBufferDomains++);
        if (domainIndex == _sharedBufferDomains.size()) _sharedBufferDomains.emplace_back();
        _sharedBufferDomains[domainIndex].Identity = identity;
        _domainLookup.Insert(domainHash, domainIndex);
    }
    auto& domain = _sharedBufferDomains[domainIndex];
    const auto nativeHash = NativeHash(domainIndex, row, program.GetGeneration(), group);
    const auto found = _nativeLookup.Find(nativeHash, [&](uint32_t index) {
        const auto& entry = _nativeGroups[index];
        return entry.ProgramGeneration == program.GetGeneration() && entry.Group == group && entry.Domain == domainIndex && entry.Row == row;
    });
    if (found != UINT32_MAX) {
        const auto& native = _nativeGroups[found];
        if (textures.size() != native.TextureCount || samplers.size() != native.SamplerCount) return {};
        if (!bytes.empty() && (row >= domain.Rows.size() || domain.Rows[row].Size != bytes.size())) return {};
        ++_stats.SharedGroupHits;
        ++_stats.SharedBufferHits;
        return _nativeGroups[found].Id;
    }
    const auto& recipe = GetRecipe(program, group);
    if (recipe.Buffers.size() != 1 || textures.size() != recipe.TextureCount || samplers.size() != recipe.SamplerCount) return {};
    const auto& entry = recipe.Buffers.front();
    const auto& buffer = program.GetParameterLayout().Buffers()[entry.Index];
    if ((!bytes.empty() && bytes.size() != buffer.Size) || _readyGroups.size() >= UINT32_MAX) return {};
    ++_stats.GroupPreparations;
    if (row >= domain.Rows.size()) domain.Rows.resize(size_t{row} + 1);
    auto& slice = domain.Rows[row];
    if (slice.Epoch != _bufferEpoch) {
        if (bytes.empty()) return {};
        auto reservation = _arena->Reserve(bytes.size());
        if (!reservation.IsValid()) return {};
        std::memcpy(reservation.Data(), bytes.data(), bytes.size());
        _stats.BufferBytesCopied += bytes.size();
        const auto allocation = reservation.Commit(bytes.size());
        if (!allocation.IsValid() || allocation.Offset > UINT32_MAX) return {};
        slice = {_bufferEpoch, allocation.Target, static_cast<uint32_t>(allocation.Offset), static_cast<uint32_t>(bytes.size())};
        ++_stats.SharedBufferUploads;
    } else {
        if (slice.Size != buffer.Size) return {};
        ++_stats.SharedBufferHits;
    }
    if (_failNextGroup) {
        _failNextGroup = false;
        return {};
    }
    PreparedShaderGroup result;
    result.Group = group;
    if (entry.Dynamic) result.DynamicOffsets.push_back({buffer.Binding, slice.Offset});
    const FrameBufferBinding binding{entry.Index, {slice.Target.Get(), {entry.Dynamic ? 0 : slice.Offset, slice.Size}}};
    result.Set = PrepareSetForGroup(program, group, recipe, std::span{&binding, 1}, textures, samplers);
    if (!result.Set) return {};
    const FrameShaderGroupId id{static_cast<uint32_t>(_readyGroups.size())};
    _readyGroups.push_back(std::move(result));
    const auto nativeIndex = static_cast<uint32_t>(_nativeGroups.size());
    _nativeGroups.push_back({program.GetGeneration(), group, domainIndex, row, id, recipe.TextureCount, recipe.SamplerCount});
    _nativeLookup.Insert(nativeHash, nativeIndex);
    return id;
}

std::optional<PreparedShaderGroup> FrameDrawResources::PrepareGroup(
    ShaderProgram& program, uint32_t group, const ShaderParameterStorage& parameters,
    std::span<const MaterialTextureFrameData> textures, std::span<const MaterialSamplerFrameData> samplers) {
#if defined(RADRAY_RUNTIME_DETAILED_PROFILING)
    RADRAY_PROFILE_SCOPE_N("PrepareGroup");
#endif
    if (!_arena || parameters.GetLayout() != &program.GetParameterLayout()) return std::nullopt;
    ++_stats.GroupPreparations;
    auto& bindings = _bindingScratch;
    bindings.clear();
    const auto buffers = program.GetParameterLayout().Buffers();
    const auto& recipe = GetRecipe(program, group);
    PreparedShaderGroup result;
    result.Group = group;
    for (const auto& entry : recipe.Buffers)
        if (!UploadBuffer(buffers[entry.Index], entry.Index, entry.Dynamic, parameters.GetBufferData(entry.Index), bindings, result))
            return std::nullopt;
    result.Set = PrepareSetForGroup(program, group, recipe, bindings, textures, samplers);
    if (!result.Set) return std::nullopt;
    return result;
}

std::optional<PreparedShaderGroup> FrameDrawResources::PrepareGroup(
    ShaderProgram& program, uint32_t group, std::span<const byte> bufferBytes,
    std::span<const MaterialTextureFrameData> textures, std::span<const MaterialSamplerFrameData> samplers) {
#if defined(RADRAY_RUNTIME_DETAILED_PROFILING)
    RADRAY_PROFILE_SCOPE_N("PrepareGroup");
#endif
    if (!_arena) return std::nullopt;
    ++_stats.GroupPreparations;
    auto& bindings = _bindingScratch;
    bindings.clear();
    const auto& recipe = GetRecipe(program, group);
    // One span describes one cbuffer, so multi-buffer groups stay on the named storage path.
    if (recipe.Buffers.size() != 1) return std::nullopt;
    const auto& entry = recipe.Buffers.front();
    PreparedShaderGroup result;
    result.Group = group;
    if (!UploadBuffer(program.GetParameterLayout().Buffers()[entry.Index], entry.Index, entry.Dynamic, bufferBytes, bindings, result))
        return std::nullopt;
    result.Set = PrepareSetForGroup(program, group, recipe, bindings, textures, samplers);
    if (!result.Set) return std::nullopt;
    return result;
}

Nullable<render::ShaderParameterSet*> FrameDrawResources::PrepareSetForGroup(
    ShaderProgram& program, uint32_t group, const ShaderParameterGroupRecipe& recipe, std::span<const FrameBufferBinding> buffers,
    std::span<const MaterialTextureFrameData> textures, std::span<const MaterialSamplerFrameData> samplers) {
    // Fast path: a group made only of dynamic constant buffers (the common view/object case) binds every
    // buffer at offset 0 of its arena block, so the set is fully determined by (layout, group, blocks).
    // Skip the generic key construction and hash lookup that PrepareSet performs per call.
    const bool pureDynamic = textures.empty() && samplers.empty() && recipe.TextureCount == 0 && recipe.SamplerCount == 0 &&
                             !buffers.empty() && buffers.size() <= kDynamicOnlyTargets &&
                             std::all_of(recipe.Buffers.begin(), recipe.Buffers.end(), [](const auto& entry) { return entry.Dynamic; });
    if (!pureDynamic) return PrepareSet(program, group, buffers, textures, samplers);
    auto* layout = program.GetPipelineLayout();
    const auto matches = [&](const DynamicOnlySet& entry) {
        if (entry.Layout != layout || entry.Group != group || entry.Count != buffers.size()) return false;
        for (size_t index = 0; index < buffers.size(); ++index)
            if (entry.Targets[index] != buffers[index].Value.Target || entry.Indices[index] != buffers[index].BufferIndex) return false;
        return true;
    };
    if (_lastDynamicOnlySet < _dynamicOnlySets.size() && matches(_dynamicOnlySets[_lastDynamicOnlySet])) {
        ++_stats.SetCacheHits;
        return _dynamicOnlySets[_lastDynamicOnlySet].Set;
    }
    for (size_t index = 0; index < _dynamicOnlySets.size(); ++index) {
        if (index == _lastDynamicOnlySet || !matches(_dynamicOnlySets[index])) continue;
        _lastDynamicOnlySet = index;
        ++_stats.SetCacheHits;
        return _dynamicOnlySets[index].Set;
    }
    const auto set = PrepareSet(program, group, buffers, textures, samplers);
    if (set) {
        DynamicOnlySet entry{layout, group, static_cast<uint32_t>(buffers.size()), {}, {}, set.Get()};
        for (size_t index = 0; index < buffers.size(); ++index) {
            entry.Targets[index] = buffers[index].Value.Target;
            entry.Indices[index] = buffers[index].BufferIndex;
        }
        _lastDynamicOnlySet = _dynamicOnlySets.size();
        _dynamicOnlySets.push_back(entry);
    }
    return set;
}

Nullable<render::ShaderParameterSet*> FrameDrawResources::PrepareSet(
    ShaderProgram& program, uint32_t group, std::span<const FrameBufferBinding> buffers,
    std::span<const MaterialTextureFrameData> textures, std::span<const MaterialSamplerFrameData> samplers) {
    if (program.GetDevice() != _device) return nullptr;
    const auto& layout = program.GetParameterLayout();
    const auto& recipe = GetRecipe(program, group);
    auto& key = _keyScratch;
    key.Layout = program.GetPipelineLayout();
    key.Group = group;
    key.Buffers.assign(buffers.begin(), buffers.end());
    key.Textures.clear();
    key.Samplers.clear();
    std::sort(key.Buffers.begin(), key.Buffers.end(), [](const auto& a, const auto& b) { return a.BufferIndex < b.BufferIndex; });
    if (buffers.size() != recipe.Buffers.size()) return nullptr;
    for (size_t index = 0; index < key.Buffers.size(); ++index) {
        const auto& buffer = key.Buffers[index];
        if (buffer.BufferIndex >= layout.Buffers().size() || layout.Buffers()[buffer.BufferIndex].Group != group ||
            !buffer.Value.Target || (index && buffer.BufferIndex == key.Buffers[index - 1].BufferIndex)) return nullptr;
    }
    const auto parameters = layout.Parameters();
    for (uint32_t index : recipe.Textures) {
        const auto& parameter = parameters[index].Info;
        for (uint32_t element = 0; element < parameter.ElementCount; ++element) {
            const auto found = std::find_if(textures.begin(), textures.end(), [&](const auto& value) { return value.Parameter.Binding == parameter.Binding && value.Element == element; });
            if (found == textures.end() || !found->Texture) return nullptr;
            const Nullable<render::TextureView*> view = found->Texture->GetOrCreateSrv(found->SubView);
            if (!view) return nullptr;
            key.Textures.push_back({index, element, view.Get()});
        }
    }
    for (uint32_t index : recipe.Samplers) {
        const auto& parameter = parameters[index].Info;
        for (uint32_t element = 0; element < parameter.ElementCount; ++element) {
            const auto found = std::find_if(samplers.begin(), samplers.end(), [&](const auto& value) { return value.Parameter.Binding == parameter.Binding && value.Element == element; });
            if (found == samplers.end()) return nullptr;
            const auto sampler = _device->GetOrCreateSampler(found->Sampler);
            if (!sampler) return nullptr;
            key.Samplers.push_back({index, element, sampler.Get()});
        }
    }
    if (textures.size() != recipe.TextureCount || samplers.size() != recipe.SamplerCount) return nullptr;
    const auto code = FrameSetKeyHash{}(key);
    const auto found = _setLookup.Find(code, [&](uint32_t index) { return _setCache[index].Key == key; });
    if (found != UINT32_MAX) {
        ++_stats.SetCacheHits;
        return _setCache[found].Set;
    }
    auto created = _device->CreateShaderParameterSet({.Layout = key.Layout, .GroupIndex = group});
    if (!created) return nullptr;
    auto set = created.Release();
    for (const auto& buffer : key.Buffers)
        if (!set->Set(layout.Buffers()[buffer.BufferIndex].Binding, 0, buffer.Value)) return nullptr;
    for (const auto& texture : key.Textures)
        if (!set->Set(parameters[texture.Parameter].Info.Binding, texture.Element, texture.View)) return nullptr;
    for (const auto& sampler : key.Samplers)
        if (!set->Set(parameters[sampler.Parameter].Info.Binding, sampler.Element, sampler.Sampler)) return nullptr;
    if (!set->FlushWrites()) return nullptr;
    auto* pointer = set.get();
    _sets.push_back(std::move(set));
    if (_activeSets == _setCache.size()) _setCache.emplace_back();
    auto& cached = _setCache[_activeSets];
    cached.Key = key;
    cached.Set = pointer;
    _setLookup.Insert(code, static_cast<uint32_t>(_activeSets++));
    ++_stats.SetCreations;
    return pointer;
}

}  // namespace radray
