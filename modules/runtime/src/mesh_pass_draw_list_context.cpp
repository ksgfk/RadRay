#include <radray/runtime/mesh_pass_draw_list_context.h>

#include <radray/runtime/mesh_pass_processor.h>

namespace radray {

void MeshPassDrawListContext::FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) {
    const auto& element = context.Batch.Elements[elementIndex];
    command.IndexBuffer = {element.IndexBuffer.Get(), element.IndexBufferOffset, element.IndexStride};
    command.IndexCount = element.IndexCount;
    command.FirstIndex = element.FirstIndex;
    command.VertexOffset = element.VertexOffset;
}

MeshDrawCommand& DynamicMeshPassDrawListContext::AddCommand(const MeshDrawCommand& initializer) {
    return Commands.emplace_back(initializer);
}

}  // namespace radray
