#pragma once

#include <filesystem>
#include <imgui.h>
#include <sigslot/signal.hpp>
#include <radray/nullable.h>
#include <radray/runtime/application_extension.h>
#include <radray/runtime/flight_completion.h>
#include <radray/runtime/texture_asset.h>
#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/render_output.h>

namespace radray {

class Application;
class RenderSystem;
class RenderWorkloadBuilder;
struct AppUpdateContext;
class ImGuiGraph;
class ImGuiGraphComponent;

enum class ImGuiColorEncoding : uint8_t { Linear,
                                          Srgb };
struct ImGuiTextureDescriptor {
    /// Encoding of the value returned by sampling the view (sRGB views already decode).
    ImGuiColorEncoding Encoding{ImGuiColorEncoding::Linear};
    RgTextureViewDesc View{};
    std::optional<render::SamplerDescriptor> Sampler{};
};

/// Exclusive state tracking for an owned, shader-readable texture. Retained by published flights.
class ImGuiTextureLease {
public:
    ImGuiTextureLease(unique_ptr<render::Texture> texture, render::TextureStates initialState);
    ~ImGuiTextureLease();

private:
    friend class ImGuiGraph;
    friend class ImGuiSystem;
    unique_ptr<render::Texture> _texture;
    vector<render::TextureStates> _states;
    vector<uint8_t> _valid;
};

struct ImGuiFontDescriptor {
    std::filesystem::path Path;
    ImFontConfig Config{};
    vector<ImWchar> ExcludeRanges{};
};
struct ImGuiSystemDescriptor {
    bool Docking{true};
    bool Viewports{true};
    bool KeyboardNavigation{true};
    /// Register the UI component as a RenderSystem overlay so the default composer draws it
    /// after the scene. Disable when installing a custom FrameGraphComposer that places the UI itself.
    bool InstallDefaultOverlay{true};
    float FontSize{16};
    float StyleScale{1};
    std::filesystem::path SettingsPath{};
    vector<ImGuiFontDescriptor> Fonts{};
};

class ImGuiGraphFrame;

/// One ImGui context per Application, installed as an ApplicationExtension. All ImGui API and
/// platform callbacks stay on the application thread. Contract: docs/architecture/runtime-imgui.md
class ImGuiSystem final : public ApplicationExtension, public IFlightCompletionObserver {
public:
    /// Game thread, inside Application::OnInit. Creates the context, installs the extension, the
    /// flight observer and (optionally) the default overlay. Returns null without any residual
    /// registration when initialization fails. The Application owns the returned system.
    static Nullable<ImGuiSystem*> Install(Application& app, const ImGuiSystemDescriptor& descriptor);
    ~ImGuiSystem() override;
    ImGuiSystem(const ImGuiSystem&) = delete;
    ImGuiSystem& operator=(const ImGuiSystem&) = delete;

    /// Emitted inside the active ImGui frame after World::Tick; subscribers issue ImGui calls here.
    sigslot::signal<>& EventDraw() noexcept;
    /// The UI graph component; connect it in a custom FrameGraphComposer when the default overlay is disabled.
    ImGuiGraphComponent& GetGraphComponent() noexcept;

    /// GT only. Rebuild spacing from the unscaled baseline; font DPI remains upstream-owned.
    void SetStyleScale(float scale);
    ImTextureID RegisterTexture(StreamingAssetRef<TextureAsset> asset, const ImGuiTextureDescriptor& descriptor = {});
    ImTextureID RegisterTexture(shared_ptr<ImGuiTextureLease> lease, const ImGuiTextureDescriptor& descriptor = {});
    /// Display an SDR scene output produced in this frame, before UI composition. Encoding is inferred from its format.
    /// The output owner must keep it registered through all published flights; this does not request a camera view.
    ImTextureID RegisterOutput(RenderOutputId output);
    ImTextureID CreateGraphImage(const ImGuiTextureDescriptor& descriptor = {});
    bool UnregisterTexture(ImTextureID texture);
    bool HasError() const noexcept;
    ImGuiGraphFrame GetGraphFrame(uint32_t flight) noexcept;
    /// Request the outputs of every viewport captured for this flight.
    void RequestOutputs(uint32_t flight, RenderWorkloadBuilder& builder) const;

    // ApplicationExtension slots; framework use only.
    void OnBeginUpdate(uint32_t flight) override;
    void OnBeforeInput(const AppUpdateContext& ctx) override;
    void OnAfterWorldTick(const AppUpdateContext& ctx) override;
    void OnFlightsComplete(std::span<const FlightCompletion> completions) noexcept override;

private:
    friend class ImGuiGraph;
    explicit ImGuiSystem(Application& app);
    bool Initialize(const ImGuiSystemDescriptor& descriptor);
    void BeginUpdate(uint32_t flight);
    bool NewFrame(const AppUpdateContext& context);
    void CaptureFrame(uint32_t flight);
    struct Impl;
    unique_ptr<Impl> _impl;
};

}  // namespace radray
