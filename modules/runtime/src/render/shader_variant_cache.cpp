#include <radray/runtime/render/shader_variant_cache.h>

#include <radray/logger.h>
#include <radray/runtime/gpu_system.h>

namespace radray::srp {

size_t ShaderVariantCache::KeyHash::operator()(const Key& k) const noexcept {
    size_t h = std::hash<uint64_t>{}(k.ShaderId);
    h ^= std::hash<std::string_view>{}(k.LightMode) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= ShaderVariantKeyHash{}(k.Keywords) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

const ShaderVariant* ShaderVariantCache::Get(
    const Shader& shader,
    std::string_view lightMode,
    const KeywordSet& keywords) {
    Key key{
        .ShaderId = shader.Id().Value,
        .LightMode = string{lightMode},
        .Keywords = keywords};
    auto it = _cache.find(key);
    if (it == _cache.end()) {
        ShaderVariant variant = Compile(shader, lightMode, keywords);
        it = _cache.emplace(std::move(key), std::move(variant)).first;
    }
    return it->second.IsValid() ? &it->second : nullptr;
}

ShaderVariant ShaderVariantCache::Compile(
    const Shader& shader,
    std::string_view lightMode,
    const KeywordSet& keywords) {
    if (_gpu == nullptr) {
        return {};
    }
    const ShaderPassSource* pass = shader.GetPassSource(lightMode);
    if (pass == nullptr) {
        // 第二层 relevance:该 shader 没有这个 LightMode 的 pass。静默失败(非错误)。
        return {};
    }

    const std::filesystem::path shaderPath{pass->ShaderPath};
    const std::string_view name = pass->ShaderName.empty() ? std::string_view{pass->ShaderPath} : std::string_view{pass->ShaderName};

    std::optional<CompiledShaderEntry> vs = _gpu->GetOrCompileShaderEntryFromFile(
        shaderPath, pass->VsEntry, render::ShaderStage::Vertex, name, keywords.Defines());
    if (!vs.has_value() || vs->Target == nullptr) {
        RADRAY_ERR_LOG("ShaderVariantCache: failed to compile VS '{}' (shader '{}', lightMode '{}')",
                       pass->VsEntry, shader.Name(), lightMode);
        return {};
    }

    std::optional<CompiledShaderEntry> ps{};
    if (pass->PsEntry.has_value()) {
        ps = _gpu->GetOrCompileShaderEntryFromFile(
            shaderPath, *pass->PsEntry, render::ShaderStage::Pixel, name, keywords.Defines());
        if (!ps.has_value() || ps->Target == nullptr) {
            RADRAY_ERR_LOG("ShaderVariantCache: failed to compile PS '{}' (shader '{}', lightMode '{}')",
                           *pass->PsEntry, shader.Name(), lightMode);
            return {};
        }
    }

    render::Shader* shaders[2] = {vs->Target, ps.has_value() ? ps->Target : nullptr};
    const uint32_t shaderCount = ps.has_value() ? 2u : 1u;
    std::optional<RootSignatureEntry> rs =
        _gpu->GetOrCreateRootSignatureEntry(std::span<render::Shader*>{shaders, shaderCount});
    if (!rs.has_value() || rs->Target == nullptr) {
        RADRAY_ERR_LOG("ShaderVariantCache: failed to get root signature (shader '{}', lightMode '{}')",
                       shader.Name(), lightMode);
        return {};
    }

    return ShaderVariant{
        .VS = std::move(vs.value()),
        .PS = ps,
        .RootSig = rs->Target,
        .RootLayout = std::move(rs->Layout)};
}

}  // namespace radray::srp
