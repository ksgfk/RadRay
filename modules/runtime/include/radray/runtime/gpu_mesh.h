#pragma once

#include <radray/runtime/vertex_layout.h>
#include <radray/runtime/vertex_data.h>

namespace radray {

/// 持有由网格资源创建的 GPU 缓冲区及绘制视图。
class GpuMesh {
public:
    struct DrawData {
        vector<render::VertexBufferBinding> VertexBuffers;
        GeometryVertexLayout Layout;
        render::IndexBufferView Ibv;
        render::PrimitiveTopology Topology{render::PrimitiveTopology::TriangleList};
    };

    vector<unique_ptr<render::Buffer>> Buffers;
    vector<DrawData> Draws;
    /// Runtime construction packs CPU bins into Buffers[0], without changing their byte layout.
    vector<render::BufferRange> BinRanges;
};
class ResourceUploader;
Nullable<unique_ptr<GpuMesh>> AllocateRuntimeMesh(render::Device& device, const MeshResource& source, uint64_t byteLimit = 64 * 1024 * 1024);
bool TryUploadRuntimeMesh(ResourceUploader& uploader, render::CommandBuffer* command, const MeshResource& source, const GpuMesh& mesh);

}  // namespace radray
