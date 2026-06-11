#pragma once

#include <radray/types.h>
#include <radray/vertex_data.h>
#include <radray/render/common.h>

namespace radray {

/// 顶点布局工厂。RadRay 版的 UE5 FVertexFactory(最小化):
/// 从 mesh 的 CPU 属性描述(MeshPrimitive)推导出 GPU 渲染所需的 VertexBufferLayout,
/// 保证 PSO 的 input layout 始终与 mesh 实际携带的属性一致。
///
/// 当前实现假设单一交错顶点缓冲(所有属性共享一个 stride),与
/// TriangleMesh::ToSimpleMeshResource 的产出一致。
class VertexFactory {
public:
    /// 将 CPU 顶点属性类型映射为 render::VertexFormat(仅支持 32-bit 标量)。
    static render::VertexFormat ToVertexFormat(VertexDataType type, uint16_t componentCount) noexcept;

    /// 从一个 primitive 的属性列表构建顶点布局。outElements 引用 outEntries 的字符串,
    /// 因此三者生命周期需一致(都作为返回值的一部分持有)。
    struct Layout {
        vector<render::VertexElement> Elements;
        uint64_t Stride{0};
    };

    /// 构建单交错缓冲的顶点布局。失败(出现不支持的格式)时返回空 Elements。
    static Layout BuildLayout(const MeshPrimitive& primitive);

    /// 为顶点布局生成一个稳定的字符串签名,用于 PSO 缓存 key。
    static string BuildSignature(const render::VertexBufferLayout& layout);
};

}  // namespace radray
