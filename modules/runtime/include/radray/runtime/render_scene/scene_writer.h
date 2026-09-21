#pragma once

#include <radray/sparse_set.h>
#include <radray/runtime/render_scene/scene_update.h>
#include <radray/runtime/render_scene/render_asset_lifetime.h>

namespace radray {

class RenderSystem;

/// GT only. One producer owns a scene; borrows expire when destruction is requested.
class SceneWriter {
public:
    SceneWriter(SceneId id, uint32_t flightCount);
    ~SceneWriter() noexcept;
    SceneWriter(const SceneWriter&) = delete;
    SceneWriter& operator=(const SceneWriter&) = delete;

    SceneId GetSceneId() const noexcept { return _id; }
    ShapeId CreateShape();
    void RemoveShape(ShapeId id);
    void SetStaticMesh(ShapeId id, const StreamingAssetRef<StaticMesh>& mesh, const Eigen::Matrix4f& localToWorld);
    /// Requires a prior SetStaticMesh, including an explicitly empty mesh binding.
    void SetTransform(ShapeId id, const Eigen::Matrix4f& localToWorld);
    /// Reserves an identity; the light enters snapshots after its first SetLight.
    LightId CreateLight();
    void RemoveLight(LightId id);
    void SetLight(const LightData& light);

private:
    friend class RenderSystem;
    friend class PrimitiveComponent;

    static constexpr uint32_t kNoColdSlot = std::numeric_limits<uint32_t>::max();

    /// 热数据：transform-only 更新与 dirty 队列每帧随机访问，保持小巧。
    /// 成员顺序按对齐排布（Matrix4f 前是 8 字节字段，避免填充浪费）。
    struct ShapeState {
        ShapeId Id;
        size_t DirtyIndex{std::numeric_limits<size_t>::max()};
        std::optional<Eigen::Matrix4f> Transform;
        uint32_t ColdIndex{kNoColdSlot};
        bool Sent{false};
        bool HasMesh{false};
        bool PendingMesh{false};  // 冷槽持有待封包的 StaticMeshStateUpdate
    };

    /// 冷数据：只在 SetStaticMesh / RemoveShape / 封包 mesh 记录时访问。
    struct ShapeAsset {
        std::optional<AssetId> Asset;
        std::optional<StaticMeshStateUpdate> Mesh;
    };

    ShapeState& GetShape(ShapeId id);
    uint32_t AllocateColdSlot() noexcept;
    void Queue(ShapeState& state);
    void Unqueue(ShapeState& state);
    void Flush(SceneUpdateBatch& batch, uint32_t flightIndex);

    SceneId _id;
    SparseSet<ShapeState> _shapes;
    vector<ShapeAsset> _shapeAssets;
    vector<uint32_t> _freeColdSlots;
    vector<ShapeId> _dirtyShapes;
    vector<ShapeId> _removedShapes;
    SparseSet<std::monostate> _lightIds;
    LightSceneData _lights;
    bool _lightsDirty{false};
    RenderAssetLifetime _assets;
    bool _closing{false};
    bool _claimed{false};
};

}  // namespace radray
