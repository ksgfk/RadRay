#include <radray/runtime/material.h>

#include <radray/logger.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

Material::Material(GpuSystem& gpuSystem, const MaterialDescriptor& desc) noexcept {
    string name = desc.ShaderName.empty() ? desc.ShaderPath.string() : desc.ShaderName;

    render::Shader* vs = gpuSystem.GetOrCompileShaderFromFile(
                                       desc.ShaderPath, desc.VsEntry, render::ShaderStage::Vertex, name)
                             .Get();
    if (vs == nullptr) {
        RADRAY_ERR_LOG("Material: failed to compile VS '{}' from {}", desc.VsEntry, name);
        return;
    }
    render::Shader* ps = gpuSystem.GetOrCompileShaderFromFile(
                                       desc.ShaderPath, desc.PsEntry, render::ShaderStage::Pixel, name)
                             .Get();
    if (ps == nullptr) {
        RADRAY_ERR_LOG("Material: failed to compile PS '{}' from {}", desc.PsEntry, name);
        return;
    }

    // RootSignature 按 shader 绑定布局共享(非独占)。
    render::Shader* shaders[] = {vs, ps};
    render::RootSignature* rootSig = gpuSystem.GetOrCreateRootSignature(std::span<render::Shader*>{shaders}).Get();
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
    _materialSetIndex = desc.MaterialSetIndex;

    // 从 shader 反射抽取 per-material 参数布局。PS 通常承载材质 cbuffer;
    // PS 反射缺失或该 set 无 cbuffer 时回退到 VS 反射。两者都没有则保持空布局。
    auto buildLayout = [&](render::Shader* shader) -> std::optional<MaterialParameterLayout> {
        if (shader == nullptr) {
            return std::nullopt;
        }
        Nullable<const render::ShaderReflectionDesc*> refl = shader->GetReflection();
        if (!refl.HasValue()) {
            return std::nullopt;
        }
        return MaterialParameterLayout::CreateFromReflection(*refl.Get(), _materialSetIndex, desc.MaterialCBufferName);
    };

    std::optional<MaterialParameterLayout> layoutOpt = buildLayout(ps);
    if (!layoutOpt.has_value() || !layoutOpt->HasConstantBuffer()) {
        std::optional<MaterialParameterLayout> vsLayout = buildLayout(vs);
        if (vsLayout.has_value() && vsLayout->HasConstantBuffer()) {
            layoutOpt = std::move(vsLayout);
        }
    }
    if (layoutOpt.has_value()) {
        _paramLayout = std::move(layoutOpt.value());
        _storageTemplate = _paramLayout.CreateStorageTemplate();
    }
}

Material::~Material() noexcept = default;

void Material::OnUnload() {
    // RootSignature 与 shader 均为非拥有(由 GpuSystem 缓存共享)，仅置空指针。
    _rootSig = nullptr;
    _vs = nullptr;
    _ps = nullptr;
}

AssetTypeId Material::GetTypeId() const noexcept {
    return runtime_type_id_v<Material>;
}

std::optional<render::BindingParameterId> Material::FindParameterId(std::string_view name) const noexcept {
    if (_rootSig == nullptr) {
        return std::nullopt;
    }
    return _rootSig->FindParameterId(name);
}

AssetLoadTask LoadMaterial(GpuSystem& gpuSystem, MaterialDescriptor desc) {
    // 纯 CPU 加载:构造 Material(编译 shader + 取共享 RootSignature)。无 GPU 上传。
    auto material = make_unique<Material>(gpuSystem, desc);
    if (!material->IsValid()) {
        co_return AssetLoadResult::Failure("material is invalid");
    }
    co_return AssetLoadResult::Success(std::move(material));
}

}  // namespace radray
