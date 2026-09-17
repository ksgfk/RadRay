#pragma once

namespace radray {

/// Owned values only; sealed on GT before the runner publishes the flight to RT.
struct SceneUpdateBatch {
};

/// A single persistent CPU scene. RT owns Apply and reads; GT may inspect only after RT stops.
class Scene {
public:
    /// Must run once per published batch, in order, after all preceding CPU scene readers finish.
    void Apply(const SceneUpdateBatch& batch) noexcept;
};

}  // namespace radray
