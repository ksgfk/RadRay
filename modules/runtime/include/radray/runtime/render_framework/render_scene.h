#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <condition_variable>
#include <mutex>

#include <radray/types.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/render_framework/scene_update.h>
#include <radray/runtime/render_framework/static_mesh_table.h>

namespace radray {

struct SceneApplyChanges {
    vector<ShapeId> Updated;
    vector<ShapeId> Removed;
    vector<SceneParameterChange> Parameters;
};

/// A single persistent CPU scene. RT owns Apply and reads; GT may inspect only after RT stops.
class RenderScene {
public:
    RenderScene() noexcept;
    ~RenderScene() noexcept;
    RenderScene(const RenderScene&) = delete;
    RenderScene& operator=(const RenderScene&) = delete;
    RenderScene(RenderScene&&) = delete;
    RenderScene& operator=(RenderScene&&) = delete;

    class ReadLease {
    public:
        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;
        ReadLease(ReadLease&& other) noexcept;
        ReadLease& operator=(ReadLease&& other) noexcept;
        ~ReadLease() noexcept;

    private:
        friend class RenderScene;
        explicit ReadLease(const RenderScene* scene) noexcept;
        void Release() noexcept;
        Nullable<const RenderScene*> _scene;
    };
    /// Acquire on RT before dispatch; this lease may be moved to and released by the worker.
    ReadLease AcquireRead() const;

    /// Must run once per published batch, in order, after all preceding CPU scene readers finish.
    void Apply(const SceneUpdateBatch& batch, Nullable<SceneApplyChanges*> changes = nullptr) noexcept;
    bool ContainsShape(ShapeId id) const noexcept;
    std::optional<StaticMeshSceneView> GetStaticMesh(ShapeId id) const noexcept;
    /// Registered mesh matrix only, borrowed until the next Apply under the same reader contract.
    Nullable<const Eigen::Matrix4f*> GetStaticMeshTransform(ShapeId id) const noexcept;
    bool ContainsLight(LightId id) const noexcept { return static_cast<bool>(_lights.GetLight(id)); }
    /// Common parameters; type-specific borrows are available through GetLights().
    Nullable<const LightCommonData*> GetLight(LightId id) const noexcept { return _lights.GetLight(id); }
    const LightSceneData& GetLights() const noexcept { return _lights; }
    /// Dense registered mesh identities, including meshes whose geometry is not ready.
    std::span<const ShapeId> GetStaticMeshes() const noexcept { return _staticMeshes.GetColumns().Ids; }
    /// Dense column access for culling and drawing without identity lookups. Same borrow lifetime as GetStaticMesh.
    StaticMeshSceneColumns GetStaticMeshColumns() const noexcept { return _staticMeshes.GetColumns(_transforms.GetWorldMatrices()); }
    const unordered_map<string, SceneParameterStream>& GetParameterStreams() const noexcept { return _parameters; }
    struct DrawPrimitive {
        const GpuMesh::DrawData* Draw;
        const StaticMeshSection* Section;
        MaterialBinding Binding;
    };
    /// Shared stable geometry/bindings, borrowed until the next Apply.
    std::span<const DrawPrimitive> GetDrawPrimitives(ShapeId id) const noexcept;
    std::optional<ShapeDrawRevision> GetDrawRevision(ShapeId id) const noexcept;
    std::optional<ShapeDrawState> GetDrawState(ShapeId id) const noexcept;
    uint64_t GetStructureRevision() const noexcept { return _structureRevision; }

private:
    static constexpr uint32_t kNoMesh = std::numeric_limits<uint32_t>::max();
    struct MaterialList {
        vector<MaterialBinding> Values;
        uint32_t Uses{0};
    };
    void SetMaterials(ShapeId id, std::span<const MaterialBinding> values);
    void ReleaseMaterialList(uint32_t shape);

    struct TemplateKey {
        const StaticMeshRenderData* Mesh;
        const MaterialBinding* Materials;
        size_t Count;
        friend bool operator==(const TemplateKey&, const TemplateKey&) = default;
    };
    struct TemplateHash {
        size_t operator()(const TemplateKey& key) const noexcept;
    };
    struct DrawTemplate {
        Nullable<const TemplateKey*> Key{nullptr};
        vector<DrawPrimitive> Primitives;
        uint32_t Uses{0};
    };
    void ReleaseDrawTemplate(uint32_t index);
    void UpdateDrawTemplate(ShapeId id);
    /// Identity and routing only; geometry lives in the dense arrays below. Randomly accessed by
    /// every transform update, so keep it small.
    struct ShapeSlot {
        // Live identity, or the minimum generation accepted by the next creation.
        uint32_t Generation{0};
        // Dense table row, or kNoMesh when no geometry is registered.
        uint32_t MeshIndex{kNoMesh};
        uint32_t TransformRow{kNoMesh}, NextShape{kNoMesh}, PreviousShape{kNoMesh};
        bool Alive{false};
        bool OwnTransform{false};
        ShapeDrawRevision Revision;
        ShapeDrawState Draw;
    };
    void UnbindShape(uint32_t index);
    void BindShape(uint32_t index, uint32_t row, bool own);
    void UpdateLights(const SceneUpdateBatch& batch);
    vector<ShapeSlot> _shapes;
    SceneTransform _transforms;
    StaticMeshTable _staticMeshes;
    LightSceneData _lights;
    unordered_map<string, SceneParameterStream> _parameters;
    uint64_t _structureRevision{0};
    mutable std::mutex _readers;
    mutable std::condition_variable _readersDone;
    mutable size_t _activeReaders{0};
    unordered_map<TemplateKey, DrawTemplate, TemplateHash> _drawTemplates;
    /// Allocated for drawable geometry; pointers into stable nodes are cleared before identity reuse.
    vector<Nullable<DrawTemplate*>> _shapeTemplates;
    unordered_map<size_t, vector<unique_ptr<MaterialList>>> _materialLists;
    vector<Nullable<MaterialList*>> _shapeMaterials;
};

}  // namespace radray
