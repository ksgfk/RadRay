#include <radray/runtime/render_framework/material_render_snapshot.h>

#include <radray/logger.h>
#include <radray/runtime/render_framework/sampler_cache.h>

namespace radray {

Nullable<const render::CompiledShaderVariant*> MaterialRenderSnapshot::ResolveVariant(
    render::ShaderVariantCache& cache,
    uint32_t passIndex,
    render::HlslShaderModel sm) const noexcept {
    ShaderAsset* shader = Shader.Get();
    if (shader == nullptr) {
        RADRAY_ERR_LOG("MaterialRenderSnapshot::ResolveVariant: no shader assigned");
        return nullptr;
    }
    vector<std::string_view> enabled;
    enabled.reserve(EnabledKeywords.size());
    for (const string& kw : EnabledKeywords) {
        enabled.emplace_back(kw);
    }
    return shader->GetOrCreateVariant(cache, passIndex, enabled, sm);
}

uint32_t MaterialRenderSnapshot::ApplyProperties(render::ShaderParameterTable& table, SamplerCache& samplerCache) const noexcept {
    uint32_t applied = 0;
    for (const ConstantEntry& c : Constants) {
        if (!c.Bytes.empty() && table.SetBytes(c.Name, c.Bytes.data(), static_cast<uint32_t>(c.Bytes.size()))) {
            ++applied;
        }
    }
    applied += ApplyResources(table, samplerCache);
    return applied;
}

void MaterialRenderSnapshot::CollectConstants(vector<MaterialConstantValue>& out) const noexcept {
    out.clear();
    out.reserve(Constants.size());
    for (const ConstantEntry& c : Constants) {
        if (c.Bytes.empty()) {
            continue;
        }
        out.emplace_back(MaterialConstantValue{
            .Name = std::string_view{c.Name},
            .Bytes = std::span<const byte>{c.Bytes.data(), c.Bytes.size()}});
    }
}

uint32_t MaterialRenderSnapshot::ApplyResources(render::ShaderParameterTable& table, SamplerCache& samplerCache) const noexcept {
    uint32_t applied = 0;
    for (const TextureEntry& t : Textures) {
        TextureAsset* tex = t.Texture.Get();
        render::TextureView* srv = tex != nullptr ? tex->GetOrCreateSrv(t.SubView) : nullptr;
        if (srv != nullptr && table.SetResource(t.Name, static_cast<render::ResourceView*>(srv))) {
            ++applied;
        }
    }
    for (const SamplerEntry& s : Samplers) {
        render::Sampler* sampler = samplerCache.GetOrCreate(s.Desc).Get();
        if (sampler != nullptr && table.SetSampler(s.Name, sampler)) {
            ++applied;
        }
    }
    return applied;
}

}  // namespace radray
