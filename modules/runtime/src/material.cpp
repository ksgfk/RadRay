#include <radray/runtime/material.h>

#include <radray/logger.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

Material::Material(GpuSystem& gpuSystem, const MaterialDescriptor& desc) noexcept {
    _gpuSystem = &gpuSystem;
    _shaderPath = desc.ShaderPath;
    _shaderName = desc.ShaderName.empty() ? desc.ShaderPath.string() : desc.ShaderName;
    _vsEntry = desc.VsEntry;
    _psEntry = desc.PsEntry;
    _depthPsEntry = desc.DepthPsEntry;
    _blendMode = desc.BlendMode;
    _twoSided = desc.TwoSided;
    _alphaCutoff = desc.AlphaCutoff;
    _primitive = desc.Primitive;
    _materialSetIndex = desc.MaterialSetIndex;

    _defaultShaders = CompileShaderSet(ShaderVariantKey{}, PixelShaderMode::FullColor);
    if (_defaultShaders.VS == nullptr || _defaultShaders.PS == nullptr || _defaultShaders.RootSig == nullptr) {
        return;
    }

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

    std::optional<MaterialParameterLayout> layoutOpt = buildLayout(_defaultShaders.PS);
    if (!layoutOpt.has_value() || !layoutOpt->HasConstantBuffer()) {
        std::optional<MaterialParameterLayout> vsLayout = buildLayout(_defaultShaders.VS);
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

void Material::OnUnload(IRenderResourceRecycler& recycler) {
    (void)recycler;
    // RootSignature 与 shader 均为非拥有(由 GpuSystem 缓存共享)，仅置空指针。
    _defaultShaders = {};
    _variants.clear();
    _alphaClipVariants.clear();
}

AssetTypeId Material::GetTypeId() const noexcept {
    return runtime_type_id_v<Material>;
}

std::optional<render::BindingParameterId> Material::FindParameterId(std::string_view name) const noexcept {
    if (_defaultShaders.RootSig == nullptr) {
        return std::nullopt;
    }
    return _defaultShaders.RootSig->FindParameterId(name);
}

const MaterialShaderSet* Material::GetShaderSet(
    const ShaderVariantKey& key,
    PixelShaderMode psMode) const {
    if (_gpuSystem == nullptr) {
        return nullptr;
    }
    const bool needPixelShader = NeedsPixelShader(psMode);
    auto& variants = IsAlphaClipOnly(psMode) ? _alphaClipVariants : _variants;
    auto it = variants.find(key);
    if (it == variants.end()) {
        MaterialShaderSet set = CompileShaderSet(key, psMode);
        it = variants.emplace(key, set).first;
    } else if (needPixelShader && it->second.PS == nullptr) {
        it->second = CompileShaderSet(key, psMode);
    }
    const MaterialShaderSet& set = it->second;
    if (set.VS == nullptr || set.RootSig == nullptr || (needPixelShader && set.PS == nullptr)) {
        return nullptr;
    }
    return &set;
}

MaterialShaderSet Material::CompileShaderSet(
    const ShaderVariantKey& key,
    PixelShaderMode psMode) const {
    if (_gpuSystem == nullptr) {
        return {};
    }

    render::Shader* vs = _gpuSystem->GetOrCompileShaderFromFile(
                                       _shaderPath, _vsEntry, render::ShaderStage::Vertex, _shaderName, key.Defines())
                             .Get();
    if (vs == nullptr) {
        RADRAY_ERR_LOG("Material: failed to compile VS '{}' from {}", _vsEntry, _shaderName);
        return {};
    }

    render::Shader* ps = nullptr;
    if (NeedsPixelShader(psMode)) {
        const string& psEntry = IsAlphaClipOnly(psMode) ? _depthPsEntry : _psEntry;
        ps = _gpuSystem->GetOrCompileShaderFromFile(
                           _shaderPath, psEntry, render::ShaderStage::Pixel, _shaderName, key.Defines())
                 .Get();
        if (ps == nullptr) {
            RADRAY_ERR_LOG("Material: failed to compile PS '{}' from {}", psEntry, _shaderName);
            return {};
        }
    }

    render::Shader* shaders[2] = {vs, ps};
    const uint32_t shaderCount = ps != nullptr ? 2u : 1u;
    render::RootSignature* rootSig = _gpuSystem->GetOrCreateRootSignature(std::span<render::Shader*>{shaders, shaderCount}).Get();
    if (rootSig == nullptr) {
        RADRAY_ERR_LOG("Material: failed to get root signature for '{}'", _shaderName);
        return {};
    }

    return MaterialShaderSet{
        .VS = vs,
        .PS = ps,
        .RootSig = rootSig};
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
