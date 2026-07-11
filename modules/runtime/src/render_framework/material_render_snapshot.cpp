#include <radray/runtime/render_framework/material_render_snapshot.h>

#include <radray/logger.h>
#include <radray/runtime/sampler_cache.h>

namespace radray {

Nullable<const CompiledShaderVariant*> MaterialRenderSnapshot::ResolveVariant(
    ShaderVariantLibrary& cache,
    uint32_t passIndex,
    render::HlslShaderModel sm) const noexcept {
    return ResolveVariant(cache, passIndex, std::span<const std::string_view>{}, sm);
}

Nullable<const CompiledShaderVariant*> MaterialRenderSnapshot::ResolveVariant(
    ShaderVariantLibrary& cache,
    uint32_t passIndex,
    std::span<const std::string_view> extraKeywords,
    render::HlslShaderModel sm) const noexcept {
    ShaderAsset* shader = Shader.Get();
    if (shader == nullptr) {
        RADRAY_ERR_LOG("MaterialRenderSnapshot::ResolveVariant: no shader assigned");
        return nullptr;
    }
    vector<std::string_view> enabled;
    enabled.reserve(EnabledKeywords.size() + extraKeywords.size());
    for (const string& kw : EnabledKeywords) {
        enabled.emplace_back(kw);
    }
    for (std::string_view kw : extraKeywords) {
        enabled.emplace_back(kw);  // 未在 shader keyword 表声明的会被 Project 忽略
    }
    return shader->GetOrCreateVariant(cache, passIndex, enabled, sm);
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


uint32_t MaterialRenderSnapshot::ApplyResources(
    render::BindingGroup& group,
    const CompiledShaderVariant& variant,
    SamplerCache& samplerCache) const noexcept {
    uint32_t applied = 0;
    for (const TextureEntry& texture : Textures) {
        auto location = FindShaderBindingLocation(variant, texture.Name);
        if (!location.has_value() || location->Group != group.GetGroupIndex()) {
            continue;
        }
        TextureAsset* asset = texture.Texture.Get();
        render::TextureView* view = asset != nullptr ? asset->GetOrCreateSrv(texture.SubView) : nullptr;
        if (view != nullptr && group.SetResource(location->Binding, static_cast<render::ResourceView*>(view))) {
            ++applied;
        }
    }
    for (const SamplerEntry& samplerEntry : Samplers) {
        auto location = FindShaderBindingLocation(variant, samplerEntry.Name);
        if (!location.has_value() || location->Group != group.GetGroupIndex()) {
            continue;
        }
        render::Sampler* sampler = samplerCache.GetOrCreate(samplerEntry.Desc).Get();
        if (sampler != nullptr && group.SetSampler(location->Binding, sampler)) {
            ++applied;
        }
    }
    return applied;
}

}  // namespace radray
