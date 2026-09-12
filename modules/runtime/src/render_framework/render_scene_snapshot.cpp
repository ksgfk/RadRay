#include <radray/runtime/render_framework/render_scene_snapshot.h>
#include "render_memory_measure.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>

#include <radray/profiler.h>
#include <radray/runtime/render_framework/scene.h>

namespace radray {
namespace {

enum class SnapshotTable : uint8_t { Primitives,
                                     Batches,
                                     Materials,
                                     Lights,
                                     Draws,
                                     DrawBegin,
                                     Bindings,
                                     GeometryPlans,
                                     Count };
constexpr size_t kTableCount = static_cast<size_t>(SnapshotTable::Count);
constexpr uint32_t kPageEntries[kTableCount]{32, 64, 1, 16, 16, 256, 32, 32};
std::atomic<uint64_t> gNextSceneState{1};
std::atomic<uint64_t> gNextPublication{1};
std::atomic<uint64_t> gNextLegacyEpoch{uint64_t{1} << 63};

struct PendingPages {
    vector<uint8_t> Marked;
    vector<uint32_t> Pages;
    void Mark(uint32_t first, uint32_t count, uint32_t pageEntries) {
        if (count == 0) return;
        const uint32_t begin = first / pageEntries;
        const uint32_t end = (first + count - 1) / pageEntries + 1;
        if (Marked.size() < end) Marked.resize(end);
        for (uint32_t page = begin; page < end; ++page) {
            if (Marked[page]) continue;
            Pages.push_back(page);
            Marked[page] = 1;
        }
    }
    void Clear() noexcept {
        for (const uint32_t page : Pages) Marked[page] = 0;
        Pages.clear();
    }
};

uint64_t MaterialPayloadBytes(const MaterialRenderData& material) {
    uint64_t bytes = sizeof(material);
    for (const auto& pass : material.Passes)
        bytes += sizeof(pass) + pass.PassName.size() + pass.NumericBytes.size() + pass.Textures.size() * sizeof(MaterialTextureFrameData) + pass.Samplers.size() * sizeof(MaterialSamplerFrameData);
    return bytes;
}

void MeasureMaterialPayload(RenderMemoryStats& result, const MaterialRenderData& material) noexcept {
    detail::MeasureVector(result, material.Passes, true);
    for (const auto& pass : material.Passes) {
        detail::MeasureString(result, pass.PassName);
        detail::MeasureVector(result, pass.NumericBytes, true);
        detail::MeasureVector(result, pass.Textures, true);
        detail::MeasureVector(result, pass.Samplers, true);
    }
}

bool EqualLight(const RenderLightData& left, const RenderLightData& right) noexcept {
    const auto& a = left.Parameters;
    const auto& b = right.Parameters;
    return left.Type == right.Type && left.LayerMask == right.LayerMask && left.CastShadow == right.CastShadow &&
           left.WorldBounds.Radius == right.WorldBounds.Radius && (left.WorldBounds.Center.array() == right.WorldBounds.Center.array()).all() &&
           (a.WorldPosition.array() == b.WorldPosition.array()).all() && (a.Color.array() == b.Color.array()).all() &&
           (a.Direction.array() == b.Direction.array()).all() && (a.Tangent.array() == b.Tangent.array()).all() &&
           (a.SpotAngles.array() == b.SpotAngles.array()).all() && a.InvRadius == b.InvRadius && a.FalloffExponent == b.FalloffExponent &&
           a.SpecularScale == b.SpecularScale && a.DiffuseScale == b.DiffuseScale && a.SourceRadius == b.SourceRadius && a.SoftSourceRadius == b.SoftSourceRadius && a.SourceLength == b.SourceLength;
}

}  // namespace

struct SceneSnapshotPublication {
    uint64_t Owner{0};
    array<PendingPages, kTableCount> Tables;
    bool Initialized{false};
};

RenderMemoryStats MeasureRenderSceneSnapshot(const RenderSceneSnapshot& snapshot) noexcept {
    RenderMemoryStats result;
    result.ObjectBytes = sizeof(snapshot);
    result.LiveEntries = snapshot.Primitives.size() + snapshot.MeshBatches.size() + snapshot.Materials.size() + snapshot.Lights.size() + snapshot.DrawRecords.size();
    detail::MeasureVector(result, snapshot.Primitives);
    detail::MeasureVector(result, snapshot.MeshBatches);
    detail::MeasureVector(result, snapshot.Materials);
    detail::MeasureVector(result, snapshot.Lights);
    detail::MeasureVector(result, snapshot.DrawRecords);
    detail::MeasureVector(result, snapshot.BindingRecipes);
    detail::MeasureVector(result, snapshot.GeometryBindingPlans);
    detail::MeasureVector(result, snapshot.PrimitiveDrawBegin);
    detail::MeasureVector(result, snapshot.ChangedPrimitiveRanges);
    for (const auto& material : snapshot.Materials) MeasureMaterialPayload(result, material);
    for (const auto& geometry : snapshot.GeometryBindingPlans) detail::MeasureVector(result, geometry.Runs, true);
    return result;
}

RenderSceneSnapshot::RenderSceneSnapshot(const RenderSceneSnapshot& other) { *this = other; }
RenderSceneSnapshot& RenderSceneSnapshot::operator=(const RenderSceneSnapshot& other) {
    if (this == &other) return *this;
    Primitives = other.Primitives;
    MeshBatches = other.MeshBatches;
    Materials = other.Materials;
    Lights = other.Lights;
    DrawRecords = other.DrawRecords;
    BindingRecipes = other.BindingRecipes;
    GeometryBindingPlans = other.GeometryBindingPlans;
    PrimitiveDrawBegin = other.PrimitiveDrawBegin;
    ChangedPrimitiveRanges = other.ChangedPrimitiveRanges;
    SceneEpoch = other.SceneEpoch;
    PublicationId = 0;
    PublicationRevision = ChangedFromPublicationRevision = 0;
    Valid = other.Valid;
    HasPassPolicies = other.HasPassPolicies;
    Stats = other.Stats;
    _publication.reset();
    return *this;
}

void RenderSceneSnapshot::ResetForReuse() noexcept {
    Primitives.clear();
    MeshBatches.clear();
    Materials.clear();
    Lights.clear();
    DrawRecords.clear();
    BindingRecipes.clear();
    GeometryBindingPlans.clear();
    PrimitiveDrawBegin.clear();
    ChangedPrimitiveRanges.clear();
    SceneEpoch = 0;
    PublicationId = 0;
    PublicationRevision = ChangedFromPublicationRevision = 0;
    Valid = false;
    HasPassPolicies = false;
    _publication.reset();
    const auto previous = Stats;
    Stats = {};
    Stats.PrimitiveHighWatermark = previous.PrimitiveHighWatermark;
    Stats.BatchHighWatermark = previous.BatchHighWatermark;
    Stats.MaterialHighWatermark = previous.MaterialHighWatermark;
    Stats.LightHighWatermark = previous.LightHighWatermark;
}

struct SceneRenderState::Impl {
    enum class SectionStatus : uint8_t { Valid,
                                         MissingGeometry,
                                         EmptyDraw,
                                         InvalidDrawRange };
    struct Section {
        MeshDrawArgs Draw;
        uint64_t MaterialGeneration{0};
        SectionStatus Status{SectionStatus::MissingGeometry};
    };
    struct PrimitiveEntry {
        SceneObjectId Id{};
        uint32_t Packed{0};
        AxisAlignedBounds LocalBounds;
        vector<Section> Sections;
        uint64_t UnavailableMaterials{0};
        bool WorkPending{false}, BatchDirty{false}, DrawDirty{false};
    };
    struct MaterialEntry {
        Nullable<Material*> Source{nullptr};
        std::optional<uint32_t> Index;
        MaterialRenderData Unpublished;
        unordered_map<uint32_t, uint32_t> Users;
        MaterialDirtyFlags Pending;
        bool Enqueued{false}, Observed{false}, UnusedQueued{false};
    };
    struct ProgramEntry {
        uint32_t Index{0}, Users{0};
    };
    struct SharedFlight {
        shared_ptr<RenderSceneSnapshot> Snapshot{make_shared<RenderSceneSnapshot>()};
        std::optional<uint64_t> Serial;
        Nullable<vector<StreamingAssetRefAny>*> OwnerSink{nullptr};
        size_t OwnerEnd{0};
    };
    uint64_t Generation{gNextSceneState.fetch_add(1, std::memory_order_relaxed)};
    RenderSceneSnapshot Canonical;
    vector<PrimitiveEntry> Primitives;
    vector<SceneObjectId> PackedIds;
    unordered_map<uint64_t, MaterialEntry> Materials;
    vector<uint64_t> MaterialIds, PendingMaterials, ObservedMaterials, UnusedMaterials;
    unordered_map<uint64_t, ProgramEntry> Programs;
    vector<uint32_t> FreeProgramIds;
    uint32_t NextProgramId{0};
    vector<uint32_t> WorkSlots, DrawIndices;
    vector<weak_ptr<SceneSnapshotPublication>> Publications;
    vector<SharedFlight> SharedFlights;
    std::optional<uint64_t> CommittedSerial;
    uint64_t EpochPublicationId{0};
    Nullable<vector<StreamingAssetRefAny>*> EpochOwnerSink{nullptr};
    size_t EpochOwnerEnd{0};
    std::optional<SnapshotPublicationFailure> PublicationFailure;
    uint64_t Sections{0}, MissingGeometry{0}, EmptyDraws{0}, InvalidRanges{0}, UnavailableMaterials{0}, InvalidBounds{0};
    bool LayoutChanged{false};

    ~Impl() {
        for (auto& [id, entry] : Materials)
            if (entry.Source) entry.Source->RemoveChangeListener(this);
    }
    void Mark(SnapshotTable table, uint32_t first, uint32_t count) {
        for (const auto& weak : Publications)
            if (const auto publication = weak.lock())
                publication->Tables[static_cast<size_t>(table)].Mark(first, count, kPageEntries[static_cast<size_t>(table)]);
    }
    void QueueWork(uint32_t slot, bool batches, bool draws) {
        auto& entry = Primitives[slot];
        entry.BatchDirty |= batches;
        entry.DrawDirty |= draws;
        if (!entry.WorkPending) {
            entry.WorkPending = true;
            WorkSlots.push_back(slot);
        }
    }
    static void MaterialChanged(void* context, Material& material, MaterialDirtyFlags flags) noexcept {
        auto& self = *static_cast<Impl*>(context);
        const auto found = self.Materials.find(material.GetGeneration());
        if (found == self.Materials.end()) return;
        auto& entry = found->second;
        entry.Pending |= flags;
        if (flags.HasFlag(MaterialDirtyKind::Removed)) entry.Source = nullptr;
        if (!entry.Enqueued) {
            entry.Enqueued = true;
            self.PendingMaterials.push_back(found->first);
        }
    }
    void AddMaterialUse(Material* material, uint32_t slot) {
        const auto generation = material->GetGeneration();
        auto [found, created] = Materials.try_emplace(generation);
        auto& entry = found->second;
        if (created) {
            entry.Source = material;
            material->AddChangeListener(this, MaterialChanged);
            entry.Pending = MaterialDirtyKind::Structure | MaterialDirtyKind::Values | MaterialDirtyKind::Bindings;
            entry.Enqueued = true;
            PendingMaterials.push_back(generation);
        }
        ++entry.Users[slot];
    }
    void ReleasePrograms(const MaterialRenderData& data) {
        for (const auto& pass : data.Passes) {
            const auto found = Programs.find(pass.ProgramGeneration);
            if (found == Programs.end()) continue;
            if (--found->second.Users == 0) {
                FreeProgramIds.push_back(found->second.Index);
                Programs.erase(found);
            }
        }
    }
    void AcquirePrograms(MaterialRenderData& data) {
        for (auto& pass : data.Passes) {
            if (!pass.Program) continue;
            auto [found, created] = Programs.try_emplace(pass.ProgramGeneration);
            if (created) {
                if (FreeProgramIds.empty())
                    found->second.Index = NextProgramId++;
                else {
                    found->second.Index = FreeProgramIds.back();
                    FreeProgramIds.pop_back();
                }
            }
            ++found->second.Users;
            pass.ProgramFrameId = found->second.Index;
        }
    }
    void RemovePublishedMaterial(MaterialEntry& entry, bool releasePrograms = true) {
        if (!entry.Index) return;
        const auto index = *entry.Index;
        if (releasePrograms) ReleasePrograms(Canonical.Materials[index]);
        if (index + 1 != Canonical.Materials.size()) {
            ++Canonical.Stats.MaterialPayloadMoves;
            Canonical.Stats.MovedMaterialPayloadBytes += MaterialPayloadBytes(Canonical.Materials.back()) - sizeof(MaterialRenderData);
            Canonical.Materials[index] = std::move(Canonical.Materials.back());
            MaterialIds[index] = MaterialIds.back();
            auto& moved = Materials.at(MaterialIds[index]);
            moved.Index = index;
            for (const auto& [slot, count] : moved.Users) QueueWork(slot, true, true);
            Mark(SnapshotTable::Materials, index, 1);
        }
        Canonical.Materials.pop_back();
        MaterialIds.pop_back();
        entry.Index.reset();
    }
    void RemoveMaterialUse(uint64_t generation, uint32_t slot) {
        if (generation == 0) return;
        const auto found = Materials.find(generation);
        if (found == Materials.end()) return;
        auto& entry = found->second;
        const auto user = entry.Users.find(slot);
        if (user != entry.Users.end() && --user->second == 0) entry.Users.erase(user);
        if (entry.Users.empty() && !entry.UnusedQueued) {
            entry.UnusedQueued = true;
            UnusedMaterials.push_back(generation);
        }
    }
    void RemoveUnusedMaterials() {
        for (const auto generation : UnusedMaterials) {
            const auto found = Materials.find(generation);
            if (found == Materials.end()) continue;
            auto& entry = found->second;
            entry.UnusedQueued = false;
            if (!entry.Users.empty()) continue;
            if (entry.Source) entry.Source->RemoveChangeListener(this);
            RemovePublishedMaterial(entry);
            if (entry.Observed) std::erase(ObservedMaterials, generation);
            Materials.erase(found);
        }
        UnusedMaterials.clear();
    }
    void RemoveSectionCounters(const PrimitiveEntry& entry) {
        Sections -= entry.Sections.size();
        for (const auto& section : entry.Sections) {
            MissingGeometry -= section.Status == SectionStatus::MissingGeometry;
            EmptyDraws -= section.Status == SectionStatus::EmptyDraw;
            InvalidRanges -= section.Status == SectionStatus::InvalidDrawRange;
        }
    }
    void RemovePrimitive(SceneObjectId id) {
        if (id.Slot >= Primitives.size() || Primitives[id.Slot].Id != id) return;
        auto& entry = Primitives[id.Slot];
        for (const auto& section : entry.Sections) RemoveMaterialUse(section.MaterialGeneration, id.Slot);
        RemoveSectionCounters(entry);
        UnavailableMaterials -= entry.UnavailableMaterials;
        const auto packed = entry.Packed;
        if (entry.WorkPending) std::erase(WorkSlots, id.Slot);
        InvalidBounds -= !Canonical.Primitives[packed].WorldBounds.IsFiniteValid();
        Canonical.Primitives.erase(Canonical.Primitives.begin() + packed);
        PackedIds.erase(PackedIds.begin() + packed);
        entry = {};
        for (uint32_t index = packed; index < PackedIds.size(); ++index) Primitives[PackedIds[index].Slot].Packed = index;
        Mark(SnapshotTable::Primitives, packed, static_cast<uint32_t>(PackedIds.size()) - packed);
        LayoutChanged = true;
    }
    bool UpdatePrimitive(const Scene& scene, const ScenePrimitiveChange& change) {
        const auto proxy = scene.FindPrimitive(change.Id);
        if (!proxy) return true;
        if (Primitives.size() <= change.Id.Slot) Primitives.resize(static_cast<size_t>(change.Id.Slot) + 1);
        auto& entry = Primitives[change.Id.Slot];
        const bool created = entry.Id != change.Id;
        if (created) {
            if (Canonical.Primitives.size() == UINT32_MAX) return false;
            entry = {};
            entry.Id = change.Id;
            entry.Packed = static_cast<uint32_t>(Canonical.Primitives.size());
            PackedIds.push_back(change.Id);
            Canonical.Primitives.emplace_back();
            ++InvalidBounds;
            LayoutChanged = true;
        }
        auto& primitive = Canonical.Primitives[entry.Packed];
        const bool structure = created || change.Dirty.HasFlag(PrimitiveDirtyKind::Structure);
        const bool assignment = structure || change.Dirty.HasFlag(PrimitiveDirtyKind::MaterialAssignment);
        if (structure) {
            RemoveSectionCounters(entry);
            const auto count = proxy->GetSectionCount();
            for (size_t index = count; index < entry.Sections.size(); ++index) RemoveMaterialUse(entry.Sections[index].MaterialGeneration, change.Id.Slot);
            entry.Sections.resize(count);
            entry.LocalBounds = proxy->GetLocalBounds();
            for (uint32_t index = 0; index < count; ++index) {
                auto& section = entry.Sections[index];
                section.Draw = proxy->GetDrawArgs(index);
                const auto& draw = section.Draw;
                if (!draw.Geometry)
                    section.Status = SectionStatus::MissingGeometry;
                else if (!draw.IndexCount)
                    section.Status = SectionStatus::EmptyDraw;
                else {
                    const auto& ib = draw.Geometry->Ibv;
                    const bool invalid = draw.FirstIndex > UINT32_MAX - draw.IndexCount ||
                                         (ib.Target && (ib.Stride == 0 || ib.Offset > ib.Target->GetDesc().Size || uint64_t{draw.FirstIndex} + draw.IndexCount > (ib.Target->GetDesc().Size - ib.Offset) / ib.Stride));
                    section.Status = invalid ? SectionStatus::InvalidDrawRange : SectionStatus::Valid;
                }
                MissingGeometry += section.Status == SectionStatus::MissingGeometry;
                EmptyDraws += section.Status == SectionStatus::EmptyDraw;
                InvalidRanges += section.Status == SectionStatus::InvalidDrawRange;
            }
            Sections += count;
            ++Canonical.Stats.PrimitiveStructuresRebuilt;
        }
        if (assignment) {
            for (uint32_t index = 0; index < entry.Sections.size(); ++index) {
                auto& section = entry.Sections[index];
                const auto material = proxy->GetMaterial(index);
                const auto generation = material ? material->GetGeneration() : 0;
                if (generation == section.MaterialGeneration) continue;
                RemoveMaterialUse(section.MaterialGeneration, change.Id.Slot);
                section.MaterialGeneration = generation;
                if (material) AddMaterialUse(material.Get(), change.Id.Slot);
            }
            QueueWork(change.Id.Slot, true, true);
        }
        if (structure || change.Dirty.HasFlag(PrimitiveDirtyKind::TransformOrBounds)) {
            InvalidBounds -= !primitive.WorldBounds.IsFiniteValid();
            primitive.LocalToWorld = proxy->GetLocalToWorld();
            primitive.WorldBounds = TransformBounds(entry.LocalBounds, primitive.LocalToWorld);
            InvalidBounds += !primitive.WorldBounds.IsFiniteValid();
            ++Canonical.Stats.PrimitiveBoundsRebuilt;
            ++Canonical.Stats.AppliedTransforms;
            QueueWork(change.Id.Slot, false, true);
        }
        primitive.Id = change.Id;
        primitive.Generation = proxy->GetGeneration();
        primitive.MotionRevision = proxy->GetMotionRevision();
        primitive.RenderDataRevision = proxy->GetRenderDataRevision();
        primitive.TransformRevision = proxy->GetTransformRevision();
        primitive.LayerMask = proxy->GetLayerMask();
        primitive.DisableFrustumCulling = proxy->IsFrustumCullingDisabled();
        if (change.Dirty.HasFlag(PrimitiveDirtyKind::Filter)) QueueWork(change.Id.Slot, false, true);
        Mark(SnapshotTable::Primitives, entry.Packed, 1);
        return true;
    }
    void ObserveMaterial(Material* material, uint64_t serial) {
        const auto before = material->GetObservationStats();
        material->GetRevisions(serial);
        const auto& after = material->GetObservationStats();
        Canonical.Stats.LegacyMaterialsObserved += after.LegacyMaterialsObserved - before.LegacyMaterialsObserved;
        Canonical.Stats.LegacyBytesCompared += after.LegacyBytesCompared - before.LegacyBytesCompared;
        Canonical.Stats.PendingResourcesObserved += after.PendingResourcesObserved - before.PendingResourcesObserved;
    }
    void UpdateMaterials(uint64_t serial) {
        {
            RADRAY_PROFILE_SCOPE_N("Scene.ObserveLegacy");
            for (const uint64_t generation : ObservedMaterials) {
                auto& entry = Materials.at(generation);
                if (entry.Source) ObserveMaterial(entry.Source.Get(), serial);
            }
            std::erase_if(ObservedMaterials, [&](uint64_t generation) {
                auto& entry = Materials.at(generation);
                if (entry.Source && (entry.Source->HasEscapedWrites() || entry.Source->HasPendingResources())) return false;
                entry.Observed = false;
                return true;
            });
        }
        for (size_t queueIndex = 0; queueIndex < PendingMaterials.size(); ++queueIndex) {
            const uint64_t generation = PendingMaterials[queueIndex];
            const auto found = Materials.find(generation);
            if (found == Materials.end()) continue;
            auto& entry = found->second;
            if (!entry.Enqueued) continue;
            if (entry.Source) ObserveMaterial(entry.Source.Get(), serial);
            const bool wasValid = entry.Index.has_value();
            auto& data = wasValid ? Canonical.Materials[*entry.Index] : entry.Unpublished;
            const auto previousStructure = data.StructureRevision, previousRevision = data.Revision;
            const bool structural = entry.Pending.HasFlag(MaterialDirtyKind::Structure);
            if (wasValid && structural) ReleasePrograms(data);
            uint64_t bytesCopied = 0;
            vector<StreamingAssetRefAny> temporaryOwners;
            const bool valid = entry.Source && entry.Source->BuildRenderData(data, temporaryOwners, &bytesCopied, serial);
            temporaryOwners.clear();
            Canonical.Stats.MaterialBytesCopied += bytesCopied;
            if (data.Revision != previousRevision || !entry.Source) ++Canonical.Stats.MaterialsRebuilt;
            if (valid) {
                if (!wasValid) {
                    entry.Index = static_cast<uint32_t>(Canonical.Materials.size());
                    Canonical.Materials.push_back(std::move(entry.Unpublished));
                    entry.Unpublished = {};
                    MaterialIds.push_back(generation);
                }
                if (!wasValid || structural) AcquirePrograms(Canonical.Materials[*entry.Index]);
                if (data.Revision != previousRevision || !wasValid) Mark(SnapshotTable::Materials, *entry.Index, 1);
            } else if (wasValid) {
                if (!structural) ReleasePrograms(data);
                entry.Unpublished = std::move(data);
                RemovePublishedMaterial(entry, false);
            }
            const auto currentStructure = valid ? Canonical.Materials[*entry.Index].StructureRevision : entry.Unpublished.StructureRevision;
            if (valid != wasValid || structural || previousStructure != currentStructure)
                for (const auto& [slot, count] : entry.Users) QueueWork(slot, true, true);
            const bool observe = entry.Source && (entry.Source->HasEscapedWrites() || entry.Source->HasPendingResources());
            if (observe && !entry.Observed) {
                entry.Observed = true;
                ObservedMaterials.push_back(generation);
            } else if (!observe && entry.Observed) {
                entry.Observed = false;
                std::erase(ObservedMaterials, generation);
            }
            entry.Pending = {};
            entry.Enqueued = false;
        }
        PendingMaterials.clear();
    }
    uint32_t CountBatches(PrimitiveEntry& entry) {
        UnavailableMaterials -= entry.UnavailableMaterials;
        entry.UnavailableMaterials = 0;
        uint32_t count = 0;
        for (const auto& section : entry.Sections) {
            if (section.Status != SectionStatus::Valid) continue;
            const auto material = Materials.find(section.MaterialGeneration);
            if (material == Materials.end() || !material->second.Index)
                ++entry.UnavailableMaterials;
            else
                ++count;
        }
        UnavailableMaterials += entry.UnavailableMaterials;
        return count;
    }
    void WriteBatches(const PrimitiveEntry& entry, uint32_t first) {
        uint32_t offset = 0;
        for (uint32_t sectionIndex = 0; sectionIndex < entry.Sections.size(); ++sectionIndex) {
            const auto& section = entry.Sections[sectionIndex];
            if (section.Status != SectionStatus::Valid) continue;
            const auto material = Materials.find(section.MaterialGeneration);
            if (material == Materials.end() || !material->second.Index) continue;
            const auto& draw = section.Draw;
            Canonical.MeshBatches[first + offset++] = {entry.Packed, *material->second.Index, draw.Geometry, draw.FirstIndex, draw.IndexCount, draw.VertexOffset, sectionIndex};
        }
    }
    bool UpdateBatches(const Scene& scene, RenderValidationMode validation) {
        for (const auto slot : WorkSlots) {
            auto& entry = Primitives[slot];
            if (!entry.Id.IsValid() || !entry.BatchDirty) continue;
            auto& primitive = Canonical.Primitives[entry.Packed];
            const auto count = CountBatches(entry);
            LayoutChanged |= count != primitive.MeshBatchCount;
            primitive.MeshBatchCount = count;
        }
        if (LayoutChanged) {
            uint64_t count = 0;
            for (const auto& primitive : Canonical.Primitives) count += primitive.MeshBatchCount;
            if (count > UINT32_MAX) return false;
            Canonical.MeshBatches.resize(static_cast<size_t>(count));
            uint32_t first = 0;
            for (uint32_t index = 0; index < PackedIds.size(); ++index) {
                auto& entry = Primitives[PackedIds[index].Slot];
                auto& primitive = Canonical.Primitives[index];
                primitive.FirstMeshBatch = first;
                WriteBatches(entry, first);
                first += primitive.MeshBatchCount;
            }
            Mark(SnapshotTable::Primitives, 0, static_cast<uint32_t>(Canonical.Primitives.size()));
            Mark(SnapshotTable::Batches, 0, static_cast<uint32_t>(Canonical.MeshBatches.size()));
        } else {
            for (const auto slot : WorkSlots) {
                const auto& entry = Primitives[slot];
                if (!entry.Id.IsValid() || !entry.BatchDirty) continue;
                const auto& primitive = Canonical.Primitives[entry.Packed];
                WriteBatches(entry, primitive.FirstMeshBatch);
                Mark(SnapshotTable::Batches, primitive.FirstMeshBatch, primitive.MeshBatchCount);
            }
        }
        DrawIndices.clear();
        for (const auto slot : WorkSlots) {
            auto& entry = Primitives[slot];
            if (entry.Id.IsValid() && entry.DrawDirty) DrawIndices.push_back(entry.Packed);
        }
        if (!scene.GetDrawStore().SyncChanged(Canonical, DrawIndices, LayoutChanged, validation)) return false;
        if (Canonical.Stats.DrawRecordFullSyncs) {
            Mark(SnapshotTable::DrawBegin, 0, static_cast<uint32_t>(Canonical.PrimitiveDrawBegin.size()));
        }
        for (const auto range : scene.GetDrawStore().ChangedDrawRanges()) Mark(SnapshotTable::Draws, range.First, range.Count);
        for (const auto range : scene.GetDrawStore().ChangedBindingRanges()) Mark(SnapshotTable::Bindings, range.First, range.Count);
        for (const auto range : scene.GetDrawStore().ChangedGeometryRanges()) Mark(SnapshotTable::GeometryPlans, range.First, range.Count);
        for (const auto slot : WorkSlots) {
            auto& entry = Primitives[slot];
            entry.WorkPending = entry.BatchDirty = entry.DrawDirty = false;
        }
        WorkSlots.clear();
        LayoutChanged = false;
        return true;
    }
    void UpdateLights(const Scene& scene) {
        uint32_t index = 0;
        for (const auto& proxy : scene.Lights()) {
            if (!proxy || !proxy->AffectsWorld()) continue;
            RenderLightData data;
            data.Type = proxy->GetLightType();
            proxy->GetLightRenderParameters(data.Parameters);
            data.WorldBounds = {data.Parameters.WorldPosition, proxy->GetRadius()};
            data.LayerMask = proxy->GetLayerMask();
            data.CastShadow = proxy->CastShadow();
            if (index == Canonical.Lights.size()) {
                Canonical.Lights.push_back(data);
                Mark(SnapshotTable::Lights, index, 1);
            } else if (!EqualLight(Canonical.Lights[index], data)) {
                Canonical.Lights[index] = data;
                Mark(SnapshotTable::Lights, index, 1);
            }
            ++index;
        }
        Canonical.Lights.resize(index);
    }
    bool Commit(const Scene& scene, uint64_t serial, RenderValidationMode validation) {
        if (CommittedSerial == serial) return true;
        RADRAY_PROFILE_SCOPE_N("Scene.CommitChanges");
        Canonical.Stats = {};
        std::erase_if(Publications, [](const auto& weak) { return weak.expired(); });
        const auto changes = scene.BeginRenderCommit(serial);
        if (!CommittedSerial) {
            for (const auto& proxy : scene.Primitives()) {
                const auto dirty = PrimitiveDirtyKind::Structure | PrimitiveDirtyKind::MaterialAssignment |
                                   PrimitiveDirtyKind::TransformOrBounds | PrimitiveDirtyKind::Filter | PrimitiveDirtyKind::MotionReset;
                if (proxy && !UpdatePrimitive(scene, {scene.GetPrimitiveId(proxy.get()), dirty})) {
                    scene.CompleteRenderCommit(serial, false);
                    return false;
                }
            }
        } else {
            for (const auto& change : changes) {
                if (change.Dirty.HasFlag(PrimitiveDirtyKind::Removed))
                    RemovePrimitive(change.Id);
                else if (!UpdatePrimitive(scene, change)) {
                    scene.CompleteRenderCommit(serial, false);
                    return false;
                }
            }
        }
        RemoveUnusedMaterials();
        UpdateMaterials(serial);
        if (!UpdateBatches(scene, validation)) {
            scene.CompleteRenderCommit(serial, false);
            return false;
        }
        UpdateLights(scene);
        Canonical.SceneEpoch = serial;
        Canonical.Valid = true;
        auto& stats = Canonical.Stats;
        stats.InputPrimitives = stats.Primitives = Canonical.Primitives.size();
        stats.InputSections = Sections;
        stats.InputMaterials = Materials.size();
        stats.InputLights = scene.Lights().size();
        stats.MeshBatches = Canonical.MeshBatches.size();
        stats.Materials = Canonical.Materials.size();
        stats.Lights = Canonical.Lights.size();
        stats.MissingGeometry = MissingGeometry;
        stats.EmptyDraw = EmptyDraws;
        stats.InvalidDrawRange = InvalidRanges;
        stats.MaterialUnavailable = UnavailableMaterials;
        stats.InvalidBounds = InvalidBounds;
        stats.PrimitiveStructuresReused = stats.Primitives - stats.PrimitiveStructuresRebuilt;
        stats.PrimitiveBoundsReused = stats.Primitives - stats.PrimitiveBoundsRebuilt;
        stats.MaterialsReused = stats.InputMaterials - stats.MaterialsRebuilt;
        stats.SceneCommits = 1;
        stats.PendingResourcesObserved += scene.GetCommitStats().PendingResourcesObserved;
        CommittedSerial = serial;
        return scene.CompleteRenderCommit(serial, true);
    }
    template <bool InjectFailure, class T>
    bool CopyTableEntries(SnapshotTable table, const vector<T>& source, vector<T>& target, SceneSnapshotPublication& publication, RenderSceneSnapshot& out) {
        if constexpr (InjectFailure) {
            if (PublicationFailure->AfterCopiedEntries == 0) {
                PublicationFailure.reset();
                return false;
            }
        }
        target.resize(source.size());
        const auto kind = static_cast<size_t>(table);
        const uint32_t pageEntries = kPageEntries[kind];
        auto& pending = publication.Tables[kind];
        for (const auto page : pending.Pages) {
            const size_t first = size_t{page} * pageEntries;
            if (first >= source.size()) continue;
            const size_t count = std::min(size_t{pageEntries}, source.size() - first);
            if constexpr (InjectFailure) {
                for (size_t index = first; index < first + count; ++index) {
                    target[index] = source[index];
                    if (--PublicationFailure->AfterCopiedEntries == 0) {
                        PublicationFailure.reset();
                        return false;
                    }
                }
            } else {
                std::copy_n(source.begin() + first, count, target.begin() + first);
            }
            ++out.Stats.PublishedPages;
            out.Stats.PublishedBytes += count * sizeof(T);
            if constexpr (std::is_same_v<T, MaterialRenderData>)
                for (size_t index = first; index < first + count; ++index) {
                    const auto bytes = MaterialPayloadBytes(source[index]);
                    out.Stats.PublishedMaterialBytes += bytes;
                    out.Stats.PublishedVariablePayloadBytes += bytes - sizeof(MaterialRenderData);
                }
            if constexpr (std::is_same_v<T, RenderPrimitiveData>)
                out.ChangedPrimitiveRanges.push_back({static_cast<uint32_t>(first), static_cast<uint32_t>(count)});
        }
        return true;
    }
    template <class T>
    bool CopyTable(SnapshotTable table, const vector<T>& source, vector<T>& target, SceneSnapshotPublication& publication, RenderSceneSnapshot& out) {
        if (PublicationFailure && PublicationFailure->AfterCopiedEntries != UINT32_MAX)
            return CopyTableEntries<true>(table, source, target, publication, out);
        return CopyTableEntries<false>(table, source, target, publication, out);
    }
};

SceneRenderState::SceneRenderState() : _impl(make_unique<Impl>()) {}
SceneRenderState::~SceneRenderState() noexcept = default;

SceneRenderStateMemoryStats SceneRenderState::GetMemoryStats() const noexcept {
    const auto& impl = *_impl;
    SceneRenderStateMemoryStats result;
    auto& catalog = result.Catalog;
    catalog.ObjectBytes = sizeof(*this) + sizeof(impl) - sizeof(impl.Canonical);
    catalog.LiveEntries = impl.Primitives.size() + impl.Materials.size() + impl.Programs.size();
    catalog.DirtyEntries = impl.PendingMaterials.size() + impl.UnusedMaterials.size() + impl.WorkSlots.size() + impl.DrawIndices.size();
    detail::MeasureVector(catalog, impl.Primitives);
    for (const auto& primitive : impl.Primitives) detail::MeasureVector(catalog, primitive.Sections, true);
    detail::MeasureVector(catalog, impl.PackedIds);
    detail::MeasureMap(catalog, impl.Materials);
    for (const auto& [generation, material] : impl.Materials) {
        detail::MeasureMap(catalog, material.Users);
        catalog.DependencyEdges += material.Users.size();
        MeasureMaterialPayload(catalog, material.Unpublished);
    }
    detail::MeasureVector(catalog, impl.MaterialIds);
    detail::MeasureVector(catalog, impl.PendingMaterials);
    detail::MeasureVector(catalog, impl.ObservedMaterials);
    detail::MeasureVector(catalog, impl.UnusedMaterials);
    detail::MeasureMap(catalog, impl.Programs);
    for (const auto& [generation, program] : impl.Programs) catalog.DependencyEdges += program.Users;
    detail::MeasureVector(catalog, impl.FreeProgramIds);
    detail::MeasureVector(catalog, impl.WorkSlots);
    detail::MeasureVector(catalog, impl.DrawIndices);
    result.Canonical = MeasureRenderSceneSnapshot(impl.Canonical);
    detail::MeasureVector(result.Publications, impl.Publications);
    for (const auto& weak : impl.Publications) {
        const auto publication = weak.lock();
        if (!publication) {
            ++result.ExpiredPublications;
            continue;
        }
        ++result.LivePublications;
        result.Publications.ObjectBytes += sizeof(*publication);
        for (const auto& table : publication->Tables) {
            detail::MeasureVector(result.Publications, table.Marked);
            detail::MeasureVector(result.Publications, table.Pages);
            result.PendingPages += table.Pages.size();
        }
    }
    result.Publications.LiveEntries = result.LivePublications;
    result.Publications.DirtyEntries = result.PendingPages;
    detail::MeasureVector(result.SharedFlights, impl.SharedFlights);
    for (const auto& flight : impl.SharedFlights) {
        if (flight.Snapshot) {
            result.SharedFlights.Add(MeasureRenderSceneSnapshot(*flight.Snapshot));
            ++result.SharedFlights.OwnerReferences;
        }
    }
    result.MaterialEntries = impl.Materials.size();
    result.ProgramEntries = impl.Programs.size();
    result.PrimitiveSlots = impl.Primitives.size();
    result.SharedFlightCount = impl.SharedFlights.size();
    result.PendingMaterials = impl.PendingMaterials.size();
    result.WorkSlots = impl.WorkSlots.size();
    result.ObservedMaterials = impl.ObservedMaterials.size();
    return result;
}

void SceneRenderState::FailNextPublicationForTesting(SnapshotPublicationFailure failure) noexcept {
    _impl->PublicationFailure = failure;
}

bool SceneRenderState::Publish(const Scene& scene, RenderSceneSnapshot& out, vector<StreamingAssetRefAny>& retainedAssets,
                               RenderValidationMode validation, std::optional<uint64_t> serial) {
    const auto epoch = serial ? *serial : gNextLegacyEpoch.fetch_add(1, std::memory_order_relaxed);
    const bool committed = _impl->CommittedSerial != epoch;
    if (!committed) {
        if (!out._publication || out._publication->Owner != _impl->Generation || out.PublicationId != _impl->EpochPublicationId) {
            out.Valid = false;
            return false;
        }
        if (out.Valid && out.SceneEpoch == epoch)
            return _impl->EpochOwnerSink.Get() == &retainedAssets && retainedAssets.size() >= _impl->EpochOwnerEnd;
        // A failed publication may retry only while its frozen values still match live owner sources.
        if (scene.GetPendingRenderChangeCount() != 0 || !_impl->PendingMaterials.empty() ||
            std::any_of(scene.Primitives().begin(), scene.Primitives().end(), [](const auto& proxy) { return proxy && !proxy->UsesRenderChangeNotifications(); })) return false;
    }
    out.Valid = false;
    if (!out._publication || out._publication->Owner != _impl->Generation) {
        auto publication = make_shared<SceneSnapshotPublication>();
        publication->Owner = _impl->Generation;
        std::erase_if(_impl->Publications, [](const auto& weak) { return weak.expired(); });
        _impl->Publications.push_back(publication);
        out._publication = std::move(publication);
        out.PublicationId = gNextPublication.fetch_add(1, std::memory_order_relaxed);
        out.PublicationRevision = out.ChangedFromPublicationRevision = 0;
    }
    if (!_impl->Commit(scene, epoch, validation)) return false;
    _impl->EpochPublicationId = out.PublicationId;
    if (!out._publication->Initialized) {
        const uint32_t sizes[kTableCount]{static_cast<uint32_t>(_impl->Canonical.Primitives.size()), static_cast<uint32_t>(_impl->Canonical.MeshBatches.size()), static_cast<uint32_t>(_impl->Canonical.Materials.size()),
                                          static_cast<uint32_t>(_impl->Canonical.Lights.size()), static_cast<uint32_t>(_impl->Canonical.DrawRecords.size()), static_cast<uint32_t>(_impl->Canonical.PrimitiveDrawBegin.size()), static_cast<uint32_t>(_impl->Canonical.BindingRecipes.size()), static_cast<uint32_t>(_impl->Canonical.GeometryBindingPlans.size())};
        for (size_t table = 0; table < kTableCount; ++table) out._publication->Tables[table].Mark(0, sizes[table], kPageEntries[table]);
        out._publication->Initialized = true;
    }
    RADRAY_PROFILE_SCOPE_N("Scene.PublishSnapshot");
    const auto ownerStart = retainedAssets.size();
    struct OwnerRollback {
        vector<StreamingAssetRefAny>& Owners;
        size_t Start;
        bool Published{false};
        ~OwnerRollback() {
            if (!Published) Owners.resize(Start);
        }
    } rollback{retainedAssets, ownerStart};
    const auto failRetain = [&] {
        if (!_impl->PublicationFailure || _impl->PublicationFailure->AfterRetainedOwners == UINT32_MAX ||
            retainedAssets.size() - ownerStart < _impl->PublicationFailure->AfterRetainedOwners) return false;
        _impl->PublicationFailure.reset();
        return true;
    };
    const auto failCopy = [&](uint32_t copied) {
        if (!_impl->PublicationFailure || copied != _impl->PublicationFailure->AfterCopiedTables) return false;
        _impl->PublicationFailure.reset();
        return true;
    };
    {
        RADRAY_PROFILE_SCOPE_N("Scene.RetainOwners");
        if (failRetain()) return false;
        for (const auto& proxy : scene.Primitives()) {
            if (proxy) proxy->CollectAssetReferences(retainedAssets);
            if (failRetain()) return false;
        }
        for (const auto& [id, material] : _impl->Materials) {
            if (material.Source && material.Index) material.Source->RetainAssetReferences(retainedAssets);
            if (failRetain()) return false;
        }
    }
    const auto previous = out.Stats;
    out.Stats = _impl->Canonical.Stats;
    out.Stats.SceneCommits = committed;
    out.Stats.SnapshotPublications = 1;
    if (!committed) {
        out.Stats.PrimitiveStructuresRebuilt = out.Stats.PrimitiveStructuresReused = 0;
        out.Stats.PrimitiveBoundsRebuilt = out.Stats.PrimitiveBoundsReused = 0;
        out.Stats.MaterialsRebuilt = out.Stats.MaterialsReused = out.Stats.MaterialBytesCopied = 0;
        out.Stats.DrawRecordBuilds = out.Stats.DrawRecordsReused = out.Stats.DrawRecordStateSelects = 0;
        out.Stats.DrawRecordFullSyncs = out.Stats.DrawRecordPrimitivesVisited = out.Stats.DrawRecordCopies = 0;
        out.Stats.StaticRecipeCompiles = out.Stats.BindingRecipeCompiles = 0;
        out.Stats.MaterialPayloadMoves = out.Stats.MovedMaterialPayloadBytes = 0;
        out.Stats.AppliedTransforms = out.Stats.EqualValueIgnored = out.Stats.ScratchEntriesCreated = 0;
        out.Stats.LegacyMaterialsObserved = out.Stats.LegacyBytesCompared = out.Stats.PendingResourcesObserved = 0;
    }
    out.Stats.RetainedAssets = retainedAssets.size() - ownerStart;
    out.ChangedPrimitiveRanges.clear();
    {
        RADRAY_PROFILE_SCOPE_N("Scene.CopySnapshotPages");
        auto& publication = *out._publication;
        if (failCopy(0)) return false;
        if (!_impl->CopyTable(SnapshotTable::Primitives, _impl->Canonical.Primitives, out.Primitives, publication, out)) return false;
        if (failCopy(1)) return false;
        if (!_impl->CopyTable(SnapshotTable::Batches, _impl->Canonical.MeshBatches, out.MeshBatches, publication, out)) return false;
        if (failCopy(2)) return false;
        if (!_impl->CopyTable(SnapshotTable::Materials, _impl->Canonical.Materials, out.Materials, publication, out)) return false;
        if (failCopy(3)) return false;
        if (!_impl->CopyTable(SnapshotTable::Lights, _impl->Canonical.Lights, out.Lights, publication, out)) return false;
        if (failCopy(4)) return false;
        if (!_impl->CopyTable(SnapshotTable::Draws, _impl->Canonical.DrawRecords, out.DrawRecords, publication, out)) return false;
        if (failCopy(5)) return false;
        if (!_impl->CopyTable(SnapshotTable::DrawBegin, _impl->Canonical.PrimitiveDrawBegin, out.PrimitiveDrawBegin, publication, out)) return false;
        if (failCopy(6)) return false;
        if (!_impl->CopyTable(SnapshotTable::Bindings, _impl->Canonical.BindingRecipes, out.BindingRecipes, publication, out)) return false;
        if (failCopy(7)) return false;
        if (!_impl->CopyTable(SnapshotTable::GeometryPlans, _impl->Canonical.GeometryBindingPlans, out.GeometryBindingPlans, publication, out)) return false;
        if (failCopy(8)) return false;
        for (auto& table : publication.Tables) table.Clear();
    }
    out.SceneEpoch = epoch;
    out.HasPassPolicies = _impl->Canonical.HasPassPolicies;
    out.Valid = true;
    out.ChangedFromPublicationRevision = out.PublicationRevision;
    if (out.PublicationRevision == UINT64_MAX) RADRAY_ABORT("Snapshot publication revision exhausted");
    ++out.PublicationRevision;
    _impl->EpochOwnerSink = &retainedAssets;
    _impl->EpochOwnerEnd = retainedAssets.size();
    rollback.Published = true;
    out.Stats.PrimitiveHighWatermark = std::max(previous.PrimitiveHighWatermark, out.Primitives.capacity());
    out.Stats.BatchHighWatermark = std::max(previous.BatchHighWatermark, out.MeshBatches.capacity());
    out.Stats.MaterialHighWatermark = std::max(previous.MaterialHighWatermark, out.Materials.capacity());
    out.Stats.LightHighWatermark = std::max(previous.LightHighWatermark, out.Lights.capacity());
    return true;
}

Nullable<shared_ptr<const RenderSceneSnapshot>> SceneRenderState::PrepareShared(const Scene& scene, uint64_t serial, uint32_t flight,
                                                                                vector<StreamingAssetRefAny>& retainedAssets, RenderValidationMode validation) {
    if (_impl->SharedFlights.size() <= flight) _impl->SharedFlights.resize(static_cast<size_t>(flight) + 1);
    auto& target = _impl->SharedFlights[flight];
    if (target.Serial != serial || !target.Snapshot->Valid) {
        if (!Publish(scene, *target.Snapshot, retainedAssets, validation, serial)) return nullptr;
        target.Serial = serial;
        target.OwnerSink = &retainedAssets;
        target.OwnerEnd = retainedAssets.size();
    }
    if (target.OwnerSink.Get() != &retainedAssets || retainedAssets.size() < target.OwnerEnd) return nullptr;
    return shared_ptr<const RenderSceneSnapshot>{target.Snapshot};
}

bool BuildRenderSceneSnapshot(const Scene& scene, RenderSceneSnapshot& out, vector<StreamingAssetRefAny>& retainedAssets, RenderValidationMode validation) {
    return scene.GetRenderState().Publish(scene, out, retainedAssets, validation);
}
bool RenderSceneSnapshotBuilder::Build(const Scene& scene, RenderSceneSnapshot& out, vector<StreamingAssetRefAny>& retainedAssets,
                                       RenderValidationMode validation, std::optional<uint64_t> serial) {
    return scene.GetRenderState().Publish(scene, out, retainedAssets, validation, serial);
}

}  // namespace radray
