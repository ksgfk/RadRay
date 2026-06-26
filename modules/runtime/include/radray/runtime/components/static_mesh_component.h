#pragma once

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/render/material.h>
#include <radray/runtime/render/material_assignment.h>
#include <radray/runtime/render/standard_material.h>

namespace radray {

namespace render {
class Device;
}  // namespace render

namespace srp {
class Shader;
}  // namespace srp

class StaticMeshComponent;

/// 引用 StaticMesh 资产的静态网格组件。
/// 对应 UE5 的 UStaticMeshComponent。
///
/// 资产异步加载:组件持有 StreamingAssetRef,每帧 TickComponent 检查资产是否就绪；
/// 资产就绪后【构建】自有 srp::StandardMaterial + 每 section 一个 srp::StaticMeshRenderer,注册到 srp::Scene。
///
/// 渲染依赖(srp::Shader* / render::Device* / BlendMode)由生成方(如 gltf 导出)注入。
class StaticMeshComponent : public PrimitiveComponent {
public:
    StaticMeshComponent() noexcept = default;
    ~StaticMeshComponent() noexcept override;

    RuntimeTypeId GetTypeId() const noexcept override;

    /// 设置 streaming 资产引用(mesh 必需)。组件会在资产就绪后构建 renderer。
    void SetStaticMesh(StreamingAssetRef<StaticMesh> mesh) noexcept { _mesh = std::move(mesh); }
    /// 设置 per-使用点材质参数(按名顺序打包进 gMaterial cbuffer)。
    void SetMaterialParams(vector<MaterialParameterAssignment> params) noexcept { _materialParams = std::move(params); }
    /// 设置 per-使用点材质贴图(按名绑定 space1 texture 槽)。
    void SetMaterialTextures(vector<MaterialTextureAssignment> textures) noexcept { _materialTextures = std::move(textures); }

    /// —— 渲染依赖注入(生成方设置)——
    void SetRenderShader(srp::Shader* shader) noexcept { _shader = shader; }
    void SetRenderDevice(render::Device* device) noexcept { _device = device; }
    void SetBlendMode(srp::BlendMode blend) noexcept { _blend = blend; }
    void SetTwoSided(bool twoSided) noexcept { _twoSided = twoSided; }
    void SetAlphaCutoff(float cutoff) noexcept { _cutoff = cutoff; }

    const StreamingAssetRef<StaticMesh>& GetStaticMesh() const noexcept { return _mesh; }
    const vector<MaterialParameterAssignment>& GetMaterialParams() const noexcept { return _materialParams; }
    const vector<MaterialTextureAssignment>& GetMaterialTextures() const noexcept { return _materialTextures; }

    void TickComponent(float deltaTime) override;
    vector<unique_ptr<srp::Renderer>> BuildRenderers() override;

private:
    bool AreAssetsReady() const noexcept;

    StreamingAssetRef<StaticMesh> _mesh;
    vector<MaterialParameterAssignment> _materialParams;
    vector<MaterialTextureAssignment> _materialTextures;

    srp::Shader* _shader{nullptr};
    render::Device* _device{nullptr};
    srp::BlendMode _blend{srp::BlendMode::Opaque};
    bool _twoSided{false};
    float _cutoff{0.5f};

    /// 组件拥有材质(被自有 renderer 引用)。BuildRenderers 时(重新)构建。
    unique_ptr<srp::StandardMaterial> _material;
};

template <>
struct RuntimeTypeTrait<StaticMeshComponent> {
    static constexpr RuntimeTypeId value{0xfb7d1ec1, 0x9e98, 0x4cb4, 0xa3, 0xc0, 0x61, 0x2d, 0x1b, 0x49, 0xe5, 0x37};
};

}  // namespace radray
