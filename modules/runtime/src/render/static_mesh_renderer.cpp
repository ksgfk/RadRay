#include <radray/runtime/render/static_mesh_renderer.h>

#include <radray/logger.h>
#include <radray/vertex_data.h>
#include <radray/render/gpu_resource.h>
#include <radray/runtime/static_mesh.h>

namespace radray::srp {

namespace {

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
    }
    return render::VertexFormat::UNKNOWN;
}

}  // namespace

StaticMeshRenderer::StaticMeshRenderer(
    StaticMesh* mesh,
    uint32_t primitiveIndex,
    uint32_t firstIndex,
    uint32_t indexCount,
    Material* material) noexcept
    : _material(material) {
    BuildGeometry(mesh, primitiveIndex, firstIndex, indexCount);
}

void StaticMeshRenderer::BuildGeometry(
    StaticMesh* mesh, uint32_t primitiveIndex, uint32_t firstIndex, uint32_t indexCount) noexcept {
    if (mesh == nullptr) {
        return;
    }
    const render::RenderMesh* renderMesh = mesh->GetRenderMesh();
    if (renderMesh == nullptr || primitiveIndex >= renderMesh->_drawDatas.size()) {
        return;
    }
    const render::RenderMesh::DrawData& drawData = renderMesh->_drawDatas[primitiveIndex];
    if (drawData.Vbv.Target == nullptr || drawData.Ibv.Target == nullptr || indexCount == 0) {
        return;
    }

    _element = MeshBatchElement{
        .Vbv = drawData.Vbv,
        .Ibv = drawData.Ibv,
        .IndexCount = indexCount,
        .FirstIndex = firstIndex,
        .VertexOffset = 0};

    // 顶点布局:从该 primitive(若缺,退回 Primitives[0])抽取语义,拷贝字符串自持。
    const MeshResource& meshResource = mesh->GetMeshResource();
    if (meshResource.Primitives.empty()) {
        return;
    }
    const size_t layoutPrim = primitiveIndex < meshResource.Primitives.size() ? primitiveIndex : 0;
    const MeshPrimitive& primitive = meshResource.Primitives[layoutPrim];

    _semanticStorage.reserve(primitive.VertexBuffers.size());
    _vertexElements.reserve(primitive.VertexBuffers.size());
    uint64_t stride = 0;
    uint32_t location = 0;
    bool ok = true;
    for (const VertexBufferEntry& entry : primitive.VertexBuffers) {
        const render::VertexFormat fmt = ToVertexFormat(entry.Type, entry.ComponentCount);
        if (fmt == render::VertexFormat::UNKNOWN) {
            RADRAY_ERR_LOG("StaticMeshRenderer: unsupported vertex format for semantic '{}' ({} comps)",
                           entry.Semantic, entry.ComponentCount);
            ok = false;
            break;
        }
        _semanticStorage.push_back(entry.Semantic);
        stride = entry.Stride;  // 单交错缓冲:共享 stride。
        _vertexElements.push_back(render::VertexElement{
            .Offset = entry.Offset,
            .Semantic = {},  // 下面统一回填,避免重分配导致 string_view 悬垂。
            .SemanticIndex = entry.SemanticIndex,
            .Format = fmt,
            .Location = location++});
    }
    if (!ok) {
        _semanticStorage.clear();
        _vertexElements.clear();
        return;
    }
    // _semanticStorage 已定型,回填稳定的 string_view。
    for (size_t i = 0; i < _vertexElements.size(); ++i) {
        _vertexElements[i].Semantic = _semanticStorage[i];
    }
    _vertexLayout = render::VertexBufferLayout{stride, render::VertexStepMode::Vertex, _vertexElements};
}

}  // namespace radray::srp
