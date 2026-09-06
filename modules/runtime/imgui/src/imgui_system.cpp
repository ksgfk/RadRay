#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <radray/file.h>
#include <radray/memory.h>
#include <radray/runtime/application.h>
#include <radray/runtime/render_system.h>

namespace radray {
namespace {

ImVec2 Vec(Eigen::Vector2i value) { return {float(value.x()), float(value.y())}; }
ImGuiKey TranslateKey(KeyCode key) {
    const auto offset = [key](KeyCode first, ImGuiKey target) { return ImGuiKey(int(target) + int(key) - int(first)); };
    if (key >= KeyCode::A && key <= KeyCode::Z) return offset(KeyCode::A, ImGuiKey_A);
    if (key >= KeyCode::NUM0 && key <= KeyCode::NUM9) return offset(KeyCode::NUM0, ImGuiKey_0);
    if (key >= KeyCode::F1 && key <= KeyCode::F24) return offset(KeyCode::F1, ImGuiKey_F1);
    if (key >= KeyCode::KP_0 && key <= KeyCode::KP_9) return offset(KeyCode::KP_0, ImGuiKey_Keypad0);
    switch (key) {
#define UI_KEY(native, ui) \
    case KeyCode::native: return ImGuiKey_##ui
        UI_KEY(SPACE, Space);
        UI_KEY(APOSTROPHE, Apostrophe);
        UI_KEY(COMMA, Comma);
        UI_KEY(MINUS, Minus);
        UI_KEY(PERIOD, Period);
        UI_KEY(SLASH, Slash);
        UI_KEY(SEMICOLON, Semicolon);
        UI_KEY(EQUAL, Equal);
        UI_KEY(LEFT_BRACKET, LeftBracket);
        UI_KEY(BACKSLASH, Backslash);
        UI_KEY(RIGHT_BRACKET, RightBracket);
        UI_KEY(GRAVE_ACCENT, GraveAccent);
        UI_KEY(ESCAPE, Escape);
        UI_KEY(ENTER, Enter);
        UI_KEY(TAB, Tab);
        UI_KEY(BACKSPACE, Backspace);
        UI_KEY(INSERT, Insert);
        UI_KEY(DELETE, Delete);
        UI_KEY(RIGHT, RightArrow);
        UI_KEY(LEFT, LeftArrow);
        UI_KEY(DOWN, DownArrow);
        UI_KEY(UP, UpArrow);
        UI_KEY(PAGE_UP, PageUp);
        UI_KEY(PAGE_DOWN, PageDown);
        UI_KEY(HOME, Home);
        UI_KEY(END, End);
        UI_KEY(CAPS_LOCK, CapsLock);
        UI_KEY(SCROLL_LOCK, ScrollLock);
        UI_KEY(NUM_LOCK, NumLock);
        UI_KEY(PRINT_SCREEN, PrintScreen);
        UI_KEY(PAUSE, Pause);
        UI_KEY(KP_DECIMAL, KeypadDecimal);
        UI_KEY(KP_DIVIDE, KeypadDivide);
        UI_KEY(KP_MULTIPLY, KeypadMultiply);
        UI_KEY(KP_SUBTRACT, KeypadSubtract);
        UI_KEY(KP_ADD, KeypadAdd);
        UI_KEY(KP_ENTER, KeypadEnter);
        UI_KEY(KP_EQUAL, KeypadEqual);
        UI_KEY(LEFT_SHIFT, LeftShift);
        UI_KEY(LEFT_CONTROL, LeftCtrl);
        UI_KEY(LEFT_ALT, LeftAlt);
        UI_KEY(LEFT_SUPER, LeftSuper);
        UI_KEY(RIGHT_SHIFT, RightShift);
        UI_KEY(RIGHT_CONTROL, RightCtrl);
        UI_KEY(RIGHT_ALT, RightAlt);
        UI_KEY(RIGHT_SUPER, RightSuper);
        UI_KEY(MENU, Menu);
#undef UI_KEY
        default: return ImGuiKey_None;
    }
}
void SamplerLinear(const ImDrawList*, const ImDrawCmd*) { RADRAY_ABORT("Linear sampler callbacks must be captured by ImGuiSystem"); }
void SamplerNearest(const ImDrawList*, const ImDrawCmd*) { RADRAY_ABORT("Nearest sampler callbacks must be captured by ImGuiSystem"); }
void ResetRenderState(const ImDrawList*, const ImDrawCmd*) { RADRAY_ABORT("Reset callbacks must be captured by ImGuiSystem"); }
uint64_t Fingerprint(const UiTextureRequest& request) {
    uint64_t hash = 14695981039346656037ull;
    const auto add = [&](std::span<const byte> bytes) { for (byte value : bytes) hash = (hash ^ std::to_integer<uint8_t>(value)) * 1099511628211ull; };
    const uint32_t header[]{uint32_t(request.Status), request.Width, request.Height, uint32_t(request.Format)};
    add(std::as_bytes(std::span{header}));
    add(std::as_bytes(std::span{request.Regions}));
    add(request.Pixels);
    return hash;
}
}  // namespace

ImGuiTextureLease::ImGuiTextureLease(unique_ptr<render::Texture> texture, render::TextureStates state)
    : _texture(std::move(texture)) {
    if (_texture) {
        const auto desc = _texture->GetDesc();
        _states.assign(desc.MipLevels * desc.DepthOrArraySize, state);
        _valid.assign(_states.size(), state.HasFlag(render::TextureState::Undefined) ? 0 : 1);
    }
}
ImGuiTextureLease::~ImGuiTextureLease() = default;
ImGuiSystem::ImGuiSystem(Application& app) : _impl(make_unique<Impl>(app)) {}
void ImGuiSystem::Impl::CheckThread() const {
    if (std::this_thread::get_id() != Thread) RADRAY_ABORT("ImGui context access must remain on the application thread");
}
ImGuiSystem::Impl& ImGuiSystem::Impl::Current() {
    auto& self = *static_cast<Impl*>(ImGui::GetIO().BackendPlatformUserData);
    self.CheckThread();
    return self;
}
Nullable<NativeWindow*> ImGuiSystem::Impl::Native(ImGuiViewport* vp) {
    const auto* data = static_cast<PlatformWindow*>(vp->PlatformUserData);
    return data && data->Window ? data->Window->GetNativeWindow() : nullptr;
}
void ImGuiSystem::Impl::Attach(ImGuiViewport* vp, AppWindow* window, bool main) {
    auto* data = new PlatformWindow;
    data->Window = window;
    data->Main = main;
    vp->PlatformUserData = data;
    auto* native = window->GetNativeWindow();
    vp->PlatformHandle = native;
    vp->PlatformHandleRaw = native->GetNativeHandler();
    data->Connections.emplace_back(native->EventCloseRequested().connect([vp]() { vp->PlatformRequestClose = true; }));
    data->Connections.emplace_back(native->EventMoved().connect([vp](int, int) { vp->PlatformRequestMove = true; }));
    data->Connections.emplace_back(native->EventResized().connect([vp](int, int) { vp->PlatformRequestResize = true; }));
    data->Connections.emplace_back(native->EventDisplayChanged().connect([] { Current().MonitorsDirty = true; }));
}
void ImGuiSystem::Impl::CreateWindow(ImGuiViewport* vp) {
    auto& self = Current();
    Win32WindowCreateDescriptor desc;
    desc.Title = "RadRay ImGui";
    desc.Width = std::max(1, int(vp->Size.x));
    desc.Height = std::max(1, int(vp->Size.y));
    desc.X = int(vp->Pos.x);
    desc.Y = int(vp->Pos.y);
    desc.Resizable = true;
    desc.StartVisible = false;
    desc.Decorated = (vp->Flags & ImGuiViewportFlags_NoDecoration) == 0;
    desc.ShowInTaskbar = (vp->Flags & ImGuiViewportFlags_NoTaskBarIcon) == 0;
    desc.TopMost = (vp->Flags & ImGuiViewportFlags_TopMost) != 0;
    desc.ActivateOnShow = (vp->Flags & ImGuiViewportFlags_NoFocusOnAppearing) == 0;
    desc.FocusOnClick = (vp->Flags & ImGuiViewportFlags_NoFocusOnClick) == 0;
    desc.InputPassthrough = (vp->Flags & ImGuiViewportFlags_NoInputs) != 0;
    if (auto* parent = ImGui::FindViewportByID(vp->ParentViewportId)) desc.OwnerWindow = Native(parent);
    auto* manager = self.App.GetWindowManager();
    auto window = manager->CreateWindow(desc, false, RenderOutputUsage::Auxiliary);
    if (window) {
        render::SwapChainDescriptor swap;
        swap.Width = uint32_t(desc.Width);
        swap.Height = uint32_t(desc.Height);
        swap.Format = manager->GetMainBackBufferFormat();
        swap.PresentMode = manager->GetMainPresentMode();
        if (window->AttachSwapChain(swap)) {
            Attach(vp, window.Get(), false);
            return;
        }
        manager->DestroyWindow(window.Get());
    }
    self.Error = true;
    vp->PlatformRequestClose = true;
    RADRAY_ERR_LOG("ImGui platform viewport {} could not be created or attached", vp->ID);
}
void ImGuiSystem::Impl::DestroyWindow(ImGuiViewport* vp) {
    auto& self = Current();
    auto* data = static_cast<PlatformWindow*>(vp->PlatformUserData);
    if (data) {
        if (data->Window) self.ReleaseWindowInput(data->Window->GetNativeWindow(), true);
        data->Connections.clear();
        if (!data->Main && data->Window) self.App.GetWindowManager()->DestroyWindow(data->Window.Get());
        delete data;
    }
    vp->PlatformUserData = vp->PlatformHandle = vp->PlatformHandleRaw = nullptr;
}
void ImGuiSystem::Impl::ReleaseWindowInput(NativeWindow* window, bool keyboard) {
    auto& io = ImGui::GetIO();
    for (auto it = MouseWindows.begin(); it != MouseWindows.end();) {
        if (it->second != window) {
            ++it;
            continue;
        }
        io.AddMouseButtonEvent(it->first, false);
        it = MouseWindows.erase(it);
    }
    if (!keyboard) return;
    for (auto it = KeyWindows.begin(); it != KeyWindows.end();) {
        if (it->second != window) {
            ++it;
            continue;
        }
        const auto key = TranslateKey(it->first);
        if (key != ImGuiKey_None) io.AddKeyEvent(key, false);
        Keys[it->first] = false;
        it = KeyWindows.erase(it);
    }
    io.AddKeyEvent(ImGuiMod_Ctrl, Keys[KeyCode::LEFT_CONTROL] || Keys[KeyCode::RIGHT_CONTROL]);
    io.AddKeyEvent(ImGuiMod_Shift, Keys[KeyCode::LEFT_SHIFT] || Keys[KeyCode::RIGHT_SHIFT]);
    io.AddKeyEvent(ImGuiMod_Alt, Keys[KeyCode::LEFT_ALT] || Keys[KeyCode::RIGHT_ALT]);
    io.AddKeyEvent(ImGuiMod_Super, Keys[KeyCode::LEFT_SUPER] || Keys[KeyCode::RIGHT_SUPER]);
}
void ImGuiSystem::Impl::RefreshMonitors() {
    if (!MonitorsDirty) return;
    MonitorsDirty = false;
    auto& platform = ImGui::GetPlatformIO();
    platform.Monitors.clear();
    for (const auto& monitor : App.GetWindowManager()->GetMainWindow()->GetNativeWindow()->GetMonitors()) {
        ImGuiPlatformMonitor item;
        item.MainPos = Vec(monitor.Position);
        item.MainSize = Vec(monitor.Size);
        item.WorkPos = Vec(monitor.WorkPosition);
        item.WorkSize = Vec(monitor.WorkSize);
        item.DpiScale = monitor.DpiScale;
        platform.Monitors.push_back(item);
    }
}
void ImGuiSystem::Impl::InstallPlatform() {
    auto& platform = ImGui::GetPlatformIO();
    platform.Platform_CreateWindow = CreateWindow;
    platform.Platform_DestroyWindow = DestroyWindow;
    platform.Platform_ShowWindow = [](ImGuiViewport* vp) { if (auto w = Native(vp)) w->Show((vp->Flags & ImGuiViewportFlags_NoFocusOnAppearing) ? NativeWindowShowMode::NoActivate : NativeWindowShowMode::Default); };
    platform.Platform_SetWindowPos = [](ImGuiViewport* vp, ImVec2 p) { if (auto w = Native(vp)) w->SetPosition(int(p.x), int(p.y)); };
    platform.Platform_GetWindowPos = [](ImGuiViewport* vp) { auto w = Native(vp); return w ? Vec(w->GetPosition()) : vp->Pos; };
    platform.Platform_SetWindowSize = [](ImGuiViewport* vp, ImVec2 p) { if (auto w = Native(vp)) w->SetSize(std::max(1, int(p.x)), std::max(1, int(p.y))); };
    platform.Platform_GetWindowSize = [](ImGuiViewport* vp) { auto w = Native(vp); return w ? Vec(w->GetSize()) : vp->Size; };
    platform.Platform_GetWindowFramebufferScale = [](ImGuiViewport*) { return ImVec2(1, 1); };
    platform.Platform_SetWindowFocus = [](ImGuiViewport* vp) { if (auto w = Native(vp)) w->Focus(); };
    platform.Platform_GetWindowFocus = [](ImGuiViewport* vp) { auto w = Native(vp); return w && w->IsFocused(); };
    platform.Platform_GetWindowMinimized = [](ImGuiViewport* vp) { auto w = Native(vp); return !w || w->IsMinimized(); };
    platform.Platform_SetWindowTitle = [](ImGuiViewport* vp, const char* title) { if (auto w = Native(vp)) w->SetTitle(title); };
    platform.Platform_SetWindowAlpha = [](ImGuiViewport* vp, float alpha) { if (auto w = Native(vp)) w->SetAlpha(alpha); };
    platform.Platform_GetWindowDpiScale = [](ImGuiViewport* vp) { auto w = Native(vp); return w ? w->GetDpiScale() : 1.0f; };
    platform.Platform_UpdateWindow = [](ImGuiViewport* vp) {
        if (auto w = Native(vp)) {
            auto* parent = ImGui::FindViewportByID(vp->ParentViewportId);
            w->SetOwner(parent ? Native(parent) : Nullable<NativeWindow*>{nullptr});
            w->SetDecorated((vp->Flags & ImGuiViewportFlags_NoDecoration) == 0);
            w->SetShowInTaskbar((vp->Flags & ImGuiViewportFlags_NoTaskBarIcon) == 0);
            w->SetTopMost((vp->Flags & ImGuiViewportFlags_TopMost) != 0);
            w->SetFocusOnClick((vp->Flags & ImGuiViewportFlags_NoFocusOnClick) == 0);
            w->SetInputPassthrough((vp->Flags & ImGuiViewportFlags_NoInputs) != 0);
        }
    };
    platform.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
        auto& self = Current();
        auto text = self.App.GetWindowManager()->GetMainWindow()->GetNativeWindow()->GetClipboardText();
        if (!text) return nullptr;
        self.Clipboard = std::move(*text);
        return self.Clipboard.c_str();
    };
    platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
        if (!Current().App.GetWindowManager()->GetMainWindow()->GetNativeWindow()->SetClipboardText(text)) RADRAY_WARN_LOG("ImGui clipboard write failed");
    };
    platform.Platform_SetImeDataFn = [](ImGuiContext*, ImGuiViewport* vp, ImGuiPlatformImeData* data) {
        if (auto window = Native(vp)) {
            Eigen::Vector2i position{int(data->InputPos.x), int(data->InputPos.y)};
            if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) position = window->ScreenToClient(position);
            window->SetImePosition(position, int(data->InputLineHeight), data->WantVisible);
        }
    };
    platform.Platform_OpenInShellFn = nullptr;
    platform.DrawCallback_ResetRenderState = ResetRenderState;
    platform.DrawCallback_SetSamplerLinear = SamplerLinear;
    platform.DrawCallback_SetSamplerNearest = SamplerNearest;
    Attach(ImGui::GetMainViewport(), App.GetWindowManager()->GetMainWindow(), true);
    RefreshMonitors();
}

bool ImGuiSystem::Initialize(const ImGuiSystemDescriptor& descriptor) {
    auto& self = *_impl;
    self.CheckThread();
    if (!descriptor.Enabled || self.Context || ImGui::GetCurrentContext() != nullptr ||
        !std::isfinite(descriptor.FontSize) || descriptor.FontSize <= 0 || !std::isfinite(descriptor.StyleScale) || descriptor.StyleScale <= 0) return false;
    auto* main = self.App.GetWindowManager()->GetMainWindow();
    if (!main || main->GetNativeWindow()->GetType() != NativeWindowType::Win32HWND) return false;
    const auto capabilities = main->GetNativeWindow()->GetDesktopCapabilities();
    if (!capabilities.MouseCursors || !capabilities.MousePosition || !capabilities.Clipboard || !capabilities.Ime || !capabilities.Monitors || !capabilities.HoveredWindow) return false;
    self.Descriptor = descriptor;
    ImGui::SetAllocatorFunctions([](size_t size, void*) { return radray::Malloc(size); }, [](void* memory, void*) { radray::Free(memory); });
    IMGUI_CHECKVERSION();
    static_assert(sizeof(ImTextureID) == 8 && sizeof(ImDrawIdx) == 2 && sizeof(ImDrawVert) == 20);
    static_assert(offsetof(ImDrawVert, pos) == 0 && offsetof(ImDrawVert, uv) == 8 && offsetof(ImDrawVert, col) == 16);
    self.Context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.BackendPlatformUserData = &self;
    io.BackendPlatformName = "radray_window";
    io.BackendRendererName = "radray_render_graph";
    io.ConfigFlags = ImGuiConfigFlags_IsSRGB;
    if (descriptor.KeyboardNavigation) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (descriptor.Docking) io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (descriptor.Viewports) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.BackendFlags = ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_HasSetMousePos | ImGuiBackendFlags_HasMouseHoveredViewport |
                      ImGuiBackendFlags_HasParentViewport | ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_RendererHasViewports |
                      ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    io.ConfigViewportsNoTaskBarIcon = true;
    io.ConfigViewportsNoDefaultParent = false;
    io.ConfigViewportsNoDecoration = true;
    io.ConfigViewportsPlatformFocusSetsImGuiFocus = true;
    io.ConfigDpiScaleFonts = io.ConfigDpiScaleViewports = true;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigMacOSXBehaviors = false;
    io.ConfigNavSwapGamepadButtons = io.ConfigNavMoveSetMousePos = false;
    io.ConfigNavCaptureKeyboard = io.ConfigNavEscapeClearFocusItem = io.ConfigNavCursorVisibleAuto = true;
    io.ConfigNavEscapeClearFocusWindow = io.ConfigNavCursorVisibleAlways = false;
    io.ConfigDockingNoSplit = io.ConfigDockingNoDockingOver = io.ConfigDockingWithShift = io.ConfigDockingAlwaysTabBar = io.ConfigDockingTransparentPayload = false;
    io.ConfigViewportsNoAutoMerge = false;
    io.ConfigInputTrickleEventQueue = io.ConfigInputTextCursorBlink = true;
    io.ConfigInputTextEnterKeepActive = io.ConfigDragClickToInputText = false;
    io.ConfigColorEditFlags = ImGuiColorEditFlags_DefaultOptions_;
    io.ConfigWindowsResizeFromEdges = io.ConfigScrollbarScrollByPage = true;
    io.ConfigWindowsCopyContentsWithCtrlC = io.MouseDrawCursor = io.FontAllowUserScaling = false;
    io.ConfigMemoryCompactTimer = 60;
    io.MouseDoubleClickTime = .30f;
    io.MouseDoubleClickMaxDist = 6;
    io.MouseDragThreshold = 6;
    io.MouseSingleClickDelay = .50f;
    io.KeyRepeatDelay = .275f;
    io.KeyRepeatRate = .050f;
    io.IniFilename = io.LogFilename = nullptr;
    io.IniSavingRate = 5;
    io.ConfigIniSettingsSaveLastUsedDate = true;
    io.ConfigIniSettingsAutoDiscardMonths = 0;
    io.ConfigDebugIniSettings = false;
    io.ConfigErrorRecovery = io.ConfigErrorRecoveryEnableAssert = io.ConfigErrorRecoveryEnableDebugLog = io.ConfigErrorRecoveryEnableTooltip = true;
    io.ConfigDebugHighlightIdConflicts = io.ConfigDebugHighlightIdConflictsShowItemPicker = true;
    io.ConfigDebugBeginReturnValueOnce = io.ConfigDebugBeginReturnValueLoop = io.ConfigDebugIgnoreFocusLoss = false;
    auto& platform = ImGui::GetPlatformIO();
    platform.Renderer_TextureMaxWidth = platform.Renderer_TextureMaxHeight = int(self.App.GetDevice()->GetCapabilities().Limits.MaxTexture2DDimension);
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.FontSizeBase = descriptor.FontSize;
    style.FontScaleMain = 1;
    style.WindowRounding = 0;
    style.Colors[ImGuiCol_WindowBg].w = 1;
    self.Baseline = style;
    SetStyleScale(descriptor.StyleScale);
    self.InstallPlatform();
    for (auto& font : self.Descriptor.Fonts) {
        auto bytes = ReadBinaryFile(font.Path);
        if (!bytes || bytes->empty() || bytes->size() > INT_MAX) {
            RADRAY_ERR_LOG("ImGui font read failed: {}", font.Path.string());
            return false;
        }
        void* owned = ImGui::MemAlloc(bytes->size());
        std::memcpy(owned, bytes->data(), bytes->size());
        ImFontConfig config = font.Config;
        config.FontDataOwnedByAtlas = true;
        if (font.ExcludeRanges.empty() && config.GlyphExcludeRanges) {
            for (auto* range = config.GlyphExcludeRanges; *range; range += 2) {
                font.ExcludeRanges.push_back(range[0]);
                font.ExcludeRanges.push_back(range[1]);
            }
        }
        if (!font.ExcludeRanges.empty()) {
            if (font.ExcludeRanges.back() != 0) font.ExcludeRanges.push_back(0);
            config.GlyphExcludeRanges = font.ExcludeRanges.data();
        }
        if (!io.Fonts->AddFontFromMemoryTTF(owned, int(bytes->size()), config.SizePixels > 0 ? config.SizePixels : descriptor.FontSize, &config)) return false;
    }
    if (!descriptor.SettingsPath.empty()) {
        if (auto settings = ReadTextFile(descriptor.SettingsPath)) ImGui::LoadIniSettingsFromMemory(settings->data(), settings->size());
    }
    for (uint32_t i = 0; i < self.App.GetGpuSystem()->GetFlightDataCount(); ++i) self.Flights.push_back(make_unique<UiFlight>());
    return true;
}

void ImGuiSystem::Impl::SaveSettings() {
    if (Descriptor.SettingsPath.empty()) {
        ImGui::GetIO().WantSaveIniSettings = false;
        return;
    }
    size_t size = 0;
    const char* settings = ImGui::SaveIniSettingsToMemory(&size);
    if (WriteTextFile(Descriptor.SettingsPath, {settings, size}))
        ImGui::GetIO().WantSaveIniSettings = false;
    else {
        Error = true;
        RADRAY_ERR_LOG("ImGui settings could not be saved");
    }
}
ImGuiSystem::~ImGuiSystem() {
    auto& self = *_impl;
    self.CheckThread();
    if (!self.Context) return;
    ImGui::SetCurrentContext(self.Context.Get());
    if (self.InFrame) ImGui::EndFrame();
    self.SaveSettings();
    self.Flights.clear();
    self.GpuTextures.clear();
    self.Slots.clear();
    ImGui::DestroyPlatformWindows();
    for (auto& entry : self.Pending) {
        entry.first->QueueUserData = nullptr;
        entry.first->SetTexID(0);
        entry.first->SetStatus(ImTextureStatus_Destroyed);
    }
    self.Pending.clear();
    auto& io = ImGui::GetIO();
    io.BackendFlags = 0;
    io.BackendPlatformUserData = nullptr;
    ImGui::GetPlatformIO().ClearPlatformHandlers();
    ImGui::GetPlatformIO().ClearRendererHandlers();
    ImGui::DestroyContext(self.Context.Get());
}
void ImGuiSystem::SetStyleScale(float scale) {
    auto& self = *_impl;
    self.CheckThread();
    if (!self.Context || !std::isfinite(scale) || scale <= 0) return;
    ImGui::SetCurrentContext(self.Context.Get());
    auto& style = ImGui::GetStyle();
    const float dpi = style.FontScaleDpi;
    style = self.Baseline;
    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;
    style.FontScaleDpi = dpi;
}
ImTextureID ImGuiSystem::Impl::AddRecord(shared_ptr<UiTextureRecord> record) {
    CheckThread();
    for (uint32_t i = 0; i < Slots.size(); ++i)
        if (!Slots[i].Record && Slots[i].Generation != 0) {
            record->Id = (uint64_t(Slots[i].Generation) << 32) | uint64_t(i + 1);
            Slots[i].Record = std::move(record);
            return Slots[i].Record->Id;
        }
    if (Slots.size() >= UINT32_MAX) return 0;
    record->Id = (uint64_t{1} << 32) | uint64_t(Slots.size() + 1);
    Slots.push_back({1, std::move(record)});
    return Slots.back().Record->Id;
}
shared_ptr<UiTextureRecord> ImGuiSystem::Impl::FindRecord(ImTextureID id) const {
    const auto index = uint32_t(id) - 1;
    return index < Slots.size() && Slots[index].Generation == uint32_t(id >> 32) ? Slots[index].Record : nullptr;
}
ImTextureID ImGuiSystem::RegisterTexture(StreamingAssetRef<TextureAsset> asset, const ImGuiTextureDescriptor& descriptor) {
    _impl->CheckThread();
    if (!asset) return 0;
    auto record = make_shared<UiTextureRecord>();
    record->Asset = std::move(asset);
    record->Descriptor = descriptor;
    return _impl->AddRecord(std::move(record));
}
ImTextureID ImGuiSystem::RegisterTexture(shared_ptr<ImGuiTextureLease> lease, const ImGuiTextureDescriptor& descriptor) {
    _impl->CheckThread();
    if (!lease || !lease->_texture) return 0;
    auto record = make_shared<UiTextureRecord>();
    record->Lease = std::move(lease);
    record->Descriptor = descriptor;
    return _impl->AddRecord(std::move(record));
}
ImTextureID ImGuiSystem::CreateGraphImage(const ImGuiTextureDescriptor& descriptor) {
    auto record = make_shared<UiTextureRecord>();
    record->Graph = true;
    record->Descriptor = descriptor;
    return _impl->AddRecord(std::move(record));
}
bool ImGuiSystem::UnregisterTexture(ImTextureID id) {
    _impl->CheckThread();
    auto record = _impl->FindRecord(id);
    if (!record || record->Dynamic) return false;
    auto& slot = _impl->Slots[uint32_t(id) - 1];
    slot.Record.reset();
    ++slot.Generation;
    return true;
}
bool ImGuiSystem::HasError() const noexcept { return _impl->Error.load(); }

void ImGuiSystem::BeginUpdate(uint32_t flightIndex) {
    auto& self = *_impl;
    self.CheckThread();
    ImGui::SetCurrentContext(self.Context.Get());
    for (auto& flight : self.Flights) {
        if (!flight->Completed.exchange(false, std::memory_order_acq_rel) || !flight->GraphSuccess) continue;
        for (const auto& request : flight->Requests) {
            auto it = std::find_if(self.Pending.begin(), self.Pending.end(), [&](const auto& pair) { return pair.second.Id == request.Id; });
            if (it == self.Pending.end() || it->second.Version != request.Version) continue;
            auto* texture = it->first;
            if (request.Status == ImTextureStatus_WantDestroy) {
                texture->SetTexID(0);
                texture->SetStatus(ImTextureStatus_Destroyed);
                texture->QueueUserData = nullptr;
                auto& slot = self.Slots[uint32_t(request.Id) - 1];
                slot.Record.reset();
                ++slot.Generation;
                self.Pending.erase(it);
            } else {
                texture->SetTexID(request.Id);
                texture->SetStatus(ImTextureStatus_OK);
                texture->QueueUserData = nullptr;
            }
        }
    }
    auto& flight = *self.Flights[flightIndex];
    flight.Viewports.clear();
    flight.Textures.clear();
    flight.Requests.clear();
    flight.Retained.clear();
    flight.ExternalTextures.clear();
    flight.ExternalBuffers.clear();
    flight.Uploads.clear();
    flight.AssetStates.clear();
    flight.AssetValid.clear();
    flight.UploadPasses.clear();
    flight.GraphSuccess = false;
    flight.Valid = true;
}
bool ImGuiSystem::NewFrame(const AppUpdateContext& context) {
    auto& self = *_impl;
    self.CheckThread();
    if (self.InFrame) return false;
    ImGui::SetCurrentContext(self.Context.Get());
    self.InFrame = true;
    auto& io = ImGui::GetIO();
    auto* windows = self.App.GetWindowManager();
    auto* main = windows->GetMainWindow()->GetNativeWindow();
    self.RefreshMonitors();
    bool focused = false;
    for (size_t i = 0; i < windows->GetWindowCount(); ++i) focused |= windows->GetWindow(i)->GetNativeWindow()->IsFocused();
    vector<std::pair<NativeWindow*, const WindowInputEvent*>> rawEvents;
    for (size_t i = 0; i < windows->GetWindowCount(); ++i) {
        auto* window = windows->GetWindow(i);
        auto* native = window->GetNativeWindow();
        for (const auto& event : window->GetInput().Pending()) rawEvents.emplace_back(native, &event);
    }
    std::sort(rawEvents.begin(), rawEvents.end(), [](const auto& a, const auto& b) { return a.second->Sequence < b.second->Sequence; });
    for (const auto& [native, raw] : rawEvents) {
        const auto& event = *raw;
        switch (event.Type) {
            case WindowInputType::Move: {
                auto p = event.Position.cast<int>().eval();
                if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) p = native->ClientToScreen(p);
                io.AddMousePosEvent(float(p.x()), float(p.y()));
                break;
            }
            case WindowInputType::Button: {
                const int button = int(event.Button) - int(MouseButton::BUTTON_1);
                if (button >= 0 && button < 5) {
                    if (event.State != Action::RELEASED)
                        self.MouseWindows.insert_or_assign(button, native);
                    else
                        self.MouseWindows.erase(button);
                    io.AddMouseButtonEvent(button, event.State != Action::RELEASED);
                }
                break;
            }
            case WindowInputType::Key: {
                if (event.State == Action::REPEATED) break;
                const auto key = TranslateKey(event.Key);
                self.Keys[event.Key] = event.State != Action::RELEASED;
                if (event.State != Action::RELEASED)
                    self.KeyWindows.insert_or_assign(event.Key, native);
                else
                    self.KeyWindows.erase(event.Key);
                io.AddKeyEvent(ImGuiMod_Ctrl, self.Keys[KeyCode::LEFT_CONTROL] || self.Keys[KeyCode::RIGHT_CONTROL]);
                io.AddKeyEvent(ImGuiMod_Shift, self.Keys[KeyCode::LEFT_SHIFT] || self.Keys[KeyCode::RIGHT_SHIFT]);
                io.AddKeyEvent(ImGuiMod_Alt, self.Keys[KeyCode::LEFT_ALT] || self.Keys[KeyCode::RIGHT_ALT]);
                io.AddKeyEvent(ImGuiMod_Super, self.Keys[KeyCode::LEFT_SUPER] || self.Keys[KeyCode::RIGHT_SUPER]);
                if (key != ImGuiKey_None) io.AddKeyEvent(key, event.State != Action::RELEASED);
                break;
            }
            case WindowInputType::Scroll: io.AddMouseWheelEvent(event.Position.x(), event.Position.y()); break;
            case WindowInputType::Text: io.AddInputCharactersUTF8(event.Text.c_str()); break;
            case WindowInputType::CaptureLost: self.ReleaseWindowInput(native, false); break;
            case WindowInputType::Focus:
                if (!event.Focused) self.ReleaseWindowInput(native, true);
                break;
            default: break;
        }
    }
    if (focused != self.Focused) {
        io.AddFocusEvent(focused);
        self.Focused = focused;
        if (!focused) {
            self.Keys.clear();
            self.KeyWindows.clear();
            self.MouseWindows.clear();
        }
    }
    if (focused) {
        if (io.WantSetMousePos) {
            Eigen::Vector2i pos{int(io.MousePos.x), int(io.MousePos.y)};
            if (!(io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) pos = main->ClientToScreen(pos);
            main->SetDesktopMousePosition(pos);
        } else if (auto pos = main->GetDesktopMousePosition()) {
            if (!(io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) *pos = main->ScreenToClient(*pos);
            io.AddMousePosEvent(float(pos->x()), float(pos->y()));
        }
    } else
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    ImGuiID hovered = 0;
    auto nativeHovered = main->GetHoveredWindow();
    for (auto* viewport : ImGui::GetPlatformIO().Viewports)
        if (nativeHovered && Impl::Native(viewport) == nativeHovered) hovered = viewport->ID;
    io.AddMouseViewportEvent(hovered);
    io.DisplaySize = main->IsMinimized() ? ImVec2(0, 0) : Vec(main->GetSize());
    io.DisplayFramebufferScale = {1, 1};
    io.DeltaTime = std::max(context.DeltaTime.count(), .000001f);
    ImGui::NewFrame();
    for (size_t i = 0; i < windows->GetWindowCount(); ++i) windows->GetWindow(i)->GetInput().SetCapture(io.WantCaptureMouse, io.WantCaptureKeyboard);
    if (!(io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)) {
        static constexpr NativeCursor cursors[]{NativeCursor::Arrow, NativeCursor::TextInput, NativeCursor::ResizeAll, NativeCursor::ResizeNS, NativeCursor::ResizeEW, NativeCursor::ResizeNESW, NativeCursor::ResizeNWSE, NativeCursor::Hand, NativeCursor::Wait, NativeCursor::Progress, NativeCursor::NotAllowed};
        const int cursor = int(ImGui::GetMouseCursor());
        const auto shape = io.MouseDrawCursor || cursor < 0 ? NativeCursor::Hidden : cursors[std::min(cursor, int(std::size(cursors) - 1))];
        for (size_t i = 0; i < windows->GetWindowCount(); ++i) windows->GetWindow(i)->GetNativeWindow()->SetCursor(shape);
    }
    return true;
}

void ImGuiSystem::CaptureFrame(uint32_t flightIndex) {
    auto& self = *_impl;
    self.CheckThread();
    if (!self.InFrame) return;
    ImGui::Render();
    ImGui::UpdatePlatformWindows();
    auto& flight = *self.Flights[flightIndex];
    auto& platform = ImGui::GetPlatformIO();
    for (auto* texture : platform.Textures) {
        if (texture->Status == ImTextureStatus_Destroyed) continue;
        auto [entry, inserted] = self.Pending.try_emplace(texture);
        if (inserted) {
            auto record = make_shared<UiTextureRecord>();
            record->Dynamic = true;
            entry->second.Id = self.AddRecord(record);
        }
        auto& pending = entry->second;
        flight.Textures.emplace(pending.Id, self.FindRecord(pending.Id));
        if (texture->Status == ImTextureStatus_OK) continue;
        UiTextureRequest request;
        request.Id = pending.Id;
        request.Status = texture->Status;
        request.Width = uint32_t(texture->Width);
        request.Height = uint32_t(texture->Height);
        request.Format = texture->UseColors ? render::TextureFormat::RGBA8_UNORM_SRGB : render::TextureFormat::RGBA8_UNORM;
        if (texture->Status != ImTextureStatus_WantDestroy) {
            if (!texture->Pixels || texture->Width <= 0 || texture->Height <= 0 || texture->Width > platform.Renderer_TextureMaxWidth || texture->Height > platform.Renderer_TextureMaxHeight) {
                flight.Valid = false;
                self.Error = true;
                RADRAY_ERR_LOG("ImGui dynamic texture exceeds device limits or has no pixels");
                continue;
            }
            request.Pixels.resize(size_t(request.Width) * request.Height * 4);
            if (texture->Format == ImTextureFormat_RGBA32)
                std::memcpy(request.Pixels.data(), texture->Pixels, request.Pixels.size());
            else
                for (size_t i = 0; i < request.Pixels.size() / 4; ++i) {
                    request.Pixels[i * 4] = request.Pixels[i * 4 + 1] = request.Pixels[i * 4 + 2] = byte{255};
                    request.Pixels[i * 4 + 3] = byte{texture->Pixels[i]};
                }
            if (texture->Status == ImTextureStatus_WantCreate)
                request.Regions.push_back({0, 0, uint16_t(request.Width), uint16_t(request.Height)});
            else
                request.Regions.assign(texture->Updates.begin(), texture->Updates.end());
        }
        const uint64_t fingerprint = Fingerprint(request);
        if (pending.Version == 0 || pending.Fingerprint != fingerprint) {
            ++pending.Version;
            pending.Fingerprint = fingerprint;
        }
        request.Version = pending.Version;
        texture->QueueUserData = &self;
        flight.Requests.push_back(std::move(request));
    }
    for (const auto& slot : self.Slots)
        if (slot.Record && slot.Record->Graph) flight.Textures.emplace(slot.Record->Id, slot.Record);
    for (auto* viewport : platform.Viewports) {
        const auto* data = static_cast<Impl::PlatformWindow*>(viewport->PlatformUserData);
        const auto* draw = viewport->DrawData;
        if (!data || !data->Window || !draw || !draw->Valid || draw->DisplaySize.x <= 0 || draw->DisplaySize.y <= 0) continue;
        UiViewportFrame snapshot;
        snapshot.Output = data->Window->GetRenderOutputId();
        snapshot.Position = draw->DisplayPos;
        snapshot.Size = draw->DisplaySize;
        snapshot.Scale = draw->FramebufferScale;
        int sampler = 0;
        for (const auto* list : draw->CmdLists) {
            const uint32_t baseVertex = uint32_t(snapshot.Vertices.size()), baseIndex = uint32_t(snapshot.Indices.size());
            snapshot.Vertices.insert(snapshot.Vertices.end(), list->VtxBuffer.begin(), list->VtxBuffer.end());
            snapshot.Indices.insert(snapshot.Indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
            for (const auto& command : list->CmdBuffer) {
                if (command.UserCallback) {
                    if (command.UserCallback == ResetRenderState)
                        sampler = 0;
                    else if (command.UserCallback == SamplerLinear)
                        sampler = 1;
                    else if (command.UserCallback == SamplerNearest)
                        sampler = 2;
                    else {
                        flight.Valid = false;
                        self.Error = true;
                        RADRAY_ERR_LOG("ImGui native draw callbacks are unsupported; use an explicit RenderGraph pass");
                    }
                    continue;
                }
                if (command.ElemCount == 0) continue;
                ImTextureID id = command.TexRef._TexID;
                if (command.TexRef._TexData) {
                    auto found = self.Pending.find(command.TexRef._TexData);
                    id = found != self.Pending.end() ? found->second.Id : 0;
                }
                auto record = self.FindRecord(id);
                if (!record) {
                    flight.Valid = false;
                    self.Error = true;
                    RADRAY_ERR_LOG("ImGui image {} is not registered or has been unregistered", id);
                    continue;
                }
                flight.Textures.emplace(id, record);
                snapshot.Commands.push_back({command.ClipRect, id, command.ElemCount, baseIndex + command.IdxOffset, int32_t(baseVertex + command.VtxOffset), sampler});
            }
        }
        flight.Viewports.push_back(std::move(snapshot));
    }
    if (ImGui::GetIO().WantSaveIniSettings) self.SaveSettings();
    self.InFrame = false;
}
void ImGuiSystem::NotifyFlightComplete(uint32_t flight, bool completed) noexcept {
    if (flight < _impl->Flights.size()) _impl->Flights[flight]->Completed.store(completed, std::memory_order_release);
}
void ImGuiSystem::RequestOutputs(uint32_t flight, RenderWorkloadBuilder& builder) const {
    for (const auto& viewport : _impl->Flights[flight]->Viewports) builder.RequestOutput(viewport.Output);
}

}  // namespace radray
