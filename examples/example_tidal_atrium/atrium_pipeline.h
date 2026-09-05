#pragma once

#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/texture_asset.h>

namespace radray {
class Scene;
}

namespace radray::atrium {

struct Settings {
    float Time{0}, Fps{60}, RenderScale{1.5f};
    uint32_t Frame{0}, Station{0};
    bool Depth{true}, Wireframe{false}, Split{false}, Beacons{true}, History{true}, Help{true}, Paused{false}, CameraCut{false}, ShowUi{true}, Ready{false};
    string CaptureName;
};

class AtriumPipeline final : public RenderPipeline {
public:
    AtriumPipeline(Application* app, Scene* scene, CameraComponent* camera,
                   StreamingAssetRef<TextureAsset> font, std::filesystem::path captureDirectory);
    ~AtriumPipeline() noexcept override;
    bool IsValid() const noexcept;
    void PrepareFrame(RenderPrepareContext& ctx) override;
    void Render(RenderPipelineContext& ctx) override;
    void Complete(uint32_t flight);
    bool Failed() const noexcept;
    Settings GameSettings;

private:
    struct Impl;
    unique_ptr<Impl> _impl;
};

}  // namespace radray::atrium
