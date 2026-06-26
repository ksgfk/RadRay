#include <radray/runtime/components/static_mesh_component.h>

#include <algorithm>
#include <cstring>

#include <radray/runtime/texture_asset.h>
#include <radray/runtime/render/standard_material.h>
#include <radray/runtime/render/static_mesh_renderer.h>

namespace radray {

StaticMeshComponent::~StaticMeshComponent() noexcept = default;

RuntimeTypeId StaticMeshComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<StaticMeshComponent>;
}

bool StaticMeshComponent::AreAssetsReady() const noexcept {
    const bool meshReady = _mesh.IsReady();
    const bool texturesReady = std::all_of(_materialTextures.begin(), _materialTextures.end(), [](const auto& texture) {
        return texture.Texture.IsReady();
    });
    return meshReady && texturesReady;
}

void StaticMeshComponent::TickComponent(float deltaTime) {
    (void)deltaTime;
    // 资产尚未交付为 renderer 时持续尝试;就绪后构建一次。
    if (HasRenderers()) {
        return;
    }
    if (AreAssetsReady()) {
        RecreateRenderers();
    }
}

vector<unique_ptr<srp::Renderer>> StaticMeshComponent::BuildRenderers() {
    vector<unique_ptr<srp::Renderer>> result;
    if (_shader == nullptr || _device == nullptr) {
        return result;
    }
    StaticMesh* mesh = _mesh.Get();
    if (mesh == nullptr || mesh->GetRenderMesh() == nullptr) {
        return result;
    }

    // 1) 打包 space1 cbuffer 字节(按提供顺序,gltf 已按 shader 已知布局排列)。
    vector<byte> cbufferData;
    cbufferData.reserve(_materialParams.size() * sizeof(Eigen::Vector4f));
    for (const auto& param : _materialParams) {
        const byte* src = reinterpret_cast<const byte*>(param.Value.data());
        cbufferData.insert(cbufferData.end(), src, src + sizeof(Eigen::Vector4f));
    }

    // 2) 解析贴图 SRV(类型擦除句柄 → TextureAsset → TextureView*)。
    vector<std::pair<string, render::TextureView*>> textures;
    textures.reserve(_materialTextures.size());
    for (const auto& tex : _materialTextures) {
        StreamingAssetRef<TextureAsset> ref = tex.Texture.CastTo<TextureAsset>();
        TextureAsset* asset = ref.Get();
        render::TextureView* srv = (asset != nullptr && asset->IsValid()) ? asset->GetSrv() : nullptr;
        textures.emplace_back(tex.Name, srv);
    }

    // 3) 构建组件自有材质。
    srp::StandardMaterial::Desc desc{};
    desc.MaterialShader = _shader;
    desc.Blend = _blend;
    desc.TwoSided = _twoSided;
    desc.Cutoff = _cutoff;
    desc.CBufferData = std::move(cbufferData);
    desc.Textures = std::move(textures);
    _material = make_unique<srp::StandardMaterial>(_device, std::move(desc));

    // 4) 每 section 一个 StaticMeshRenderer,引用同一材质。
    const auto& sections = mesh->GetSections();
    if (sections.empty()) {
        return result;
    }
    result.reserve(sections.size());
    for (const auto& section : sections) {
        auto renderer = make_unique<srp::StaticMeshRenderer>(
            mesh,
            section.PrimitiveIndex,
            section.FirstIndex,
            section.IndexCount,
            _material.get());
        if (renderer->IsRenderable()) {
            result.push_back(std::move(renderer));
        }
    }
    return result;
}

}  // namespace radray
