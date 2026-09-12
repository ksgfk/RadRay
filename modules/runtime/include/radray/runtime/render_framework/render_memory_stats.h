#pragma once

#include <radray/types.h>

namespace radray {

/// Explicit CPU-container census. Query only at a serialized observation point; collection
/// visits current entries without allocating. KnownBytes excludes allocator/node bookkeeping,
/// hash bucket storage, shared_ptr control blocks, borrowed authoring objects and GPU resources.
/// Payload fields describe nested logical data and overlap the storage categories.
struct RenderMemoryStats {
    uint64_t ObjectBytes{0}, VectorCapacityBytes{0}, StringCapacityBytes{0}, MapValueBytes{0};
    uint64_t MapNodes{0}, MapBuckets{0}, MapContainers{0};
    uint64_t VariablePayloadBytes{0}, VariablePayloadCapacityBytes{0};
    uint64_t LiveEntries{0}, DirtyEntries{0}, DependencyEdges{0}, OwnerReferences{0};
    uint64_t KnownBytes() const noexcept { return ObjectBytes + VectorCapacityBytes + StringCapacityBytes + MapValueBytes; }
    void Add(const RenderMemoryStats& other) noexcept {
        ObjectBytes += other.ObjectBytes;
        VectorCapacityBytes += other.VectorCapacityBytes;
        StringCapacityBytes += other.StringCapacityBytes;
        MapValueBytes += other.MapValueBytes;
        MapNodes += other.MapNodes;
        MapBuckets += other.MapBuckets;
        MapContainers += other.MapContainers;
        VariablePayloadBytes += other.VariablePayloadBytes;
        VariablePayloadCapacityBytes += other.VariablePayloadCapacityBytes;
        LiveEntries += other.LiveEntries;
        DirtyEntries += other.DirtyEntries;
        DependencyEdges += other.DependencyEdges;
        OwnerReferences += other.OwnerReferences;
    }
    friend bool operator==(const RenderMemoryStats&, const RenderMemoryStats&) = default;
};

struct SceneRenderStateMemoryStats {
    RenderMemoryStats Catalog, Canonical, Publications, SharedFlights;
    uint64_t MaterialEntries{0}, ProgramEntries{0}, PrimitiveSlots{0};
    uint64_t SharedFlightCount{0}, LivePublications{0}, ExpiredPublications{0};
    uint64_t PendingPages{0}, PendingMaterials{0}, WorkSlots{0}, ObservedMaterials{0};
    RenderMemoryStats Total() const noexcept {
        auto result = Catalog;
        result.Add(Canonical);
        result.Add(Publications);
        result.Add(SharedFlights);
        return result;
    }
};

}  // namespace radray
