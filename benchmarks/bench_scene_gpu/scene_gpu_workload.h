#pragma once

#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>

namespace radray {
class World;
class SceneComponent;
}  // namespace radray

namespace radray::benchmarking {

enum class SceneGpuLoad { Transforms,
                          Parent,
                          Churn,
                          FirstUse,
                          Growth };

class SceneGpuWorkload {
public:
    SceneGpuWorkload(render::RenderBackend backend, uint32_t flights, uint32_t changes, uint32_t views, SceneGpuLoad load = SceneGpuLoad::Transforms);
    ~SceneGpuWorkload();
    bool IsValid() const noexcept { return _gpu != nullptr; }
    const string& Error() const noexcept { return _error; }
    AppFrameContext Begin();
    uint32_t BeginUpdate();
    void SealScene(uint32_t flight);
    AppFrameContext BeginRecord(uint32_t flight);
    void Consume(const AppFrameContext& frame);
    void Prepare(AppFrameContext& frame);
    void Submit(AppFrameContext& frame);
    void Drain();
    void RestartColdScene();
    uint64_t UploadedBytes{0}, UploadRanges{0};
    double GpuMilliseconds{0};

private:
    void Complete(uint32_t flight);
    void CreateObjects(uint32_t count);
    Application _app;
    unique_ptr<GpuSystem> _gpu;
    unique_ptr<RenderSystem> _renderer;
    unique_ptr<World> _world;
    Nullable<SceneComponent*> _parent{nullptr};
    SceneGpuLoad _load;
    SceneId _scene;
    vector<ShapeId> _objects;
    vector<uint64_t> _serials;
    uint32_t _changes, _views, _step{0};
    string _error;
};

}  // namespace radray::benchmarking
