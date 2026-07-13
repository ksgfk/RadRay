#include <radray/runtime/render_framework/material_render_snapshot.h>

#include <radray/logger.h>

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

}  // namespace radray
