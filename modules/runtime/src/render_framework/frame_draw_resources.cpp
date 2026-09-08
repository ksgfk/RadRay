#include <radray/runtime/render_framework/frame_draw_resources.h>

#include <algorithm>
#include <cstring>
#include <radray/runtime/shader_program.h>

namespace radray {

FrameDrawResources::FrameDrawResources(render::Device* device, DynamicCBufferArena::Descriptor descriptor)
    : _device(device), _descriptor(std::move(descriptor)) {
    _descriptor.Alignment = std::max(_descriptor.Alignment, std::max<uint64_t>(device->GetDetail().CBufferAlignment, 1));
}

FrameDrawResources::~FrameDrawResources() noexcept = default;

void FrameDrawResources::ClearSets() noexcept {
    _setCache.clear();
    _sets.clear();
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

std::optional<PreparedShaderGroup> FrameDrawResources::PrepareGroup(
    ShaderProgram& program, uint32_t group, const ShaderParameterStorage& parameters,
    std::span<const MaterialTextureFrameData> textures, std::span<const MaterialSamplerFrameData> samplers) {
    if (!_arena || parameters.GetLayout() != &program.GetParameterLayout()) return std::nullopt;
    ++_stats.GroupPreparations;
    auto& bindings = _bindingScratch;
    bindings.clear();
    const auto buffers = program.GetParameterLayout().Buffers();
    const auto& recipe = GetRecipe(program, group);
    PreparedShaderGroup result;
    result.Group = group;
    for (const auto& entry : recipe.Buffers) {
        const auto index = entry.Index;
        const auto& buffer = buffers[index];
        const auto bytes = parameters.GetBufferData(index);
        if (bytes.empty() || bytes.size() != buffer.Size) return std::nullopt;
        auto reservation = _arena->Reserve(bytes.size());
        if (!reservation.IsValid()) return std::nullopt;
        std::memcpy(reservation.Data(), bytes.data(), bytes.size());
        _stats.BufferBytesCopied += bytes.size();
        const auto allocation = reservation.Commit(bytes.size());
        if (!allocation.IsValid() || allocation.Offset > std::numeric_limits<uint32_t>::max()) return std::nullopt;
        const bool dynamic = entry.Dynamic;
        bindings.push_back({index, {allocation.Target, {dynamic ? 0 : allocation.Offset, buffer.Size}}});
        if (dynamic) result.DynamicOffsets.push_back({buffer.Binding, static_cast<uint32_t>(allocation.Offset)});
    }
    result.Set = PrepareSet(program, group, bindings, textures, samplers);
    if (!result.Set) return std::nullopt;
    return result;
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
    const auto found = _setCache.find(key);
    if (found != _setCache.end()) {
        ++_stats.SetCacheHits;
        return found->second;
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
    _setCache.emplace(key, pointer);
    ++_stats.SetCreations;
    return pointer;
}

}  // namespace radray
