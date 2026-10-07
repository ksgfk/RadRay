#pragma once

#include <radray/runtime/vertex_layout.h>

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
};

}  // namespace radray
