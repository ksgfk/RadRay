#pragma once

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/render_component.h>
#include <radray/runtime/static_mesh.h>

namespace radray {

/// Component-local business identity; independent of the current scene connection and dense row.
struct InstanceHandle {
    uint32_t Index{UINT32_MAX}, Generation{0};
    constexpr bool IsValid() const noexcept { return Index != UINT32_MAX; }
    constexpr auto operator<=>(const InstanceHandle&) const noexcept = default;
};
struct InstanceInit {
    Eigen::Matrix4f LocalToComponent{Eigen::Matrix4f::Identity()};
    ShapeDrawState Draw{};
};
struct InstanceUpdate {
    InstanceHandle Handle;
    Eigen::Matrix4f LocalToComponent;
    std::optional<ShapeDrawState> Draw{};
};
enum class InstanceDirtyFlag : uint8_t { TransformDirty = 1,
                                         DrawDirty = 2,
                                         NewInstance = 4 };
template <>
struct is_flags<InstanceDirtyFlag> : std::true_type {};
using InstanceDirtyFlags = EnumFlags<InstanceDirtyFlag>;
inline auto format_as(InstanceDirtyFlag value) noexcept { return EnumFlagBitName(value); }

/// One business component with many ordinary scene shapes. Contract: docs/architecture/render-framework.md
class InstancedStaticMeshComponent final : public RenderComponent {
public:
    InstancedStaticMeshComponent() noexcept = default;
    ~InstancedStaticMeshComponent() noexcept override;
    void SetStaticMesh(StreamingAssetRef<StaticMesh> mesh);
    void SetMaterials(vector<StreamingAssetRef<Material>> materials);
    const StreamingAssetRef<StaticMesh>& GetStaticMesh() const noexcept { return _mesh; }
    std::span<const StreamingAssetRef<Material>> GetMaterials() const noexcept { return _materials; }
    size_t GetInstanceCount() const noexcept { return _local.size(); }
    bool Contains(InstanceHandle handle) const noexcept;
    std::optional<Eigen::Matrix4f> GetInstanceTransform(InstanceHandle handle) const noexcept;
    ShapeId GetShapeId(InstanceHandle handle) const noexcept;
    bool AddInstances(std::span<const InstanceInit> values, std::span<InstanceHandle> result);
    bool UpdateInstances(std::span<const InstanceUpdate> values);
    /// Rejects stale or duplicate handles without changing the collection.
    bool RemoveInstances(std::span<const InstanceHandle> handles);
    bool SetInstanceParameters(std::span<const InstanceHandle> handles, const InstanceParameterLayout& layout, std::span<const byte> values);

private:
    void CreateRenderState(SceneWriter& writer) override;
    void DestroyRenderState(SceneWriter& writer) override;
    void CollectRenderUpdates(SceneCapture& capture, RenderDirtyFlags dirty) override;
    void QueueDirty(size_t row, InstanceDirtyFlags flags);
    void StartReadyWaits();
    task<void> WaitForMeshReady(StreamingAssetRef<StaticMesh> mesh, uint64_t connection);
    task<void> WaitForMaterialReady(StreamingAssetRef<Material> material, uint64_t connection);
    static bool ValidTransform(const Eigen::Matrix4f& value) noexcept;
    SparseSet<uint32_t> _instances;
    vector<InstanceHandle> _handles, _dirty;
    vector<Eigen::Matrix4f> _local;
    vector<ShapeId> _shapes, _removed;
    vector<ShapeDrawState> _draw;
    vector<InstanceDirtyFlags> _dirtyFlags;
    struct Parameters {
        InstanceParameterLayout Layout;
        vector<byte> Values;
        vector<InstanceHandle> Dirty;
        vector<uint8_t> Queued;
    };
    vector<Parameters> _parameters;
    StreamingAssetRef<StaticMesh> _mesh;
    vector<StreamingAssetRef<Material>> _materials;
    bool _materialsDirty{true}, _connected{false};
    uint64_t _connection{0}, _domain{0};
    unique_ptr<TaskScope> _readyWait;
    vector<uint32_t> _rows;
    vector<ShapeId> _newIds, _meshIds, _parameterIds;
    vector<Eigen::Matrix4f> _worldMatrices;
    vector<ShapeTransformUpdate> _transforms;
    vector<ShapeDrawUpdate> _drawUpdates;
    vector<byte> _parameterBytes;
};

template <>
struct RuntimeTypeTrait<InstancedStaticMeshComponent> {
    static constexpr RuntimeTypeId value{0x2fd2c172, 0xc840, 0x4ff0, 0x8d, 0x24, 0x57, 0x3b, 0x4a, 0x65, 0x11, 0xa8};
};
}  // namespace radray
