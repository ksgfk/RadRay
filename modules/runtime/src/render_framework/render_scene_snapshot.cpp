#include <radray/runtime/render_framework/render_scene_snapshot.h>

#include <algorithm>
#include <limits>
#include <radray/logger.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/shader_program.h>

namespace radray {

void RenderSceneSnapshot::ResetForReuse() noexcept {
    const auto previous = Stats;
    Primitives.clear();
    MeshBatches.clear();
    Materials.clear();
    Lights.clear();
    DrawRecords.clear();
    PrimitiveDrawBegin.clear();
    Stats = {};
    Stats.PrimitiveHighWatermark = previous.PrimitiveHighWatermark;
    Stats.BatchHighWatermark = previous.BatchHighWatermark;
    Stats.MaterialHighWatermark = previous.MaterialHighWatermark;
    Stats.LightHighWatermark = previous.LightHighWatermark;
}

bool BuildRenderSceneSnapshot(const Scene& scene, RenderSceneSnapshot& out, vector<StreamingAssetRefAny>& retainedAssets) {
    RenderSceneSnapshotBuilder builder;
    return builder.Build(scene, out, retainedAssets);
}

bool RenderSceneSnapshotBuilder::Build(const Scene& scene, RenderSceneSnapshot& out, vector<StreamingAssetRefAny>& retainedAssets) {
    if (++_epoch == 0) {
        _primitives.clear();
        _materials.clear();
        _programs.clear();
        ++_epoch;
    }
    RenderSceneSnapshot next = std::move(out);
    out = {};
    auto primitiveStorage = std::move(next.Primitives);
    auto materialStorage = std::move(next.Materials);
    next.ResetForReuse();
    next.Primitives = std::move(primitiveStorage);
    next.Materials = std::move(_materialScratch);
    for (size_t index = 0; index < materialStorage.size(); ++index) {
        const auto generation = materialStorage[index].Generation;
        if (generation == 0) continue;
        auto [entry, created] = _materials.try_emplace(generation);
        next.Stats.ScratchEntriesCreated += created ? 1 : 0;
        entry->second.StorageEpoch = _epoch;
        entry->second.StorageIndex = index;
    }
    uint32_t programCount = 0;
    const auto ownerStart = retainedAssets.size();
    struct Rollback {
        vector<StreamingAssetRefAny>& Owners;
        size_t Start;
        bool Success{false};
        ~Rollback() {
            if (!Success) Owners.resize(Start);
        }
    } rollback{retainedAssets, ownerStart};
    constexpr size_t kMaxIndex = std::numeric_limits<uint32_t>::max();
    _primitives.reserve(scene.Primitives().size());
    next.Primitives.reserve(scene.Primitives().size());
    size_t primitiveCount = 0;
    // Only a membership/order change needs generation lookup. Stable slots never visit a hash table.
    std::optional<unordered_map<uint64_t, size_t>> reorderedPrimitives;
    for (const auto& proxy : scene.Primitives()) {
        if (proxy) proxy->CollectAssetReferences(retainedAssets);
    }
    for (const auto& proxy : scene.Primitives()) {
        ++next.Stats.InputPrimitives;
        if (!proxy) continue;
        if (primitiveCount >= kMaxIndex) return false;
        const uint64_t generation = proxy->GetGeneration();
        bool created = false;
        if (primitiveCount == _primitives.size()) {
            _primitives.emplace_back();
            _primitives.back().Generation = generation;
            created = true;
        } else if (_primitives[primitiveCount].Generation != generation) {
            if (!reorderedPrimitives) {
                reorderedPrimitives.emplace();
                reorderedPrimitives->reserve(_primitives.size() - primitiveCount);
                for (size_t index = primitiveCount; index < _primitives.size(); ++index)
                    reorderedPrimitives->emplace(_primitives[index].Generation, index);
            }
            const auto found = reorderedPrimitives->find(generation);
            const size_t oldIndex = found == reorderedPrimitives->end() ? _primitives.size() : found->second;
            if (found == reorderedPrimitives->end()) {
                _primitives.emplace_back();
                _primitives.back().Generation = generation;
                created = true;
            } else {
                reorderedPrimitives->erase(found);
            }
            std::swap(_primitives[primitiveCount], _primitives[oldIndex]);
            (*reorderedPrimitives)[_primitives[oldIndex].Generation] = oldIndex;
        }
        next.Stats.ScratchEntriesCreated += created ? 1 : 0;
        auto& cachedPrimitive = _primitives[primitiveCount];
        const uint64_t revision = proxy->GetRenderDataRevision();
        const bool rebuild = created || revision == 0 || cachedPrimitive.Revision != revision;
        if (rebuild) {
            cachedPrimitive.Revision = revision;
            cachedPrimitive.LocalBounds = proxy->GetLocalBounds();
            cachedPrimitive.Sections.clear();
            const uint32_t sectionCount = proxy->GetSectionCount();
            for (uint32_t section = 0; section < sectionCount; ++section) {
                auto& item = cachedPrimitive.Sections.emplace_back();
                item.Draw = proxy->GetDrawArgs(section);
                const auto& args = item.Draw;
                if (!args.Geometry) {
                    item.Status = SectionStatus::MissingGeometry;
                } else if (!args.IndexCount) {
                    item.Status = SectionStatus::EmptyDraw;
                } else {
                    const auto& ib = args.Geometry->Ibv;
                    const bool invalid = args.FirstIndex > std::numeric_limits<uint32_t>::max() - args.IndexCount ||
                                         (ib.Target && (ib.Stride == 0 || ib.Offset > ib.Target->GetDesc().Size ||
                                                        uint64_t{args.FirstIndex} + args.IndexCount > (ib.Target->GetDesc().Size - ib.Offset) / ib.Stride));
                    item.Status = invalid ? SectionStatus::InvalidDrawRange : SectionStatus::Valid;
                }
            }
            ++next.Stats.PrimitiveStructuresRebuilt;
        } else {
            ++next.Stats.PrimitiveStructuresReused;
        }
        if (primitiveCount == next.Primitives.size()) next.Primitives.emplace_back();
        auto& primitive = next.Primitives[primitiveCount];
        primitive.Id = scene.GetPrimitiveId(proxy.get());
        const uint64_t transformRevision = proxy->GetTransformRevision();
        const Eigen::Matrix4f localToWorld = proxy->GetLocalToWorld();
        if (rebuild || transformRevision == 0 || primitive.Generation != generation ||
            primitive.RenderDataRevision != revision || primitive.TransformRevision != transformRevision) {
            if (!rebuild && primitive.Generation == generation && primitive.RenderDataRevision == revision &&
                (primitive.LocalToWorld.array() == localToWorld.array()).all()) {
                ++next.Stats.EqualValueIgnored;
            } else {
                ++next.Stats.AppliedTransforms;
            }
            primitive.LocalToWorld = localToWorld;
            primitive.WorldBounds = TransformBounds(cachedPrimitive.LocalBounds, primitive.LocalToWorld);
            ++next.Stats.PrimitiveBoundsRebuilt;
        } else {
            ++next.Stats.PrimitiveBoundsReused;
        }
        primitive.Generation = generation;
        primitive.MotionRevision = proxy->GetMotionRevision();
        primitive.RenderDataRevision = revision;
        primitive.TransformRevision = transformRevision;
        primitive.LayerMask = proxy->GetLayerMask();
        primitive.DisableFrustumCulling = proxy->IsFrustumCullingDisabled();
        primitive.FirstMeshBatch = static_cast<uint32_t>(next.MeshBatches.size());
        primitive.MeshBatchCount = 0;
        if (!primitive.WorldBounds.IsFiniteValid()) ++next.Stats.InvalidBounds;
        const auto primitiveIndex = static_cast<uint32_t>(primitiveCount);
        for (uint32_t section = 0; section < cachedPrimitive.Sections.size(); ++section) {
            ++next.Stats.InputSections;
            const auto& cachedSection = cachedPrimitive.Sections[section];
            const auto& args = cachedSection.Draw;
            if (cachedSection.Status == SectionStatus::MissingGeometry) {
                ++next.Stats.MissingGeometry;
                continue;
            }
            if (cachedSection.Status == SectionStatus::EmptyDraw) {
                ++next.Stats.EmptyDraw;
                continue;
            }
            if (cachedSection.Status == SectionStatus::InvalidDrawRange) {
                ++next.Stats.InvalidDrawRange;
                continue;
            }
            const auto material = proxy->GetMaterial(section);
            if (!material) {
                ++next.Stats.MaterialUnavailable;
                continue;
            }
            auto [found, inserted] = _materials.try_emplace(material->GetGeneration());
            next.Stats.ScratchEntriesCreated += inserted ? 1 : 0;
            if (found->second.Epoch != _epoch) {
                auto& entry = found->second;
                entry.Epoch = _epoch;
                entry.Index.reset();
                ++next.Stats.InputMaterials;
                if (next.Materials.size() >= kMaxIndex) return false;
                auto& data = entry.StorageEpoch == _epoch ? materialStorage[entry.StorageIndex] : entry.Unpublished;
                const uint64_t previousGeneration = data.Generation, previousRevision = data.Revision;
                uint64_t bytesCopied = 0;
                const bool valid = material->BuildRenderData(data, retainedAssets, &bytesCopied);
                next.Stats.MaterialBytesCopied += bytesCopied;
                if (previousGeneration == data.Generation && previousRevision == data.Revision)
                    ++next.Stats.MaterialsReused;
                else
                    ++next.Stats.MaterialsRebuilt;
                if (valid) {
                    for (auto& pass : data.Passes) {
                        if (!pass.Program) continue;
                        auto [program, created2] = _programs.try_emplace(pass.Program.Get());
                        next.Stats.ScratchEntriesCreated += created2 ? 1 : 0;
                        if (program->second.Epoch != _epoch) {
                            if (programCount == kMaxIndex) return false;
                            program->second = {_epoch, programCount++};
                        }
                        pass.ProgramFrameId = *program->second.Index;
                    }
                    entry.Index = static_cast<uint32_t>(next.Materials.size());
                    next.Materials.push_back(std::move(data));
                    data = {};
                } else if (&data != &entry.Unpublished) {
                    entry.Unpublished = std::move(data);
                    data = {};
                }
            }
            if (!found->second.Index) {
                ++next.Stats.MaterialUnavailable;
                continue;
            }
            if (next.MeshBatches.size() >= kMaxIndex) return false;
            next.MeshBatches.push_back({primitiveIndex, *found->second.Index, args.Geometry, args.FirstIndex, args.IndexCount, args.VertexOffset, section});
            ++primitive.MeshBatchCount;
        }
        ++primitiveCount;
    }
    _primitives.resize(primitiveCount);
    next.Primitives.resize(primitiveCount);
    for (const auto& light : scene.Lights()) {
        ++next.Stats.InputLights;
        if (!light || !light->AffectsWorld()) continue;
        if (next.Lights.size() >= kMaxIndex) return false;
        RenderLightData data;
        data.Type = light->GetLightType();
        light->GetLightRenderParameters(data.Parameters);
        data.WorldBounds = {data.Parameters.WorldPosition, light->GetRadius()};
        data.LayerMask = light->GetLayerMask();
        data.CastShadow = light->CastShadow();
        next.Lights.push_back(std::move(data));
    }
    _materialScratch = std::move(materialStorage);
    _materialScratch.clear();
    std::erase_if(_materials, [&](const auto& entry) { return entry.second.Epoch != _epoch; });
    std::erase_if(_programs, [&](const auto& entry) { return entry.second.Epoch != _epoch; });
    next.Stats.Primitives = next.Primitives.size();
    next.Stats.MeshBatches = next.MeshBatches.size();
    next.Stats.Materials = next.Materials.size();
    next.Stats.Lights = next.Lights.size();
    next.Stats.RetainedAssets = retainedAssets.size() - ownerStart;
    if (!_draws.Sync(next)) return false;
    next.Stats.PrimitiveHighWatermark = std::max(next.Stats.PrimitiveHighWatermark, next.Primitives.capacity());
    next.Stats.BatchHighWatermark = std::max(next.Stats.BatchHighWatermark, next.MeshBatches.capacity());
    next.Stats.MaterialHighWatermark = std::max(next.Stats.MaterialHighWatermark, next.Materials.capacity());
    next.Stats.LightHighWatermark = std::max(next.Stats.LightHighWatermark, next.Lights.capacity());
    rollback.Success = true;
    out = std::move(next);
    return true;
}

}  // namespace radray
