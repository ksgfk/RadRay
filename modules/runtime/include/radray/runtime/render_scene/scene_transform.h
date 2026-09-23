#pragma once

#include <span>
#include <radray/basic_math.h>
#include <radray/runtime/render_scene/scene_id.h>

namespace radray {

/// Packed local TRS. Rotation is x/y/z/w; no SIMD padding is transmitted.
struct LocalTransform {
    float Rotation[4]{0, 0, 0, 1};
    float Translation[3]{0, 0, 0};
    float Scale[3]{1, 1, 1};
    LocalTransform() noexcept = default;
    LocalTransform(const Eigen::Vector3f& translation, const Eigen::Quaternionf& rotation, const Eigen::Vector3f& scale) noexcept;
    Eigen::Matrix4f ToMatrix() const noexcept;
};
static_assert(sizeof(LocalTransform) == 40);

struct TransformCreate {
    TransformId Id;
    TransformId Parent;
    LocalTransform Local;
};
struct LocalTransformUpdate {
    TransformId Id;
    LocalTransform Local;
};
static_assert(sizeof(LocalTransformUpdate) == 48);
struct TransformParentUpdate {
    TransformId Id;
    TransformId Parent;
};

/// RT-owned integer forest. Physical rows remain stable until removed; borrows expire at Apply.
class SceneTransform {
public:
    static constexpr uint32_t kNoRow = std::numeric_limits<uint32_t>::max();
    void BeginApply();
    void Create(TransformId id, const LocalTransform& local);
    void Remove(TransformId id);
    void SetParent(TransformId id, TransformId parent);
    void SetLocal(TransformId id, const LocalTransform& local);
    uint32_t CreateStandalone(const AffineTransform& matrix);
    void RemoveStandalone(uint32_t row);
    void SetStandalone(uint32_t row, const AffineTransform& matrix);
    uint32_t GetRow(TransformId id) const noexcept;
    std::span<const uint32_t> Evaluate();
    std::span<const Eigen::Matrix4f> GetWorldMatrices() const noexcept { return _world; }
    const Eigen::Matrix4f& GetWorld(uint32_t row) const noexcept { return _world[row]; }
    bool WasUpdated(uint32_t row) const noexcept { return _affected[row] == _epoch; }
    uint32_t GetFirstShape(uint32_t row) const noexcept { return _firstShape[row]; }
    void SetFirstShape(uint32_t row, uint32_t shape) noexcept { _firstShape[row] = shape; }

private:
    struct Slot {
        uint32_t Generation{0};
        uint32_t Row{kNoRow};
    };
    struct Children {
        uint32_t First{kNoRow}, Next{kNoRow}, Previous{kNoRow};
    };
    uint32_t Allocate();
    void Release(uint32_t row);
    void Detach(uint32_t row);
    void Seed(uint32_t row);
    void EvaluateSubtree(uint32_t row);
    vector<Slot> _slots;
    vector<uint32_t> _parent;
    vector<Children> _children;
    vector<Eigen::Matrix4f> _local, _world;
    vector<uint32_t> _firstShape, _seedEpoch, _affected;
    vector<uint8_t> _alive;
    vector<uint32_t> _free, _seeds, _work, _changed;
    uint32_t _epoch{0};
};

}  // namespace radray
