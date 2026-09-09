#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <tuple>
#include <radray/profiler.h>
#include <radray/runtime/render_framework/cpu_draw_record.h>
#include <radray/types.h>

namespace radray {
namespace {

constexpr uint32_t kMaxSharedRendererLists = 8;

struct ListTarget {
    const RendererListDesc* Desc;
    RendererList* Out;
    uint32_t PassHash;
};

void SortRendererListItems(const RendererListDesc& desc, RendererList& out) {
    RADRAY_PROFILE_SCOPE_N("SortRendererList");
    std::sort(out.Items.begin(), out.Items.end(), [&](const auto& left, const auto& right) {
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
}

bool HasDrawRecordTable(const RenderSceneSnapshot& scene) noexcept {
    return !scene.DrawRecords.empty() && scene.PrimitiveDrawBegin.size() == scene.Primitives.size() + 1;
}

bool DescriptorIsValid(const RendererListDesc& desc) noexcept {
    return desc.Culling && desc.View && desc.Culling->Scene && desc.Culling->Stats.Valid &&
           desc.Culling->View == desc.View && desc.QueueRange.Min <= desc.QueueRange.Max && !desc.MaterialPassName.empty();
}

bool ValidateVisibleBatches(const CullingResults& culling) {
    const auto& scene = *culling.Scene.Get();
    for (const auto& visible : culling.Primitives) {
        if (visible.Primitive >= scene.Primitives.size()) return false;
        const auto& primitive = scene.Primitives[visible.Primitive];
        if (primitive.FirstMeshBatch > scene.MeshBatches.size() || primitive.MeshBatchCount > scene.MeshBatches.size() - primitive.FirstMeshBatch) return false;
        for (uint32_t offset = 0; offset < primitive.MeshBatchCount; ++offset) {
            const auto& batch = scene.MeshBatches[primitive.FirstMeshBatch + offset];
            if (batch.Primitive != visible.Primitive || batch.Material >= scene.Materials.size()) return false;
        }
    }
    return true;
}

bool AppendPreparedCommand(const RendererListDesc& desc, const VisiblePrimitive& visible, MeshBatchIndex batchIndex,
                            RenderQueue queue, uint32_t programFrameId, RenderMaterialIndex material,
                            MeshPassDrawListContext& result, RendererList& out) {
    if (!result.HasCommand()) {
        switch (result.Reason()) {
            case MeshPassRejectReason::MissingPass:
                ++out.Stats.MissingPass;
                if (desc.RequireMaterialPass) ++out.Stats.MissingRequiredPass;
                break;
            case MeshPassRejectReason::InvalidBindings: ++out.Stats.InvalidBindings; break;
            case MeshPassRejectReason::InvalidGeometry: ++out.Stats.InvalidGeometry; break;
            case MeshPassRejectReason::PrepareResourceFailed: ++out.Stats.PrepareResourceFailed; break;
            case MeshPassRejectReason::ProcessorRejected: ++out.Stats.ProcessorRejected; break;
        }
        return true;
    }
    float depth = visible.ViewDepth;
    if (!std::isfinite(depth)) {
        ++out.Stats.NonFiniteDepth;
        depth = std::numeric_limits<float>::max();
    }
    if (out.Commands.size() >= std::numeric_limits<uint32_t>::max()) return false;
    out.Items.push_back({{queue, programFrameId, material, depth, visible.Primitive, batchIndex}, static_cast<uint32_t>(out.Commands.size())});
    out.Commands.push_back(result.TakeCommand());
    return true;
}

void FinishList(const RendererListDesc& desc, RendererList& out) {
    SortRendererListItems(desc, out);
    out.Stats.Commands = out.Commands.size();
    out.Stats.Valid = true;
}

void ResetTargets(std::span<ListTarget> targets) {
    for (auto& target : targets) target.Out->ResetForReuse();
}

bool EmitFromRecords(const ListTarget& target, MeshPassProcessor& processor) {
    const auto& desc = *target.Desc;
    const auto& scene = *desc.Culling->Scene.Get();
    auto& out = *target.Out;
    out.Stats.VisiblePrimitives = desc.Culling->Primitives.size();
    for (const auto& visible : desc.Culling->Primitives) {
        if (visible.Primitive >= scene.Primitives.size() || visible.Primitive + 1 >= scene.PrimitiveDrawBegin.size()) return false;
        const auto& primitive = scene.Primitives[visible.Primitive];
        if (!(primitive.LayerMask & desc.LayerMask)) {
            out.Stats.LayerRejected += std::max<uint64_t>(1, primitive.MeshBatchCount);
            continue;
        }
        const uint32_t begin = scene.PrimitiveDrawBegin[visible.Primitive];
        const uint32_t end = scene.PrimitiveDrawBegin[visible.Primitive + 1];
        bool matchedPass = false;
        for (uint32_t index = begin; index < end; ++index) {
            const auto& record = scene.DrawRecords[index];
            if (record.PassNameHash != target.PassHash) continue;
            matchedPass = true;
            ++out.Stats.ConsideredBatches;
            if (!desc.QueueRange.Contains(record.Queue)) {
                ++out.Stats.QueueRejected;
                continue;
            }
            if (record.Status == DrawRecordStatus::MissingPass) {
                ++out.Stats.MissingPass;
                if (desc.RequireMaterialPass) ++out.Stats.MissingRequiredPass;
                continue;
            }
            if (record.Status == DrawRecordStatus::InvalidBindings) {
                ++out.Stats.InvalidBindings;
                continue;
            }
            if (record.Status == DrawRecordStatus::InvalidGeometry) {
                ++out.Stats.InvalidGeometry;
                continue;
            }
            MeshPassDrawListContext result;
            processor.PrepareRecord(desc, scene, record, result);
            if (!AppendPreparedCommand(desc, visible, record.Batch, record.Queue, record.ProgramFrameId, record.Material, result, out)) return false;
        }
        if (!matchedPass && primitive.MeshBatchCount != 0) {
            out.Stats.ConsideredBatches += primitive.MeshBatchCount;
            out.Stats.MissingPass += primitive.MeshBatchCount;
            if (desc.RequireMaterialPass) out.Stats.MissingRequiredPass += primitive.MeshBatchCount;
        }
    }
    FinishList(desc, out);
    return true;
}

bool EmitFromBatches(const ListTarget& target, MeshPassProcessor& processor) {
    const auto& desc = *target.Desc;
    const auto& scene = *desc.Culling->Scene.Get();
    auto& out = *target.Out;
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
                if (desc.RequireMaterialPass) ++out.Stats.MissingRequiredPass;
                continue;
            }
            if (!pass->Valid || !pass->Program) {
                ++out.Stats.InvalidBindings;
                continue;
            }
            MeshPassDrawListContext result;
            processor.AddMeshBatch(desc, scene, batch, result);
            if (!AppendPreparedCommand(desc, visible, batchIndex, material.Queue, pass->ProgramFrameId, batch.Material, result, out)) return false;
        }
    }
    FinishList(desc, out);
    return true;
}

bool EmitTargets(std::span<ListTarget> targets, MeshPassProcessor& processor, bool records) {
    for (auto& target : targets) {
        const bool ok = records ? EmitFromRecords(target, processor) : EmitFromBatches(target, processor);
        if (!ok) return false;
    }
    return true;
}

bool BuildRendererListsImpl(std::span<const RendererListDesc> descs, MeshPassProcessor& processor, std::span<RendererList*> outs) {
    if (descs.size() != outs.size() || descs.size() > kMaxSharedRendererLists) return false;
    for (auto* out : outs) {
        if (out == nullptr) return false;
        out->ResetForReuse();
    }
    if (descs.empty()) return true;
    for (const auto& desc : descs) {
        if (!DescriptorIsValid(desc) || desc.Culling != descs[0].Culling || desc.View != descs[0].View) {
            for (auto* out : outs) out->ResetForReuse();
            return false;
        }
    }
    const auto& scene = *descs[0].Culling->Scene.Get();
    const bool records = HasDrawRecordTable(scene);
    if (!records && !ValidateVisibleBatches(*descs[0].Culling.Get())) return false;
    array<ListTarget, kMaxSharedRendererLists> storage{};
    const uint32_t listCount = static_cast<uint32_t>(descs.size());
    const size_t visible = descs[0].Culling->Primitives.size();
    for (uint32_t list = 0; list < listCount; ++list) {
        storage[list] = {&descs[list], outs[list], HashPassName(descs[list].MaterialPassName)};
        outs[list]->Commands.reserve(visible);
        outs[list]->Items.reserve(visible);
    }
    auto targets = std::span<ListTarget>{storage.data(), listCount};
    const bool ok = EmitTargets(targets, processor, records);
    if (!ok) ResetTargets(targets);
    return ok;
}

}  // namespace

void MeshPassProcessor::PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                      const DrawRecord& record, MeshPassDrawListContext& out) {
    if (record.Batch >= scene.MeshBatches.size()) {
        out.Reject(MeshPassRejectReason::InvalidGeometry);
        return;
    }
    AddMeshBatch(desc, scene, scene.MeshBatches[record.Batch], out);
}

bool BuildRendererList(const RendererListDesc& desc, MeshPassProcessor& processor, RendererList& out) {
    RADRAY_PROFILE_SCOPE_N("BuildRendererList");
    RendererList* outPtr = &out;
    return BuildRendererListsImpl(std::span<const RendererListDesc>{&desc, 1}, processor, std::span<RendererList*>{&outPtr, 1});
}

bool BuildRendererLists(std::span<const RendererListDesc> descs, MeshPassProcessor& processor, std::span<RendererList*> outs) {
    RADRAY_PROFILE_SCOPE_N("BuildRendererLists");
    return BuildRendererListsImpl(descs, processor, outs);
}

}  // namespace radray
