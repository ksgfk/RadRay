#pragma once

#include <atomic>
#include <thread>
#include <radray/runtime/imgui/imgui_graph.h>
#include <radray/runtime/window_manager.h>

namespace radray {

struct UiTextureRecord {
    ImTextureID Id{0};
    ImGuiTextureDescriptor Descriptor;
    StreamingAssetRef<TextureAsset> Asset;
    shared_ptr<ImGuiTextureLease> Lease;
    RenderOutputId Output;
    bool Graph{false}, Dynamic{false};
};
struct UiDrawCommand {
    ImVec4 Clip;
    ImTextureID Texture{0};
    uint32_t Count{0}, IndexOffset{0};
    int32_t VertexOffset{0};
    int Sampler{0};  // 0 = texture/default, 1 = linear, 2 = nearest
};
struct UiViewportFrame {
    RenderOutputId Output;
    ImVec2 Position{}, Size{}, Scale{1, 1};
    vector<ImDrawVert> Vertices;
    vector<ImDrawIdx> Indices;
    vector<UiDrawCommand> Commands;
};
struct UiTextureRequest {
    ImTextureID Id{0};
    uint64_t Version{0};
    ImTextureStatus Status{ImTextureStatus_OK};
    uint32_t Width{0}, Height{0};
    render::TextureFormat Format{render::TextureFormat::RGBA8_UNORM};
    vector<ImTextureRect> Regions;
    vector<byte> Pixels;  // Always owned, tightly packed RGBA32.
};
struct UiGpuTexture {
    unique_ptr<render::Texture> Texture;
    array<render::TextureStates, 1> States{render::TextureState::Undefined};
    array<uint8_t, 1> Valid{0};
};
struct UiFlight {
    vector<UiViewportFrame> Viewports;
    unordered_map<ImTextureID, shared_ptr<UiTextureRecord>> Textures;
    vector<UiTextureRequest> Requests;
    vector<shared_ptr<UiGpuTexture>> Retained;
    vector<unique_ptr<MappedUploadPage>> Uploads;
    vector<unique_ptr<RenderExternalTexture>> ExternalTextures;
    vector<unique_ptr<RenderExternalBuffer>> ExternalBuffers;
    vector<vector<render::TextureStates>> AssetStates;
    vector<vector<uint8_t>> AssetValid;
    vector<RgPassHandle> UploadPasses;
    bool GraphSuccess{false}, Valid{true};
    std::atomic_bool Completed{false};
};

struct ImGuiSystem::Impl {
    explicit Impl(Application& app) : App(app), Thread(std::this_thread::get_id()) {}
    Application& App;
    std::thread::id Thread;
    Nullable<ImGuiContext*> Context{nullptr};
    ImGuiSystemDescriptor Descriptor;
    ImGuiStyle Baseline;
    bool InFrame{false};
    std::atomic_bool Error{false};
    string Clipboard;
    struct PlatformWindow {
        Nullable<AppWindow*> Window{nullptr};
        bool Main{false};
        vector<sigslot::scoped_connection> Connections;
    };
    struct TextureSlot {
        uint32_t Generation{1};
        shared_ptr<UiTextureRecord> Record;
    };
    struct PendingTexture {
        ImTextureID Id{0};
        uint64_t Version{0}, Fingerprint{0};
    };
    vector<TextureSlot> Slots;
    unordered_map<ImTextureData*, PendingTexture> Pending;
    vector<unique_ptr<UiFlight>> Flights;
    unordered_map<ImTextureID, shared_ptr<UiGpuTexture>> GpuTextures;  // RT only
    Nullable<ShaderProgram*> DrawProgram{nullptr}, CompositeProgram{nullptr};
    unordered_map<KeyCode, bool> Keys;
    unordered_map<KeyCode, NativeWindow*> KeyWindows;
    unordered_map<int, NativeWindow*> MouseWindows;
    bool Focused{true};
    bool MonitorsDirty{true};
    void CheckThread() const;
    ImTextureID AddRecord(shared_ptr<UiTextureRecord> record);
    shared_ptr<UiTextureRecord> FindRecord(ImTextureID id) const;
    static Impl& Current();
    static Nullable<NativeWindow*> Native(ImGuiViewport* viewport);
    static void Attach(ImGuiViewport* viewport, AppWindow* window, bool main);
    static void CreateWindow(ImGuiViewport* viewport);
    static void DestroyWindow(ImGuiViewport* viewport);
    void InstallPlatform();
    void RefreshMonitors();
    void ReleaseWindowInput(NativeWindow* window, bool keyboard);
    void SaveSettings();
};

}  // namespace radray
