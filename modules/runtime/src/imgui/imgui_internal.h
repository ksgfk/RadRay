#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <atomic>
#include <thread>
#include "imgui_graph_frame.h"
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
struct ImGuiSystem::Impl {
    explicit Impl(Application& app);
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
    vector<vector<shared_ptr<UiTextureRecord>>> FrameOwners;  // Game thread only, released after flight completion.
    UiGraphResources Graph;
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

#endif  // RADRAY_ENABLE_IMGUI
