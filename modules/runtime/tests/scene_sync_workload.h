#pragma once

#include <cstdint>
#include <radray/types.h>

namespace radray::test {

enum class Workload { Transform,
                      SameValue,
                      HierarchyLeaf,
                      HierarchyRoot,
                      Rebind,
                      Lights,
                      Churn,
                      Burst,
                      Reparent,
                      Mixed,
                      Level };

struct Scenario {
    string Name;
    Workload Kind{Workload::Transform};
    uint32_t Shapes{10000}, Changes{100}, Lights{0}, LightChanges{0}, Repeats{1}, Depth{1};
    bool Wide{false}, Sequential{false};
    /// Workload::Level only. Changes moves, RebindCount rebinds and StreamCount replacements draw
    /// disjoint selection offsets, so their sum must stay within the selection.
    uint32_t StreamCount{0}, StreamPeriod{1}, RebindCount{0};
    bool UniqueAssets{false};
    uint32_t Views{0};
};

vector<Scenario> SceneSyncScenarios(bool small);

/// Shared workload and bounded GT/RT transport; contains no timers or sample collection.
class SceneSyncWorkload {
public:
    SceneSyncWorkload(Scenario scenario, uint32_t flights, bool threaded, bool verify = false, bool gate = false);
    ~SceneSyncWorkload();
    /// Finishes all submitted frames before returning. Suitable for KeepRunningBatch.
    void RunFrames(size_t count);
    bool FinishAndValidate();
    uint32_t PublishedWhileGated() const;
    /// Accumulated view result; read after RunFrames has drained all flights.
    uint64_t Visible() const;

private:
    class Impl;
    unique_ptr<Impl> _impl;
};

}  // namespace radray::test
