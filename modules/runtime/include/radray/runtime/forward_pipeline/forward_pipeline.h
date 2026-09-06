#pragma once

#include <radray/render/shader_layout.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/render_scene_snapshot.h>
#include <radray/runtime/render_framework/mesh_draw_command.h>
#include <radray/types.h>

namespace radray {

namespace forward_detail {
struct ForwardViewDrawWork;
struct ForwardPipelineTestAccess;
}  // namespace forward_detail

class Application;
class CameraComponent;
class Scene;

struct ForwardStageBStats {
    uint64_t SnapshotBuilds{0}, CullCalls{0}, CullFailures{0};
    uint64_t DepthCommands{0}, OpaqueCommands{0}, TransparentCommands{0};
    DrawExecutionStats Execution;
};

enum class ForwardAntialiasing : uint8_t { None,
                                           Temporal,
                                           Msaa4 };
enum class ForwardDebugView : uint8_t { Final,
                                        LinearDepth,
                                        Normals,
                                        Motion,
                                        AmbientOcclusion,
                                        TileOccupancy,
                                        Bloom,
                                        Shadows,
                                        CurrentHdr,
                                        HistoryHdr,
                                        DepthPyramid };

struct ForwardPipelineSettings {
    bool Hdr{false}, Shadows{false}, ForwardPlus{false}, AmbientOcclusion{false}, Bloom{false}, Fireflies{false};
    ForwardAntialiasing Antialiasing{ForwardAntialiasing::None};
    ForwardDebugView DebugView{ForwardDebugView::Final};
    float RenderScale{1}, Exposure{1}, ShadowDistance{80}, AoRadius{1}, BloomStrength{.08f};
    uint32_t ShadowResolution{1024}, MaxLocalLights{256}, MaxLightsPerTile{64};
    static ForwardPipelineSettings Temporal() noexcept;
    static ForwardPipelineSettings Msaa() noexcept;
    bool IsValid() const noexcept;
    friend bool operator==(const ForwardPipelineSettings&, const ForwardPipelineSettings&) = default;
};

struct ForwardViewSource {
    RenderOutputId Output;
    RenderViewDesc View;
};

struct ForwardOutputOverlay {
    RenderOutputId Source, Destination;
    NormalizedRect Rectangle{.72f, .03f, .25f, .25f};
};

class ForwardPipeline final : public RenderPipeline {
public:
    ForwardPipeline(
        Application* app,
        Scene* scene,
        CameraComponent* camera);
    ~ForwardPipeline() noexcept override;

    void PrepareFrame(RenderPrepareContext& ctx) override;
    void Render(RenderPipelineContext& ctx) override;

    /// Game thread. Inputs are copied into the next writable flight during PrepareFrame.
    bool SetSettings(const ForwardPipelineSettings& settings) noexcept;
    const ForwardPipelineSettings& GetSettings() const noexcept;
    /// Empty restores the camera supplied to the constructor for each presentation output.
    bool SetViews(std::span<const ForwardViewSource> views);
    /// Sources are SDR offscreen outputs produced by this frame, sampled by the final composite.
    bool SetOutputOverlays(std::span<const ForwardOutputOverlay> overlays);
    /// Capture the next prepared frame. CompleteCaptures is called after that flight's fence.
    void RequestCapture(std::filesystem::path directory, string name);
    bool CompleteCaptures(uint32_t flightIndex);
    bool Failed() const noexcept;

    // The pipeline uploads its view, material, and object constant buffers out of a per-frame
    // arena, so each of those declarations has to take its offset at bind time: a root descriptor
    // on D3D12 and a dynamic uniform buffer descriptor on Vulkan.
    static render::ShaderProgramLayoutRecipe GetLayoutRecipe() noexcept;
    static render::ShaderProgramLayoutRecipe GetDepthOnlyLayoutRecipe() noexcept;

    // Read only at the flight's phase boundary, while its owner is not updating/rendering it.
    const RenderSceneSnapshot& GetSceneSnapshot(uint32_t flightIndex) const noexcept;
    const ForwardStageBStats& GetStageBStats(uint32_t flightIndex) const noexcept;

private:
    friend struct forward_detail::ForwardPipelineTestAccess;
    std::span<const forward_detail::ForwardViewDrawWork> GetViewWork(uint32_t flightIndex, uint32_t familyIndex) const noexcept;

    struct Impl;

    unique_ptr<Impl> _impl;
};

}  // namespace radray
