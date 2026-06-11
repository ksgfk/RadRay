#include <radray/runtime/renderer/static_mesh_scene_proxy.h>

#include <radray/logger.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/vertex_factory.h>

namespace radray {

StaticMeshSceneProxy::StaticMeshSceneProxy(AssetRef<StaticMesh> mesh, AssetRef<Material> material) noexcept
    : _mesh(std::move(mesh)),
      _material(std::move(material)) {
    // 不在此构建/上传:生命周期初态 Pending。渲染系统在帧顶 UpdateResources 上传并推进到 Ready。
}

StaticMeshSceneProxy::~StaticMeshSceneProxy() noexcept = default;

bool StaticMeshSceneProxy::IsRenderable() const noexcept {
    return GetResourceState() == ResourceState::Ready && !_batchElements.empty() && !_vertexElements.empty();
}

void StaticMeshSceneProxy::UpdateResources(render::CommandBuffer* cmdBuffer, ResourceUploader& uploader) {
    // 仅在 Pending 态被调用(SceneRenderer::PrepareResources 保证)。
    StaticMesh* mesh = _mesh.Get();
    if (mesh == nullptr) {
        return;  // 资产丢失:保持 Pending,下帧再试(实际不应发生,代理持有 AssetRef)。
    }
    if (!mesh->HasRenderData()) {
        // 共享去重:同一 StaticMesh 被多个代理引用时仅上传一次,后续代理走 HasRenderData 分支。
        std::optional<render::RenderMesh> renderMeshOpt = uploader.UploadMesh(cmdBuffer, mesh, _mesh.AsAny());
        if (!renderMeshOpt.has_value()) {
            RADRAY_ERR_LOG("StaticMeshSceneProxy: failed to upload mesh '{}'", mesh->GetMeshResource().Name);
            return;  // 保持 Pending,下帧再试。
        }
        mesh->SetRenderMesh(std::move(renderMeshOpt.value()));
    }
    // 此后 GPU 数据就绪且不变,一次构建几何即可。
    BuildGeometry();
    SetResourceState(ResourceState::Ready);
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
        VertexFactory::Layout layout = VertexFactory::BuildLayout(meshResource.Primitives[0]);
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
