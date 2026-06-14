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
    : _mesh(std::move(mesh)),
      _material(std::move(material)) {
    // 资产在构造代理前已由 AssetManager 保证 GPU 就绪(StaticMesh 构造即完整),
    // 故这里直接构建几何单元。
    BuildGeometry();
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

}  // namespace radray
