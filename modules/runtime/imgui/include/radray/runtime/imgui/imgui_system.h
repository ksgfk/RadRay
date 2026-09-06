#pragma once

#include <filesystem>
#include <imgui.h>
#include <radray/runtime/texture_asset.h>
#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/render_output.h>

namespace radray {

class Application;
struct AppUpdateContext;
class ImGuiGraph;

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
    bool Enabled{false};
    bool Docking{true};
    bool Viewports{true};
    bool KeyboardNavigation{true};
    float FontSize{16};
    float StyleScale{1};
    std::filesystem::path SettingsPath{};
    vector<ImGuiFontDescriptor> Fonts{};
};

class ImGuiSystem {
public:
    explicit ImGuiSystem(Application& app);
    ~ImGuiSystem();
    ImGuiSystem(const ImGuiSystem&) = delete;
    ImGuiSystem& operator=(const ImGuiSystem&) = delete;
    bool Initialize(const ImGuiSystemDescriptor& descriptor);
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
    /// Framework entry points. Context and platform callbacks are confined to the creating thread.
    void BeginUpdate(uint32_t flight);
    bool NewFrame(const AppUpdateContext& context);
    void CaptureFrame(uint32_t flight);
    void NotifyFlightComplete(uint32_t flight, bool gpuCompleted) noexcept;
    void RequestOutputs(uint32_t flight, class RenderWorkloadBuilder& builder) const;

private:
    friend class ImGuiGraph;
    struct Impl;
    unique_ptr<Impl> _impl;
};

}  // namespace radray
