#pragma once
#include <radray/runtime/frame_timeline.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/scene_manager.h>
#include <radray/runtime/render_framework/scene_draw.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/components/instanced_static_mesh_component.h>

namespace radray::benchmarking {
class MaterialSceneWorkload {
public:
    MaterialSceneWorkload(render::RenderBackend backend, uint32_t count, uint32_t materials, uint32_t views, uint32_t percent, uint32_t churn = 0, uint32_t mode = 1, bool uniqueAssets = false, uint32_t flights = 2);
    ~MaterialSceneWorkload();
    const string& Error() const noexcept { return _error; }
    uint32_t BeginUpdate();
    void Sync(uint32_t flight);
    AppFrameContext BeginRecord(uint32_t flight);
    void PrepareAndRecord(AppFrameContext& frame);
    void Submit(AppFrameContext& frame);
    void Drain();
    uint64_t GetSceneAllocatedBytes() const noexcept;
    /// Native allocator block/allocation bytes and counts; sampled outside the timed loop.
    array<uint64_t, 4> GetNativeAllocatorUsage() const noexcept;
    size_t ActorCount() const noexcept { return _world ? _world->GetActors().size() : 0; }
    uint64_t Draws{0}, UploadBytes{0}, Frames{0};
    double GpuMs{0};

private:
    void Complete(uint32_t flight);
    StaticMeshComponent* AddObject(uint32_t index);
    FrameTimeline _timeline;
    unique_ptr<GpuSystem> _gpu;
    unique_ptr<RenderSystem> _renderer;
    AssetManager _assets;
    unique_ptr<SceneManager> _scenes;
    unique_ptr<World> _world;
    StreamingAssetRef<StaticMesh> _mesh;
    vector<StreamingAssetRef<Material>> _materials;
    vector<StaticMeshComponent*> _objects;
    Nullable<InstancedStaticMeshComponent*> _instanced{nullptr};
    vector<InstanceHandle> _instances;
    vector<InstanceUpdate> _instanceUpdates;
    vector<InstanceHandle> _parameterHandles;
    vector<byte> _parameterValues;
    InstanceParameterLayout _parameterLayout;
    vector<StreamingAssetRef<StaticMesh>> _uploadOwners;
    vector<const StaticMesh*> _uploadMeshes;
    uint32_t _nextAsset{0x100000};
    bool _uniqueAssets;
    vector<SceneViewRequest> _views;
    SceneDraw _draw;
    vector<uint64_t> _sceneBytes = vector<uint64_t>(_timeline.GetFlightDataCount());
    vector<uint64_t> _serials;
    vector<unique_ptr<render::Texture>> _colors;
    vector<unique_ptr<render::TextureView>> _colorViews;
    vector<bool> _initialized;
    uint32_t _count, _changes, _churn, _step{0};
    uint32_t _mode;
    string _error;
};
}  // namespace radray::benchmarking
