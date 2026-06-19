#include <radray/runtime/renderer/static_mesh_scene_proxy.h>

#include <radray/logger.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/static_mesh.h>

namespace radray {

namespace {

struct StaticMeshVertexLayout {
    vector<render::VertexElement> Elements;
    uint64_t Stride{0};
};

render::VertexFormat ToVertexFormat(VertexDataType type, uint16_t componentCount) noexcept {
    switch (type) {
        case VertexDataType::FLOAT:
            switch (componentCount) {
                case 1: return render::VertexFormat::FLOAT32;
                case 2: return render::VertexFormat::FLOAT32X2;
                case 3: return render::VertexFormat::FLOAT32X3;
                case 4: return render::VertexFormat::FLOAT32X4;
                default: return render::VertexFormat::UNKNOWN;
            }
        case VertexDataType::UINT:
            switch (componentCount) {
                case 1: return render::VertexFormat::UINT32;
                case 2: return render::VertexFormat::UINT32X2;
                case 3: return render::VertexFormat::UINT32X3;
                case 4: return render::VertexFormat::UINT32X4;
                default: return render::VertexFormat::UNKNOWN;
            }
        case VertexDataType::SINT:
            switch (componentCount) {
                case 1: return render::VertexFormat::SINT32;
                case 2: return render::VertexFormat::SINT32X2;
                case 3: return render::VertexFormat::SINT32X3;
                case 4: return render::VertexFormat::SINT32X4;
                default: return render::VertexFormat::UNKNOWN;
            }
        default:
            return render::VertexFormat::UNKNOWN;
    }
}

StaticMeshVertexLayout BuildStaticMeshVertexLayout(const MeshPrimitive& primitive) {
    StaticMeshVertexLayout layout{};
    uint32_t location = 0;
    for (const VertexBufferEntry& entry : primitive.VertexBuffers) {
        render::VertexFormat fmt = ToVertexFormat(entry.Type, entry.ComponentCount);
        if (fmt == render::VertexFormat::UNKNOWN) {
            RADRAY_ERR_LOG("StaticMeshSceneProxy: unsupported vertex attribute '{}'", entry.Semantic);
            layout.Elements.clear();
            layout.Stride = 0;
            return layout;
        }
        layout.Elements.emplace_back(render::VertexElement{
            .Offset = entry.Offset,
            .Semantic = entry.Semantic,
            .SemanticIndex = entry.SemanticIndex,
            .Format = fmt,
            .Location = location++});
        layout.Stride = entry.Stride;  // single interleaved buffer: stride is shared
    }
    return layout;
}

}  // namespace

StaticMeshSceneProxy::StaticMeshSceneProxy(
    StreamingAssetRef<StaticMesh> mesh,
    StreamingAssetRef<Material> material) noexcept
    : StaticMeshSceneProxy(std::move(mesh), std::move(material), {}) {}

StaticMeshSceneProxy::StaticMeshSceneProxy(
    StreamingAssetRef<StaticMesh> mesh,
    StreamingAssetRef<Material> material,
    vector<MaterialParameterAssignment> materialParams) noexcept
    : StaticMeshSceneProxy(std::move(mesh), std::move(material), std::move(materialParams), {}) {}

StaticMeshSceneProxy::StaticMeshSceneProxy(
    StreamingAssetRef<StaticMesh> mesh,
    StreamingAssetRef<Material> material,
    vector<MaterialParameterAssignment> materialParams,
    vector<MaterialTextureAssignment> materialTextures) noexcept
    : _mesh(std::move(mesh)),
      _material(std::move(material)),
      _materialParams(std::move(materialParams)),
      _materialTextures(std::move(materialTextures)) {
    // 资产在构造代理前已由 AssetManager 保证 GPU 就绪(StaticMesh 构造即完整),
    // 故这里直接构建几何单元与材质参数实例。GPU 侧材质代理延迟到首次索取。
    BuildGeometry();
    BuildMaterialInstance();
}

StaticMeshSceneProxy::~StaticMeshSceneProxy() noexcept = default;

bool StaticMeshSceneProxy::IsRenderable() const noexcept {
    return !_batchElements.empty() && !_vertexElements.empty();
}

void StaticMeshSceneProxy::CollectBatchElements(vector<MeshBatchElement>& out) const {
    out.insert(out.end(), _batchElements.begin(), _batchElements.end());
}

void StaticMeshSceneProxy::BuildGeometry() noexcept {
    StaticMesh* mesh = _mesh.Get();
    if (mesh == nullptr) {
        return;
    }
    const render::RenderMesh* renderMesh = mesh->GetRenderMesh();
    if (renderMesh == nullptr) {
        return;
    }
    const MeshResource& meshResource = mesh->GetMeshResource();
    const vector<StaticMeshSection>& sections = mesh->GetSections();

    // 顶点布局:从第一个 primitive 推导(单交错缓冲)。
    if (!meshResource.Primitives.empty()) {
        StaticMeshVertexLayout layout = BuildStaticMeshVertexLayout(meshResource.Primitives[0]);
        _vertexElements = std::move(layout.Elements);
        _vertexLayout = render::VertexBufferLayout{
            layout.Stride,
            render::VertexStepMode::Vertex,
            _vertexElements};
    }

    auto makeElement = [&](uint32_t primitiveIndex, uint32_t firstIndex, uint32_t indexCount) -> void {
        if (primitiveIndex >= renderMesh->_drawDatas.size()) {
            return;
        }
        const render::RenderMesh::DrawData& drawData = renderMesh->_drawDatas[primitiveIndex];
        if (drawData.Vbv.Target == nullptr || drawData.Ibv.Target == nullptr || indexCount == 0) {
            return;
        }
        _batchElements.emplace_back(MeshBatchElement{
            .Vbv = drawData.Vbv,
            .Ibv = drawData.Ibv,
            .IndexCount = indexCount,
            .FirstIndex = firstIndex,
            .VertexOffset = 0});
    };

    if (!sections.empty()) {
        // 有 section:每个 section 一个绘制单元(对应 UE5 按材质槽位切分)。
        for (const StaticMeshSection& section : sections) {
            makeElement(section.PrimitiveIndex, section.FirstIndex, section.IndexCount);
        }
    } else {
        // 无 section:每个 primitive 整体绘制一次,索引数取自 CPU 元数据。
        for (uint32_t primIdx = 0; primIdx < meshResource.Primitives.size(); ++primIdx) {
            const MeshPrimitive& prim = meshResource.Primitives[primIdx];
            makeElement(primIdx, 0, prim.IndexBuffer.IndexCount);
        }
    }
}

void StaticMeshSceneProxy::BuildMaterialInstance() noexcept {
    Material* material = _material.Get();
    if (material == nullptr || !material->IsValid()) {
        return;
    }
    _materialInstance = MaterialInstance{material};
    // 把 per-使用点材质参数按名写入实例存储(向量型)。
    for (const MaterialParameterAssignment& param : _materialParams) {
        _materialInstance.SetVector(param.Name, param.Value);
    }
    for (const MaterialTextureAssignment& texture : _materialTextures) {
        _materialInstance.SetTexture(texture.Name, texture.Texture);
    }
}

render::DescriptorSet* StaticMeshSceneProxy::GetMaterialDescriptorSet(GpuSystem* gpuSystem) const {
    if (gpuSystem == nullptr || gpuSystem->GetDevice() == nullptr || !_materialInstance.IsValid()) {
        return nullptr;
    }
    // 静态材质:首次索取时一次性构建 GPU 代理并缓存;失败后不再重试。
    if (!_materialProxyBuilt && !_materialProxyFailed) {
        if (_materialRenderProxy.Build(gpuSystem->GetDevice(), gpuSystem, _materialInstance)) {
            _materialProxyBuilt = true;
        } else {
            _materialProxyFailed = true;
        }
    }
    return _materialProxyBuilt ? _materialRenderProxy.GetDescriptorSet() : nullptr;
}

render::DescriptorSetIndex StaticMeshSceneProxy::GetMaterialSetIndex() const noexcept {
    Material* material = _material.Get();
    if (material != nullptr) {
        return render::DescriptorSetIndex{material->GetMaterialSetIndex()};
    }
    return render::DescriptorSetIndex{1};
}

}  // namespace radray
