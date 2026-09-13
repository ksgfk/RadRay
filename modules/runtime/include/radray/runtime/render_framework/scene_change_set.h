#pragma once

#include <radray/types.h>

namespace radray {

enum class SceneDataTable : uint8_t {
    Primitives,
    Batches,
    Materials,
    Lights,
    Draws,
    DrawBegin,
    Bindings,
    GeometryPlans,
    DrawPlans,
    StatePlans,
    Count
};

struct SnapshotChangedRange {
    uint32_t First{0}, Count{0};
};

/// Apply by resizing to Count and replacing Ranges with rows from the matching snapshot.
/// Replaced rows carry current identities, including moved or reused primitive/draw slots.
struct SceneTableChanges {
    uint32_t PreviousCount{0}, Count{0};
    vector<SnapshotChangedRange> Ranges;
};

/// Delta from this publication target's last successful version, including skipped epochs.
struct SceneChangeSet {
    uint64_t PublicationId{0}, Epoch{0}, FromRevision{0}, Revision{0};
    array<SceneTableChanges, static_cast<size_t>(SceneDataTable::Count)> Tables;
    bool IsValid() const noexcept { return PublicationId != 0 && Revision != 0; }
    const SceneTableChanges& Get(SceneDataTable table) const noexcept { return Tables[static_cast<size_t>(table)]; }
    void ResetForReuse() noexcept {
        PublicationId = Epoch = FromRevision = Revision = 0;
        for (auto& table : Tables) {
            table.PreviousCount = table.Count = 0;
            table.Ranges.clear();
        }
    }
};

}  // namespace radray
