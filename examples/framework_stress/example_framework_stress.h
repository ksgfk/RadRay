#pragma once

#include "scene_draw.h"
#include <radray/runtime/application.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/static_mesh_component.h>

namespace radray::example {

enum class StressMode { World,
                        Sync,
                        Upload,
                        Draw };
enum class StressWorkload { Idle,
                            Move,
                            Parent,
                            Tick,
                            Churn };

struct FrameworkStressOptions {
    StressMode Mode{StressMode::Upload};
    StressWorkload Workload{StressWorkload::Move};
    uint32_t Objects{10000};
    uint32_t Changes{100};
    uint32_t Views{1};
    uint32_t Flights{2};
    uint32_t Warmup{120};
    uint32_t Frames{0};
    uint32_t Seconds{60};
    bool Window{false};
    bool Multithreaded{true};
    bool Validation{false};
    bool GpuProfiler{true};
    render::RenderBackend Backend{render::RenderBackend::D3D12};
};

class FrameworkStressApp final : public Application {
public:
    explicit FrameworkStressApp(FrameworkStressOptions options);
    bool Failed() const noexcept { return _failed.load(std::memory_order_relaxed); }

protected:
    void OnInit() override;
    void OnUpdate(const AppUpdateContext& context) override;
    void OnCollectRenderViews(SceneViewCollector& collector) override;
    void OnRender(AppFrameContext& frame) override;
    void OnShutdown() override;

private:
    struct FrameResources {
        uint64_t UpdateIndex{0};
        unique_ptr<render::Texture> Image;
        unique_ptr<render::TextureView> View;
        unique_ptr<render::Framebuffer> Framebuffer;
        render::TextureStates State{render::TextureState::Undefined};
    };

    StaticMeshComponent* SpawnObject(uint32_t index);
    bool InitializeDrawing();
    void MoveObjects();
    void ReplaceObjects();
    void Fail(std::string_view reason);

    const FrameworkStressOptions _options;
    Nullable<World*> _world{nullptr};
    Nullable<CameraComponent*> _camera{nullptr};
    Nullable<SceneComponent*> _parent{nullptr};
    StreamingAssetRef<StaticMesh> _mesh;
    vector<StaticMeshComponent*> _objects;
    Nullable<render::RenderPass*> _pass{nullptr};
    unique_ptr<SceneDraw> _draw;
    vector<FrameResources> _frames;
    std::chrono::steady_clock::time_point _captureStart{};
    uint64_t _updates{0};
    uint64_t _renderCallbacks{0};
    uint32_t _side{1};
    uint32_t _cursor{0};
    std::atomic_bool _failed{false};
};

}  // namespace radray::example
