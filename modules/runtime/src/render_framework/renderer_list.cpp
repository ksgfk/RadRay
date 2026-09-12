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
RendererList::RendererList(const RendererList& other) { *this = other; }
RendererList& RendererList::operator=(const RendererList& other) {
    if (this == &other) return *this;
    Commands = other.Commands;
    Items = other.Items;
    Stats = other.Stats;
    _draws = other._draws;
    _dynamicGeometryPlans = other._dynamicGeometryPlans;
    _programs = other._programs;
    _scene = other._scene;
    _resources = other._resources;
    _frameEpoch = other._frameEpoch;
    _publicationId = other._publicationId;
    _sceneEpoch = other._sceneEpoch;
    _publicationRevision = other._publicationRevision;
    AdvanceRevision();
    return *this;
}
RendererList::RendererList(RendererList&& other) noexcept { *this = std::move(other); }
RendererList& RendererList::operator=(RendererList&& other) noexcept {
    if (this == &other) return *this;
    Commands = std::move(other.Commands);
    Items = std::move(other.Items);
    Stats = other.Stats;
    _draws = std::move(other._draws);
    _dynamicGeometryPlans = std::move(other._dynamicGeometryPlans);
    _programs = std::move(other._programs);
    _scene = other._scene;
    _resources = other._resources;
    _frameEpoch = other._frameEpoch;
    _publicationId = other._publicationId;
    _sceneEpoch = other._sceneEpoch;
    _publicationRevision = other._publicationRevision;
    AdvanceRevision();
    other.ResetForReuse();
    return *this;
}
void RendererList::AdvanceRevision() noexcept {
    if (_buildRevision == UINT64_MAX) RADRAY_ABORT("Renderer list revision exhausted");
    ++_buildRevision;
}
Nullable<const RendererList::Draw*> RendererList::FindDraw(size_t executionIndex) const noexcept {
    return _draws.empty() ? nullptr : &_draws[Items.empty() ? executionIndex : Items[executionIndex].CommandIndex];
}
const MeshDrawDescription& RendererList::GetDescription(size_t index) const noexcept {
    const auto draw = FindDraw(index);
    if (!draw) return Commands[Items.empty() ? index : Items[index].CommandIndex];
    return draw->Dynamic ? static_cast<const MeshDrawDescription&>(Commands[draw->Source]) : _scene->DrawRecords[draw->Source].Description;
}
const MaterialPipelineState& RendererList::GetPipelineState(size_t index) const noexcept {
    const auto draw = FindDraw(index);
    if (!draw || draw->Dynamic) return GetDescription(index).PipelineState;
    const auto& record = _scene->DrawRecords[draw->Source];
    return record.Mirrored ? record.MirroredState : record.Description.PipelineState;
}
RendererDrawGroupsView RendererList::GetGroups(size_t index) const noexcept {
    const auto draw = FindDraw(index);
    if (draw && !draw->Dynamic) return RendererDrawGroupsView{_resources.Get(), draw->Binding};
    const auto& command = GetCommand(index);
    return RendererDrawGroupsView{std::span<const PreparedShaderGroup>{command.Groups.data(), command.Groups.size()}};
}
FrameDrawBindingId RendererList::GetBindingId(size_t index) const noexcept {
    const auto draw = FindDraw(index);
    return draw && !draw->Dynamic ? draw->Binding : FrameDrawBindingId{};
}
uint64_t RendererList::GetEffectiveStateId(size_t index) const noexcept {
    const auto draw = FindDraw(index);
    if (!draw || draw->Dynamic) return 0;
    const auto& record = _scene->DrawRecords[draw->Source];
    return record.Mirrored ? record.MirroredStateId : record.NormalStateId;
}
std::span<const CpuVertexBindingRun> RendererList::GetVertexBindingRuns(size_t index) const noexcept {
    const auto draw = FindDraw(index);
    if (!draw) return {};
    if (draw->Dynamic) return _dynamicGeometryPlans[draw->Source].Runs;
    const auto plan = _scene->DrawRecords[draw->Source].GeometryBindingPlan;
    return plan < _scene->GeometryBindingPlans.size() ? std::span<const CpuVertexBindingRun>{_scene->GeometryBindingPlans[plan].Runs} : std::span<const CpuVertexBindingRun>{};
}
const MeshDrawCommand& RendererList::GetCommand(size_t index) const noexcept {
    const auto draw = FindDraw(index);
    RADRAY_ASSERT(!draw || draw->Dynamic);
    return Commands[draw ? draw->Source : Items.empty() ? index
                                                        : Items[index].CommandIndex];
}
bool RendererList::IsCurrent() const noexcept {
    return (!_resources || _resources->GetEpoch() == _frameEpoch) &&
           (!_scene || (_scene->PublicationId == _publicationId && _scene->SceneEpoch == _sceneEpoch &&
                        _scene->PublicationRevision == _publicationRevision && _scene->DrawRecords.size() != 0));
}
void RendererList::ResetForReuse() noexcept {
    Items.clear();
    Commands.clear();
    _draws.clear();
    _programs.clear();
    _dynamicGeometryPlans.clear();
    Stats = {};
    _scene = nullptr;
    _resources = nullptr;
    _frameEpoch = _publicationId = _sceneEpoch = _publicationRevision = 0;
    AdvanceRevision();
}
bool RendererList::AppendStatic(const RenderSceneSnapshot& scene, uint32_t recordIndex, FrameDrawResources& resources, FrameDrawBindingId binding) {
    if (!IsCurrent() || recordIndex >= scene.DrawRecords.size() || !resources.IsValid(binding) ||
        (_scene && _scene.Get() != &scene) || (_resources && (_resources.Get() != &resources || _frameEpoch != resources.GetEpoch()))) return false;
    _scene = &scene;
    _resources = &resources;
    _frameEpoch = resources.GetEpoch();
    _publicationId = scene.PublicationId;
    _sceneEpoch = scene.SceneEpoch;
    _publicationRevision = scene.PublicationRevision;
    _draws.push_back({recordIndex, binding, false});
    AdvanceRevision();
    return true;
}
bool RendererList::AppendDynamic(MeshDrawCommand&& command, Nullable<FrameDrawResources*> resources) {
    if (!IsCurrent()) return false;
    if (resources) {
        if (_resources && (_resources.Get() != resources.Get() || _frameEpoch != resources->GetEpoch())) return false;
        _resources = resources.Get();
        _frameEpoch = resources->GetEpoch();
    }
    CpuGeometryBindingPlan plan;
    if (command.Geometry) {
        const auto& buffers = command.Geometry->VertexBuffers;
        for (uint32_t first = 0; first < buffers.size();) {
            uint32_t end = first + 1;
            while (end < buffers.size() && uint64_t{buffers[end - 1].Binding} + 1 == buffers[end].Binding) ++end;
            plan.Runs.push_back({first, end - first});
            first = end;
        }
    }
    _draws.push_back({static_cast<uint32_t>(Commands.size()), {}, true});
    _dynamicGeometryPlans.push_back(std::move(plan));
    Commands.push_back(std::move(command));
    AdvanceRevision();
    return true;
}
void RendererList::AddProgram(RendererListProgramUse use) {
    if (std::none_of(_programs.begin(), _programs.end(), [&](const auto& value) { return value.Program == use.Program && value.ProgramGeneration == use.ProgramGeneration && value.Bindings == use.Bindings; })) {
        _programs.push_back(use);
        AdvanceRevision();
    }
}

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
    return scene.PrimitiveDrawBegin.size() == scene.Primitives.size() + 1;
}

bool DescriptorIsValid(const RendererListDesc& desc) noexcept {
    return desc.Culling && desc.View && desc.Culling->Scene && desc.Culling->Stats.Valid &&
           desc.Culling->View == desc.View && desc.QueueRange.Min <= desc.QueueRange.Max && !desc.MaterialPassName.empty();
}

bool AppendPreparedCommand(const RendererListDesc& desc, const VisiblePrimitive& visible, MeshBatchIndex batchIndex,
                           RenderQueue queue, uint32_t programFrameId, RenderMaterialIndex material,
                           MeshPassDrawListContext& result, RendererList& out, const RenderSceneSnapshot& scene, uint32_t recordIndex) {
    if (!result.HasDraw()) {
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
    if (out.GetDrawCount() >= std::numeric_limits<uint32_t>::max()) return false;
    out.Items.push_back({{queue, programFrameId, material, depth, visible.Primitive, batchIndex}, static_cast<uint32_t>(out.GetDrawCount())});
    return result.AppendTo(out, scene, recordIndex);
}

void FinishList(const RendererListDesc& desc, RendererList& out) {
    SortRendererListItems(desc, out);
    out.Stats.Commands = out.GetDrawCount();
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
        if (begin > end || end > scene.DrawRecords.size()) return false;
        bool matchedPass = false;
        for (uint32_t index = begin; index < end; ++index) {
            const auto& record = scene.DrawRecords[index];
            if (record.Policy != desc.Policy || record.PassNameHash != target.PassHash) continue;
            if (record.Material >= scene.Materials.size() || record.PassIndex >= scene.Materials[record.Material].Passes.size()) return false;
            if (scene.Materials[record.Material].Passes[record.PassIndex].PassName != desc.MaterialPassName) continue;
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
            const auto previousDraws = out.GetDrawCount();
            if (!AppendPreparedCommand(desc, visible, record.Batch, record.Queue, record.ProgramFrameId, record.Material, result, out, scene, index)) return false;
            if (out.GetDrawCount() != previousDraws) {
                const auto& pass = scene.Materials[record.Material].Passes[record.PassIndex];
                const auto program = out.GetDescription(previousDraws).Program;
                const bool indexed = out.GetBindingId(previousDraws).IsValid();
                out.AddProgram({program, program == pass.Program ? pass.ProgramGeneration : 0,
                                indexed && record.BindingRecipe < scene.BindingRecipes.size() ? &scene.BindingRecipes[record.BindingRecipe] : nullptr});
            }
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
        if (visible.Primitive >= scene.Primitives.size()) return false;
        const auto& primitive = scene.Primitives[visible.Primitive];
        if (primitive.FirstMeshBatch > scene.MeshBatches.size() ||
            primitive.MeshBatchCount > scene.MeshBatches.size() - primitive.FirstMeshBatch)
            return false;
        for (uint32_t offset = 0; offset < primitive.MeshBatchCount; ++offset) {
            ++out.Stats.ConsideredBatches;
            if (!(primitive.LayerMask & desc.LayerMask)) {
                ++out.Stats.LayerRejected;
                continue;
            }
            const auto batchIndex = primitive.FirstMeshBatch + offset;
            const auto& batch = scene.MeshBatches[batchIndex];
            if (batch.Primitive != visible.Primitive || batch.Material >= scene.Materials.size()) return false;
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
            const auto previousDraws = out.GetDrawCount();
            if (!AppendPreparedCommand(desc, visible, batchIndex, material.Queue, pass->ProgramFrameId, batch.Material, result, out, scene, UINT32_MAX)) return false;
            if (out.GetDrawCount() != previousDraws) {
                const auto program = out.GetDescription(previousDraws).Program;
                out.AddProgram({program, program == pass->Program ? pass->ProgramGeneration : 0, nullptr});
            }
        }
    }
    FinishList(desc, out);
    return true;
}

bool EmitTargets(std::span<ListTarget> targets, MeshPassProcessor& processor, bool records) {
    for (auto& target : targets) {
        const auto& scene = *target.Desc->Culling->Scene;
        const bool compatibleRecords = records && scene.HasPassPolicies == target.Desc->Policy.IsValid();
        const bool ok = compatibleRecords ? EmitFromRecords(target, processor) : EmitFromBatches(target, processor);
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
    array<ListTarget, kMaxSharedRendererLists> storage{};
    const uint32_t listCount = static_cast<uint32_t>(descs.size());
    const size_t visible = descs[0].Culling->Primitives.size();
    for (uint32_t list = 0; list < listCount; ++list) {
        storage[list] = {&descs[list], outs[list], HashPassName(descs[list].MaterialPassName)};
        outs[list]->Items.reserve(visible);
    }
    auto targets = std::span<ListTarget>{storage.data(), listCount};
    if (!EmitTargets(targets, processor, records)) {
        ResetTargets(targets);
        return false;
    }
    return true;
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
