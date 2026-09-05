#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace radray {

bool BuildRendererList(const RendererListDesc& desc, MeshPassProcessor& processor, RendererList& out) {
    out.ResetForReuse();
    if (!desc.Culling || !desc.View || !desc.Culling->Scene || !desc.Culling->Stats.Valid ||
        desc.Culling->View != desc.View || desc.QueueRange.Min > desc.QueueRange.Max || desc.MaterialPassName.empty()) return false;
    const auto& scene = *desc.Culling->Scene.Get();
    for (const auto& visible : desc.Culling->Primitives) {
        if (visible.Primitive >= scene.Primitives.size()) return false;
        const auto& primitive = scene.Primitives[visible.Primitive];
        if (primitive.FirstMeshBatch > scene.MeshBatches.size() || primitive.MeshBatchCount > scene.MeshBatches.size() - primitive.FirstMeshBatch) return false;
        for (uint32_t offset = 0; offset < primitive.MeshBatchCount; ++offset) {
            const auto& batch = scene.MeshBatches[primitive.FirstMeshBatch + offset];
            if (batch.Primitive != visible.Primitive || batch.Material >= scene.Materials.size()) return false;
        }
    }
    out.Stats.VisiblePrimitives = desc.Culling->Primitives.size();
    for (const auto& visible : desc.Culling->Primitives) {
        const auto& primitive = scene.Primitives[visible.Primitive];
        for (uint32_t offset = 0; offset < primitive.MeshBatchCount; ++offset) {
            ++out.Stats.ConsideredBatches;
            if (!(primitive.LayerMask & desc.LayerMask)) {
                ++out.Stats.LayerRejected;
                continue;
            }
            const auto batchIndex = primitive.FirstMeshBatch + offset;
            const auto& batch = scene.MeshBatches[batchIndex];
            const auto& material = scene.Materials[batch.Material];
            if (!desc.QueueRange.Contains(material.Queue)) {
                ++out.Stats.QueueRejected;
                continue;
            }
            const auto pass = material.FindPass(desc.MaterialPassName);
            if (!pass) {
                ++out.Stats.MissingPass;
                continue;
            }
            if (!pass->Valid || !pass->Program) {
                ++out.Stats.InvalidBindings;
                continue;
            }
            MeshPassDrawListContext result;
            processor.AddMeshBatch(desc, scene, batch, result);
            if (!result._command) {
                switch (result._reason) {
                    case MeshPassRejectReason::MissingPass: ++out.Stats.MissingPass; break;
                    case MeshPassRejectReason::InvalidBindings: ++out.Stats.InvalidBindings; break;
                    case MeshPassRejectReason::InvalidGeometry: ++out.Stats.InvalidGeometry; break;
                    case MeshPassRejectReason::PrepareResourceFailed: ++out.Stats.PrepareResourceFailed; break;
                    case MeshPassRejectReason::ProcessorRejected: ++out.Stats.ProcessorRejected; break;
                }
                continue;
            }
            float depth = visible.ViewDepth;
            if (!std::isfinite(depth)) {
                ++out.Stats.NonFiniteDepth;
                depth = std::numeric_limits<float>::max();
            }
            result._command->SortData = {material.Queue, pass->ProgramFrameId, batch.Material, depth, batch.Primitive, batchIndex};
            out.Commands.push_back(std::move(*result._command));
        }
    }
    std::sort(out.Commands.begin(), out.Commands.end(), [&](const auto& left, const auto& right) {
        const auto& a = left.SortData;
        const auto& b = right.SortData;
        if (a.Queue != b.Queue) return static_cast<int32_t>(a.Queue) < static_cast<int32_t>(b.Queue);
        if (desc.Sorting == RendererListSorting::StateThenFrontToBack) {
            if (a.ProgramFrameId != b.ProgramFrameId) return a.ProgramFrameId < b.ProgramFrameId;
            if (a.Material != b.Material) return a.Material < b.Material;
        }
        if (a.ViewDepth != b.ViewDepth) return desc.Sorting == RendererListSorting::BackToFront ? a.ViewDepth > b.ViewDepth : a.ViewDepth < b.ViewDepth;
        return std::tie(a.Primitive, a.Batch) < std::tie(b.Primitive, b.Batch);
    });
    out.Stats.Commands = out.Commands.size();
    out.Stats.Valid = true;
    return true;
}

}  // namespace radray
