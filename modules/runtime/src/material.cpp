#include <radray/runtime/material.h>

#include <radray/logger.h>
#include <radray/runtime/render_system.h>

namespace radray {

Material::Material(AppRenderSystem& renderSystem, const MaterialDescriptor& desc) noexcept {
    string name = desc.ShaderName.empty() ? desc.ShaderPath.string() : desc.ShaderName;

    render::Shader* vs = renderSystem.GetOrCompileShaderFromFile(
                                       desc.ShaderPath, desc.VsEntry, render::ShaderStage::Vertex, name)
                             .Get();
    if (vs == nullptr) {
        RADRAY_ERR_LOG("Material: failed to compile VS '{}' from {}", desc.VsEntry, name);
        return;
    }
    render::Shader* ps = renderSystem.GetOrCompileShaderFromFile(
                                       desc.ShaderPath, desc.PsEntry, render::ShaderStage::Pixel, name)
                             .Get();
    if (ps == nullptr) {
        RADRAY_ERR_LOG("Material: failed to compile PS '{}' from {}", desc.PsEntry, name);
        return;
    }

    // RootSignature 按 shader 绑定布局共享(非独占)。
    render::Shader* shaders[] = {vs, ps};
    render::RootSignature* rootSig = renderSystem.GetOrCreateRootSignature(std::span<render::Shader*>{shaders}).Get();
    if (rootSig == nullptr) {
        RADRAY_ERR_LOG("Material: failed to get root signature for '{}'", name);
        return;
    }

    _rootSig = rootSig;
    _vs = vs;
    _ps = ps;
    _vsEntry = desc.VsEntry;
    _psEntry = desc.PsEntry;
    _primitive = desc.Primitive;
    _depthStencil = desc.DepthStencil;
    _blend = desc.Blend;
}

Material::~Material() noexcept = default;

void Material::OnUnload() {
    // RootSignature 与 shader 均为非拥有(由 AppRenderSystem 缓存共享)，仅置空指针。
    _rootSig = nullptr;
    _vs = nullptr;
    _ps = nullptr;
}

AssetTypeId Material::GetTypeId() const noexcept {
    return asset_type_id_v<Material>;
}

std::optional<render::BindingParameterId> Material::FindParameterId(std::string_view name) const noexcept {
    if (_rootSig == nullptr) {
        return std::nullopt;
    }
    return _rootSig->FindParameterId(name);
}

}  // namespace radray
