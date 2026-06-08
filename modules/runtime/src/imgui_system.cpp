#ifdef RADRAY_ENABLE_IMGUI

#include <radray/runtime/imgui_system.h>

#include <radray/logger.h>
#include <radray/window/native_window.h>

#include <fmt/format.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <algorithm>
#include <cmath>
#include <ranges>

#ifdef RADRAY_PLATFORM_WINDOWS
#include <radray/platform/win32_headers.h>
#ifdef CreateWindow
#undef CreateWindow
#endif
#ifdef DELETE
#undef DELETE
#endif
#endif

namespace radray {

constexpr render::DescriptorSetIndex ImGuiTextureSetIndex{1};
constexpr uint64_t ImGuiVertexBufferExtraCount = 5000;
constexpr uint64_t ImGuiIndexBufferExtraCount = 10000;

struct ImGuiPushConstants {
    float Scale[2]{};
    float Translate[2]{};
};

static uint64_t GetAlignedTextureUploadPitch(render::Device* device, ImTextureData* tex) noexcept {
    const uint64_t pitch = static_cast<uint64_t>(tex->Width) * static_cast<uint64_t>(tex->BytesPerPixel);
    const uint64_t alignment = std::max<uint64_t>(1, device->GetDetail().TextureDataPitchAlignment);
    return Align(pitch, alignment);
}

static uint64_t GetAlignedTextureUploadSize(render::Device* device, ImTextureData* tex) noexcept {
    return GetAlignedTextureUploadPitch(device, tex) * static_cast<uint64_t>(tex->Height);
}

static void ExtractDrawDataToFrame(ImGuiRenderer* renderer, ImGuiRenderer::Frame* frame, std::span<ImDrawData*> drawDataList);
static void OnRenderBeginFrame(ImGuiRenderer::Frame* frame, render::CommandBuffer* cmdBuffer);
static void OnRenderFrame(ImGuiRenderer* renderer, ImGuiRenderer::Frame* frame, uint32_t drawDataIndex, render::GraphicsCommandEncoder* encoder);
static void OnRenderCompleteFrame(ImGuiRenderer::Frame* frame);
static void SetupRenderStateForFrame(ImGuiRenderer* renderer, ImGuiRenderer::Frame* frame, uint32_t drawDataIndex, render::GraphicsCommandEncoder* encoder, int32_t fbWidth, int32_t fbHeight);

static NativeWindow* GetPlatformWindow(ImGuiViewport* viewport) noexcept {
    if (viewport == nullptr || viewport->PlatformUserData == nullptr) {
        return nullptr;
    }

    auto viewportWindow = static_cast<ImGuiSystem::ViewportWindow*>(viewport->PlatformUserData);
    return viewportWindow->GetWindow();
}

static Nullable<NativeWindow*> GetParentPlatformWindow(ImGuiViewport* viewport) noexcept {
    if (viewport == nullptr || viewport->ParentViewport == nullptr) {
        return nullptr;
    }
    return GetPlatformWindow(viewport->ParentViewport);
}

static ImGuiSystem::ViewportWindow* GetViewportWindow(ImGuiViewport* viewport) noexcept {
    if (viewport == nullptr || viewport->PlatformUserData == nullptr) {
        return nullptr;
    }
    return static_cast<ImGuiSystem::ViewportWindow*>(viewport->PlatformUserData);
}

static ImGuiSystem* GetBackendSystem() noexcept {
    return static_cast<ImGuiSystem*>(ImGui::GetIO().BackendPlatformUserData);
}

static bool IsSecondaryViewport(ImGuiViewport* viewport) noexcept {
    return viewport != nullptr && viewport->ID != ImGui::GetMainViewport()->ID;
}

static uint32_t GetViewportWidth(ImGuiViewport* viewport) noexcept {
    return static_cast<uint32_t>(std::max(viewport != nullptr ? viewport->Size.x : 0.0f, 1.0f));
}

static uint32_t GetViewportHeight(ImGuiViewport* viewport) noexcept {
    return static_cast<uint32_t>(std::max(viewport != nullptr ? viewport->Size.y : 0.0f, 1.0f));
}

static void DestroyViewportSwapChainTarget(ImGuiSystem::ViewportWindow* viewportWindow) noexcept {
    if (viewportWindow == nullptr) {
        return;
    }
    AppWindow* window = viewportWindow->Window;
    if (window == nullptr || !window->_swapchain) {
        return;
    }

    window->DetachSwapChain();
}

static bool CreateViewportSwapChainTarget(ImGuiSystem* system, ImGuiSystem::ViewportWindow* viewportWindow, ImGuiViewport* viewport) noexcept {
    if (system == nullptr || viewportWindow == nullptr || viewport == nullptr || viewportWindow->GetWindow() == nullptr) {
        return false;
    }
    if (system->_renderer == nullptr || system->_renderer->_device == nullptr) {
        return false;
    }

    DestroyViewportSwapChainTarget(viewportWindow);

    render::SwapChainDescriptor desc{};
    desc.Width = GetViewportWidth(viewport);
    desc.Height = GetViewportHeight(viewport);
    desc.Format = system->_renderTargetFormat;
    desc.PresentMode = system->_presentMode;
    viewportWindow->Window->AttachSwapChain(desc);
    return true;
}

static void RequestViewportSwapChainCreate(ImGuiSystem* system, ImGuiSystem::ViewportWindow* viewportWindow) noexcept {
    if (system == nullptr || viewportWindow == nullptr || viewportWindow->Viewport == nullptr || viewportWindow->GetWindow() == nullptr) {
        return;
    }
    CreateViewportSwapChainTarget(system, viewportWindow, viewportWindow->Viewport);
}

#ifdef RADRAY_PLATFORM_WINDOWS
static bool IsAnyImGuiWindowFocused(const ImGuiSystem* system) noexcept {
    if (system == nullptr) {
        return false;
    }

    if (system->_window != nullptr && system->_window->IsFocused()) {
        return true;
    }
    for (const auto& viewportWindow : system->_viewportWindows) {
        NativeWindow* window = viewportWindow->GetWindow();
        if (window != nullptr && window->IsFocused()) {
            return true;
        }
    }
    return false;
}

static void UpdateMouseState(ImGuiSystem* system) {
    if (system == nullptr || !system->_context.IsValid()) {
        return;
    }

    system->_context.SetCurrent();
    ImGuiIO& io = ImGui::GetIO();
    if (!IsAnyImGuiWindowFocused(system)) {
        return;
    }

    POINT mouseScreenPos{};
    if (::GetCursorPos(&mouseScreenPos) == 0) {
        return;
    }

    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        io.AddMousePosEvent(static_cast<float>(mouseScreenPos.x), static_cast<float>(mouseScreenPos.y));

        ImGuiID hoveredViewportId = 0;
        if (HWND hoveredHwnd = ::WindowFromPoint(mouseScreenPos)) {
            if (ImGuiViewport* hoveredViewport = ImGui::FindViewportByPlatformHandle(hoveredHwnd)) {
                hoveredViewportId = hoveredViewport->ID;
            }
        }
        io.AddMouseViewportEvent(hoveredViewportId);
    } else if (system->_window != nullptr) {
        Eigen::Vector2i mouseClientPos = system->_window->ScreenToClient(Eigen::Vector2i{mouseScreenPos.x, mouseScreenPos.y});
        io.AddMousePosEvent(static_cast<float>(mouseClientPos.x()), static_cast<float>(mouseClientPos.y()));
    }
}
#endif

static Win32WindowCreateDescriptor BuildViewportWindowDescriptor(ImGuiViewport* viewport) noexcept {
    Win32WindowCreateDescriptor desc{};
    desc.Title = "RadRay ImGui Viewport";
    desc.Width = static_cast<int32_t>(std::max(viewport->Size.x, 1.0f));
    desc.Height = static_cast<int32_t>(std::max(viewport->Size.y, 1.0f));
    desc.X = static_cast<int32_t>(viewport->Pos.x);
    desc.Y = static_cast<int32_t>(viewport->Pos.y);
    desc.Resizable = true;
    desc.StartVisible = false;
    desc.OwnerWindow = GetParentPlatformWindow(viewport);
    desc.Decorated = (viewport->Flags & ImGuiViewportFlags_NoDecoration) == 0;
    desc.ShowInTaskbar = (viewport->Flags & ImGuiViewportFlags_NoTaskBarIcon) == 0;
    desc.TopMost = (viewport->Flags & ImGuiViewportFlags_TopMost) != 0;
    desc.ActivateOnShow = (viewport->Flags & ImGuiViewportFlags_NoFocusOnAppearing) == 0;
    desc.FocusOnClick = (viewport->Flags & ImGuiViewportFlags_NoFocusOnClick) == 0;
    desc.InputPassthrough = (viewport->Flags & ImGuiViewportFlags_NoInputs) != 0;
    return desc;
}

static void ApplyViewportWindowFlags(ImGuiViewport* viewport, NativeWindow* window) noexcept {
    if (viewport == nullptr || window == nullptr) {
        return;
    }

    window->SetOwner(GetParentPlatformWindow(viewport));
    window->SetDecorated((viewport->Flags & ImGuiViewportFlags_NoDecoration) == 0);
    window->SetShowInTaskbar((viewport->Flags & ImGuiViewportFlags_NoTaskBarIcon) == 0);
    window->SetTopMost((viewport->Flags & ImGuiViewportFlags_TopMost) != 0);
    window->SetFocusOnClick((viewport->Flags & ImGuiViewportFlags_NoFocusOnClick) == 0);
    window->SetInputPassthrough((viewport->Flags & ImGuiViewportFlags_NoInputs) != 0);
}

static void ImGuiNativePlatform_CreateWindow(ImGuiViewport* viewport) {
    ImGuiSystem* system = GetBackendSystem();
    if (system == nullptr || system->_windowSystem == nullptr) {
        return;
    }

    Win32WindowCreateDescriptor desc = BuildViewportWindowDescriptor(viewport);
    AppWindow* window = system->_windowSystem->CreateWindow(desc, false);
    if (window == nullptr) {
        RADRAY_ERR_LOG("Failed to create ImGui viewport AppWindow");
        return;
    }

    auto viewportWindow = make_unique<ImGuiSystem::ViewportWindow>();
    viewportWindow->Viewport = viewport;
    viewportWindow->Window = window;
    NativeWindow* nativeWindow = viewportWindow->GetWindow();

    viewport->PlatformUserData = viewportWindow.get();
    viewport->PlatformHandle = viewport->PlatformHandleRaw = nativeWindow->GetNativeHandler();
    viewport->PlatformRequestResize = false;

    viewportWindow->AttachInput(system);
    system->_viewportWindows.push_back(std::move(viewportWindow));
}

static void ImGuiNativePlatform_DestroyWindow(ImGuiViewport* viewport) {
    if (viewport == nullptr) {
        return;
    }

    ImGuiSystem* system = GetBackendSystem();
    auto viewportWindow = static_cast<ImGuiSystem::ViewportWindow*>(viewport->PlatformUserData);
    viewport->PlatformUserData = nullptr;
    viewport->PlatformHandle = nullptr;
    viewport->PlatformHandleRaw = nullptr;

    if (!IsSecondaryViewport(viewport)) {
        return;
    }

    if (viewportWindow != nullptr && system != nullptr) {
        DestroyViewportSwapChainTarget(viewportWindow);
        AppWindow* appWindow = viewportWindow->Window;
        viewportWindow->Connections.clear();
        viewportWindow->Window = nullptr;
        auto iter = std::ranges::find_if(system->_viewportWindows, [viewportWindow](const unique_ptr<ImGuiSystem::ViewportWindow>& item) {
            return item.get() == viewportWindow;
        });
        if (iter != system->_viewportWindows.end()) {
            system->_viewportWindows.erase(iter);
        }
        if (appWindow != nullptr && system->_windowSystem != nullptr) {
            system->_windowSystem->DestroyWindow(appWindow);
        }
    }
}

static void ImGuiNativePlatform_ShowWindow(ImGuiViewport* viewport) {
    NativeWindow* window = GetPlatformWindow(viewport);
    if (window == nullptr) {
        return;
    }

    window->Show((viewport->Flags & ImGuiViewportFlags_NoFocusOnAppearing) != 0
                     ? NativeWindowShowMode::NoActivate
                     : NativeWindowShowMode::Default);
}

static void ImGuiNativePlatform_SetWindowPos(ImGuiViewport* viewport, ImVec2 pos) {
    NativeWindow* window = GetPlatformWindow(viewport);
    if (window != nullptr) {
        window->SetPosition(static_cast<int>(pos.x), static_cast<int>(pos.y));
    }
}

static ImVec2 ImGuiNativePlatform_GetWindowPos(ImGuiViewport* viewport) {
    NativeWindow* window = GetPlatformWindow(viewport);
    if (window == nullptr) {
        return viewport != nullptr ? viewport->Pos : ImVec2{};
    }

    auto pos = window->GetPosition();
    return ImVec2{static_cast<float>(pos.x()), static_cast<float>(pos.y())};
}

static void ImGuiNativePlatform_SetWindowSize(ImGuiViewport* viewport, ImVec2 size) {
    NativeWindow* window = GetPlatformWindow(viewport);
    if (window != nullptr) {
        window->SetSize(static_cast<int>(std::max(size.x, 1.0f)), static_cast<int>(std::max(size.y, 1.0f)));
    }
}

static ImVec2 ImGuiNativePlatform_GetWindowSize(ImGuiViewport* viewport) {
    NativeWindow* window = GetPlatformWindow(viewport);
    if (window == nullptr) {
        return viewport != nullptr ? viewport->Size : ImVec2{};
    }

    auto size = window->GetSize();
    return ImVec2{static_cast<float>(size.x()), static_cast<float>(size.y())};
}

static void ImGuiNativePlatform_SetWindowFocus(ImGuiViewport* viewport) {
    NativeWindow* window = GetPlatformWindow(viewport);
    if (window != nullptr) {
        window->Focus();
    }
}

static bool ImGuiNativePlatform_GetWindowFocus(ImGuiViewport* viewport) {
    NativeWindow* window = GetPlatformWindow(viewport);
    return window != nullptr && window->IsFocused();
}

static bool ImGuiNativePlatform_GetWindowMinimized(ImGuiViewport* viewport) {
    NativeWindow* window = GetPlatformWindow(viewport);
    return window != nullptr && window->IsMinimized();
}

static void ImGuiNativePlatform_SetWindowTitle(ImGuiViewport* viewport, const char* title) {
    NativeWindow* window = GetPlatformWindow(viewport);
    if (window != nullptr) {
        window->SetTitle(title != nullptr ? std::string_view{title} : std::string_view{});
    }
}

static void ImGuiNativePlatform_SetWindowAlpha(ImGuiViewport* viewport, float alpha) {
    NativeWindow* window = GetPlatformWindow(viewport);
    if (window != nullptr) {
        window->SetAlpha(alpha);
    }
}

static void ImGuiNativePlatform_UpdateWindow(ImGuiViewport* viewport) {
    NativeWindow* window = GetPlatformWindow(viewport);
    if (window != nullptr) {
        ApplyViewportWindowFlags(viewport, window);
    }
}

static float ImGuiNativePlatform_GetWindowDpiScale(ImGuiViewport* viewport) {
    NativeWindow* window = GetPlatformWindow(viewport);
    return window != nullptr ? window->GetDpiScale() : 1.0f;
}

static ImVec2 ImGuiNativePlatform_GetWindowFramebufferScale(ImGuiViewport*) {
    return ImVec2{1.0f, 1.0f};
}

static void InitImGuiNativePlatform(ImGuiSystem* system, AppWindow* mainAppWindow) {
    NativeWindow* mainWindow = mainAppWindow != nullptr ? mainAppWindow->_window.get() : nullptr;
    if (mainWindow == nullptr) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "radray_imgui_native_platform";
    io.BackendPlatformUserData = system;
    io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;
#ifdef RADRAY_PLATFORM_WINDOWS
    io.BackendFlags |= ImGuiBackendFlags_HasMouseHoveredViewport;
#endif

    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    platformIO.Platform_CreateWindow = ImGuiNativePlatform_CreateWindow;
    platformIO.Platform_DestroyWindow = ImGuiNativePlatform_DestroyWindow;
    platformIO.Platform_ShowWindow = ImGuiNativePlatform_ShowWindow;
    platformIO.Platform_SetWindowPos = ImGuiNativePlatform_SetWindowPos;
    platformIO.Platform_GetWindowPos = ImGuiNativePlatform_GetWindowPos;
    platformIO.Platform_SetWindowSize = ImGuiNativePlatform_SetWindowSize;
    platformIO.Platform_GetWindowSize = ImGuiNativePlatform_GetWindowSize;
    platformIO.Platform_GetWindowFramebufferScale = ImGuiNativePlatform_GetWindowFramebufferScale;
    platformIO.Platform_SetWindowFocus = ImGuiNativePlatform_SetWindowFocus;
    platformIO.Platform_GetWindowFocus = ImGuiNativePlatform_GetWindowFocus;
    platformIO.Platform_GetWindowMinimized = ImGuiNativePlatform_GetWindowMinimized;
    platformIO.Platform_SetWindowTitle = ImGuiNativePlatform_SetWindowTitle;
    platformIO.Platform_SetWindowAlpha = ImGuiNativePlatform_SetWindowAlpha;
    platformIO.Platform_UpdateWindow = ImGuiNativePlatform_UpdateWindow;
    platformIO.Platform_GetWindowDpiScale = ImGuiNativePlatform_GetWindowDpiScale;

    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    auto mainViewportWindow = make_unique<ImGuiSystem::ViewportWindow>();
    mainViewportWindow->Viewport = mainViewport;
    mainViewportWindow->Window = mainAppWindow;
    mainViewport->PlatformUserData = mainViewportWindow.get();
    mainViewport->PlatformHandle = mainViewport->PlatformHandleRaw = mainWindow->GetNativeHandler();
    mainViewportWindow->AttachInput(system);
    system->_viewportWindows.push_back(std::move(mainViewportWindow));

    platformIO.Monitors.resize(0);
    ImGuiPlatformMonitor monitor{};
    monitor.MainPos = ImVec2{-32768.0f, -32768.0f};
    monitor.MainSize = ImVec2{65536.0f, 65536.0f};
    monitor.WorkPos = monitor.MainPos;
    monitor.WorkSize = monitor.MainSize;
    monitor.DpiScale = mainWindow->GetDpiScale();
    platformIO.Monitors.push_back(monitor);
}

static void ShutdownImGuiNativePlatform() {
    ImGui::DestroyPlatformWindows();

    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = nullptr;
    io.BackendPlatformUserData = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_PlatformHasViewports;
#ifdef RADRAY_PLATFORM_WINDOWS
    io.BackendFlags &= ~ImGuiBackendFlags_HasMouseHoveredViewport;
#endif
    ImGui::GetPlatformIO().ClearPlatformHandlers();
}

static void ImGuiRenderer_CreateWindow(ImGuiViewport* viewport) {
    if (viewport == nullptr) {
        return;
    }

    if (!IsSecondaryViewport(viewport)) {
        viewport->RendererUserData = ImGui::GetIO().BackendRendererUserData;
        return;
    }

    viewport->RendererUserData = viewport->PlatformUserData;
    RequestViewportSwapChainCreate(GetBackendSystem(), GetViewportWindow(viewport));
}

static void ImGuiRenderer_DestroyWindow(ImGuiViewport* viewport) {
    if (viewport == nullptr) {
        return;
    }

    if (IsSecondaryViewport(viewport)) {
        if (auto* viewportWindow = static_cast<ImGuiSystem::ViewportWindow*>(viewport->RendererUserData)) {
            DestroyViewportSwapChainTarget(viewportWindow);
        }
    }
    viewport->RendererUserData = nullptr;
}

static void ImGuiRenderer_SetWindowSize(ImGuiViewport* viewport, ImVec2) {
    (void)viewport;
}

static void ImGuiRenderer_RenderWindow(ImGuiViewport* viewport, void*) {
    (void)viewport;
}

static void ImGuiRenderer_SwapBuffers(ImGuiViewport* viewport, void*) {
    (void)viewport;
}

static void InitImGuiRendererBackend(ImGuiRenderer* renderer) {
    ImGuiIO& io = ImGui::GetIO();
    RADRAY_ASSERT(io.BackendRendererUserData == nullptr);
    RADRAY_ASSERT(renderer != nullptr);

    io.BackendRendererName = "radray_imgui_renderer";
    io.BackendRendererUserData = renderer;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset |
                       ImGuiBackendFlags_RendererHasTextures |
                       ImGuiBackendFlags_RendererHasViewports;

    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    platformIO.Renderer_CreateWindow = ImGuiRenderer_CreateWindow;
    platformIO.Renderer_DestroyWindow = ImGuiRenderer_DestroyWindow;
    platformIO.Renderer_SetWindowSize = ImGuiRenderer_SetWindowSize;
    platformIO.Renderer_RenderWindow = ImGuiRenderer_RenderWindow;
    platformIO.Renderer_SwapBuffers = ImGuiRenderer_SwapBuffers;

    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    mainViewport->RendererUserData = renderer;
}

static void ShutdownImGuiRendererBackend() {
    ImGui::DestroyPlatformWindows();

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset |
                         ImGuiBackendFlags_RendererHasTextures |
                         ImGuiBackendFlags_RendererHasViewports);
    ImGui::GetPlatformIO().ClearRendererHandlers();
}

ImGuiContextRAII::ImGuiContextRAII(ImFontAtlas* sharedFontAtlas)
    : _ctx(ImGui::CreateContext(sharedFontAtlas)) {}

ImGuiContextRAII::ImGuiContextRAII(ImGuiContextRAII&& other) noexcept
    : _ctx(other._ctx) {
    other._ctx = nullptr;
}

ImGuiContextRAII& ImGuiContextRAII::operator=(ImGuiContextRAII&& other) noexcept {
    ImGuiContextRAII temp{std::move(other)};
    swap(*this, temp);
    return *this;
}

ImGuiContextRAII::~ImGuiContextRAII() noexcept {
    this->Destroy();
}

bool ImGuiContextRAII::IsValid() const noexcept {
    return _ctx != nullptr;
}

void ImGuiContextRAII::Destroy() noexcept {
    if (_ctx != nullptr) {
        ImGui::DestroyContext(_ctx);
        _ctx = nullptr;
    }
}

ImGuiContext* ImGuiContextRAII::Get() const noexcept { return _ctx; }

void ImGuiContextRAII::SetCurrent() {
    ImGui::SetCurrentContext(_ctx);
}

ImGuiRenderer::ImGuiRenderer() noexcept = default;

ImGuiRenderer::~ImGuiRenderer() noexcept {
    _frames.clear();
    _aliveTexs.clear();
    _pso.reset();
    _rootSig.reset();
    _device = nullptr;
}

Nullable<unique_ptr<ImGuiRenderer>> ImGuiRenderer::Create(const ImGuiRendererDescriptor& desc) noexcept {
    if (desc.FlightDataCount == 0) {
        RADRAY_ERR_LOG("FlightDataCount must be greater than 0");
        return nullptr;
    }
    if (desc.Device == nullptr) {
        RADRAY_ERR_LOG("ImGuiRendererDescriptor Device must not be null");
        return nullptr;
    }
    if (desc.RenderTargetFormat == render::TextureFormat::UNKNOWN) {
        RADRAY_ERR_LOG("ImGuiRendererDescriptor RenderTargetFormat must be valid");
        return nullptr;
    }

    const render::RenderBackend backendType = desc.Device->GetBackend();
    unique_ptr<render::Shader> shaderVS;
    unique_ptr<render::Shader> shaderPS;
    if (backendType == render::RenderBackend::D3D12) {
        render::HlslShaderDesc reflVS{};
        reflVS.BoundResources.push_back(render::HlslInputBindDesc{
            .Name = "gPush",
            .Type = render::HlslShaderInputType::CBUFFER,
            .BindPoint = 0,
            .BindCount = 1,
            .Space = 0,
        });
        auto& cbufferVS = reflVS.ConstantBuffers.emplace_back();
        cbufferVS.Name = "gPush";
        cbufferVS.Type = render::HlslCBufferType::CBUFFER;
        cbufferVS.Size = 16;
        cbufferVS.IsViewInHlsl = true;
        render::ShaderDescriptor descVS{
            GetImGuiVertexShaderDXIL(),
            render::ShaderBlobCategory::DXIL,
            render::ShaderStage::Vertex,
            render::ShaderReflectionDesc{std::move(reflVS)}};
        auto shaderVSOpt = desc.Device->CreateShader(descVS);
        if (!shaderVSOpt.HasValue()) {
            return nullptr;
        }
        shaderVS = shaderVSOpt.Release();

        render::HlslShaderDesc reflPS{};
        reflPS.BoundResources.push_back(render::HlslInputBindDesc{
            .Name = "gTexture",
            .Type = render::HlslShaderInputType::TEXTURE,
            .BindPoint = 0,
            .BindCount = 1,
            .ReturnType = render::HlslResourceReturnType::FLOAT,
            .Dimension = render::HlslSRVDimension::TEXTURE2D,
            .Space = 1,
        });
        reflPS.BoundResources.push_back(render::HlslInputBindDesc{
            .Name = "gSampler",
            .Type = render::HlslShaderInputType::SAMPLER,
            .BindPoint = 1,
            .BindCount = 1,
            .Space = 1,
        });
        render::ShaderDescriptor descPS{
            GetImGuiPixelShaderDXIL(),
            render::ShaderBlobCategory::DXIL,
            render::ShaderStage::Pixel,
            render::ShaderReflectionDesc{std::move(reflPS)}};
        auto shaderPSOpt = desc.Device->CreateShader(descPS);
        if (!shaderPSOpt.HasValue()) {
            return nullptr;
        }
        shaderPS = shaderPSOpt.Release();
    } else if (backendType == render::RenderBackend::Vulkan) {
        render::SpirvShaderDesc reflVS{};
        reflVS.PushConstants.push_back(render::SpirvPushConstantRange{
            .Name = "gPush",
            .Offset = 0,
            .Size = 16,
        });
        render::ShaderDescriptor descVS{
            GetImGuiVertexShaderSPIRV(),
            render::ShaderBlobCategory::SPIRV,
            render::ShaderStage::Vertex,
            render::ShaderReflectionDesc{std::move(reflVS)}};
        auto shaderVSOpt = desc.Device->CreateShader(descVS);
        if (!shaderVSOpt.HasValue()) {
            return nullptr;
        }
        shaderVS = shaderVSOpt.Release();

        render::SpirvShaderDesc reflPS{};
        reflPS.ResourceBindings.push_back(render::SpirvResourceBinding{
            .Name = "gTexture",
            .Kind = render::SpirvResourceKind::SeparateImage,
            .Set = 1,
            .Binding = 0,
            .ArraySize = 1,
            .ImageInfo = render::SpirvImageInfo{
                .Dim = render::SpirvImageDim::Dim2D,
                .SampledType = 0,
            },
        });
        auto& samplerPS = reflPS.ResourceBindings.emplace_back();
        samplerPS.Name = "gSampler";
        samplerPS.Kind = render::SpirvResourceKind::SeparateSampler;
        samplerPS.Set = 1;
        samplerPS.Binding = 1;
        samplerPS.ArraySize = 1;
        render::ShaderDescriptor descPS{
            GetImGuiPixelShaderSPIRV(),
            render::ShaderBlobCategory::SPIRV,
            render::ShaderStage::Pixel,
            render::ShaderReflectionDesc{std::move(reflPS)}};
        auto shaderPSOpt = desc.Device->CreateShader(descPS);
        if (!shaderPSOpt.HasValue()) {
            return nullptr;
        }
        shaderPS = shaderPSOpt.Release();
    } else {
        RADRAY_ERR_LOG("unsupported ImGui renderer backend {}", backendType);
        return nullptr;
    }

    render::SamplerDescriptor sampler{
        render::AddressMode::ClampToEdge,
        render::AddressMode::ClampToEdge,
        render::AddressMode::ClampToEdge,
        render::FilterMode::Linear,
        render::FilterMode::Linear,
        render::FilterMode::Linear,
        0.0f,
        std::numeric_limits<float>::max(),
        std::nullopt,
        0};
    render::StaticSamplerDescriptor staticSampler{
        "gSampler",
        render::DescriptorSetIndex{1},
        1,
        render::ShaderStage::Pixel,
        sampler};

    render::Shader* shaders[] = {shaderVS.get(), shaderPS.get()};
    render::RootSignatureDescriptor rsDesc{
        std::span<render::Shader*>{shaders},
        std::span<const render::StaticSamplerDescriptor>{&staticSampler, 1}};

    auto rootSigOpt = desc.Device->CreateRootSignature(rsDesc);
    if (!rootSigOpt.HasValue()) {
        return nullptr;
    }
    auto pushConstantId = rootSigOpt.Get()->FindParameterId("gPush");
    if (!pushConstantId.has_value()) {
        RADRAY_ERR_LOG("ImGui renderer root signature is missing gPush");
        return nullptr;
    }

    render::VertexElement vertexElems[] = {
        {offsetof(ImDrawVert, pos), "POSITION", 0, render::VertexFormat::FLOAT32X2, 0},
        {offsetof(ImDrawVert, uv), "TEXCOORD", 0, render::VertexFormat::FLOAT32X2, 1},
        {offsetof(ImDrawVert, col), "COLOR", 0, render::VertexFormat::UNORM8X4, 2}};
    render::VertexBufferLayout vbLayout{
        sizeof(ImDrawVert),
        render::VertexStepMode::Vertex,
        vertexElems};

    auto rtState = render::ColorTargetState::Default(desc.RenderTargetFormat);
    rtState.Blend = render::BlendState{
        {render::BlendFactor::SrcAlpha,
         render::BlendFactor::OneMinusSrcAlpha,
         render::BlendOperation::Add},
        {render::BlendFactor::One,
         render::BlendFactor::OneMinusSrcAlpha,
         render::BlendOperation::Add}};

    render::GraphicsPipelineStateDescriptor psoDesc{
        rootSigOpt.Get(),
        render::ShaderEntry{shaderVS.get(), "VSMain"},
        render::ShaderEntry{shaderPS.get(), "PSMain"},
        std::span<const render::VertexBufferLayout>{&vbLayout, 1},
        render::PrimitiveState::Default(),
        std::nullopt,
        render::MultiSampleState{1, std::numeric_limits<uint64_t>::max(), false},
        std::span<const render::ColorTargetState>{&rtState, 1}};
    psoDesc.Primitive.Cull = render::CullMode::None;

    auto psoOpt = desc.Device->CreateGraphicsPipelineState(psoDesc);
    if (!psoOpt.HasValue()) {
        return nullptr;
    }

    auto result = make_unique<ImGuiRenderer>();
    result->_device = desc.Device;
    result->_rootSig = rootSigOpt.Release();
    result->_pso = psoOpt.Release();
    result->_pushConstantId = pushConstantId.value();
    result->_frames.reserve(desc.FlightDataCount);
    for (uint32_t i = 0; i < desc.FlightDataCount; ++i) {
        result->_frames.emplace_back(make_unique<Frame>());
    }
    return result;
}

static void ExtractDrawDataToFrame(ImGuiRenderer* renderer, ImGuiRenderer::Frame* framePtr, std::span<ImDrawData*> drawDataList) {
    if (renderer == nullptr || framePtr == nullptr) {
        return;
    }
    auto& frame = *framePtr;
    frame._tempBufs.clear();
    frame._waitForFreeTexs.clear();
    frame._drawData.clear();
    frame._uploadTexReqs.clear();

    int32_t totalVtxCount = 0;
    int32_t totalIdxCount = 0;
    uint32_t debugViewportId = 0;
    uint32_t validDrawDataCount = 0;
    for (ImDrawData* drawData : drawDataList) {
        if (drawData == nullptr || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
            continue;
        }
        ++validDrawDataCount;
        if (debugViewportId == 0 && drawData->OwnerViewport != nullptr) {
            debugViewportId = drawData->OwnerViewport->ID;
        }
        totalVtxCount += drawData->TotalVtxCount;
        totalIdxCount += drawData->TotalIdxCount;
    }

    if (validDrawDataCount == 0) {
        return;
    }

    vector<ImTextureData*> processedTextures;
    for (ImDrawData* drawData : drawDataList) {
        if (drawData == nullptr || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f || drawData->Textures == nullptr) {
            continue;
        }
        for (ImTextureData* tex : *drawData->Textures) {
            if (std::ranges::find(processedTextures, tex) != processedTextures.end()) {
                continue;
            }
            processedTextures.push_back(tex);

            if (tex == nullptr || tex->Status == ImTextureStatus_OK || tex->Status == ImTextureStatus_Destroyed) {
                continue;
            }

            if (tex->Status == ImTextureStatus_WantCreate) {
                IM_ASSERT(tex->TexID == ImTextureID_Invalid && tex->BackendUserData == nullptr);
                IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);

                render::TextureDescriptor texDesc{
                    .Dim = render::TextureDimension::Dim2D,
                    .Width = static_cast<uint32_t>(tex->Width),
                    .Height = static_cast<uint32_t>(tex->Height),
                    .DepthOrArraySize = 1,
                    .MipLevels = 1,
                    .SampleCount = 1,
                    .Format = render::TextureFormat::RGBA8_UNORM,
                    .Memory = render::MemoryType::Device,
                    .Usage = render::TextureUse::Resource | render::TextureUse::CopyDestination,
                    .Hints = render::ResourceHint::None};
                auto texObjOpt = renderer->_device->CreateTexture(texDesc);
                if (!texObjOpt.HasValue()) {
                    continue;
                }
                auto texObj = texObjOpt.Release();
                texObj->SetDebugName(fmt::format("imgui_tex_{}", tex->UniqueID));

                render::TextureViewDescriptor texViewDesc{
                    .Target = texObj.get(),
                    .Dim = render::TextureDimension::Dim2D,
                    .Format = texDesc.Format,
                    .Range = render::SubresourceRange::AllSub(),
                    .Usage = render::TextureViewUsage::Resource};
                auto srvOpt = renderer->_device->CreateTextureView(texViewDesc);
                if (!srvOpt.HasValue()) {
                    continue;
                }
                auto srv = srvOpt.Release();
                srv->SetDebugName(fmt::format("imgui_tex_srv_{}", tex->UniqueID));

                auto descSetOpt = renderer->_device->CreateDescriptorSet(renderer->_rootSig.get(), ImGuiTextureSetIndex);
                if (!descSetOpt.HasValue()) {
                    continue;
                }
                auto descSet = descSetOpt.Release();
                descSet->SetDebugName(fmt::format("imgui_tex_set_{}", tex->UniqueID));
                if (!descSet->WriteResource("gTexture", srv.get())) {
                    continue;
                }

                auto& ptr = renderer->_aliveTexs.emplace_back(make_unique<ImGuiRenderer::ImGuiTexture>(std::move(texObj), std::move(srv), std::move(descSet)));
                tex->SetTexID(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(ptr.get())));
                tex->BackendUserData = ptr.get();
            }

            if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
                auto* backendTex = static_cast<ImGuiRenderer::ImGuiTexture*>(tex->BackendUserData);
                if (backendTex == nullptr) {
                    continue;
                }
                IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);

                const uint64_t uploadPitchSrc = static_cast<uint64_t>(tex->Width) * static_cast<uint64_t>(tex->BytesPerPixel);
                const uint64_t uploadPitchDst = GetAlignedTextureUploadPitch(renderer->_device, tex);
                const uint64_t uploadSize = GetAlignedTextureUploadSize(renderer->_device, tex);
                render::BufferDescriptor uploadDesc{
                    .Size = uploadSize,
                    .Memory = render::MemoryType::Upload,
                    .Usage = render::BufferUse::CopySource | render::BufferUse::MapWrite,
                    .Hints = render::ResourceHint::None};
                auto uploadBufferOpt = renderer->_device->CreateBuffer(uploadDesc);
                if (!uploadBufferOpt.HasValue()) {
                    continue;
                }

                auto uploadBuffer = uploadBufferOpt.Release();
                uploadBuffer->SetDebugName(fmt::format("imgui_tex_upload_{}", tex->UniqueID));
                auto* dst = static_cast<std::byte*>(uploadBuffer->Map(0, uploadSize));
                const auto* src = static_cast<const std::byte*>(tex->GetPixels());
                for (int32_t y = 0; y < tex->Height; ++y) {
                    std::memcpy(dst + static_cast<uint64_t>(y) * uploadPitchDst, src + static_cast<uint64_t>(y) * uploadPitchSrc, uploadPitchSrc);
                }
                uploadBuffer->Unmap(0, uploadSize);

                frame._uploadTexReqs.emplace_back(backendTex->_texture.get(), uploadBuffer.get(), tex->Status == ImTextureStatus_WantCreate);
                frame._tempBufs.emplace_back(std::move(uploadBuffer));
                tex->SetStatus(ImTextureStatus_OK);
            }

            if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames >= static_cast<int32_t>(renderer->_frames.size())) {
                auto* backendTex = static_cast<ImGuiRenderer::ImGuiTexture*>(tex->BackendUserData);
                auto iter = std::ranges::find_if(renderer->_aliveTexs, [backendTex](const unique_ptr<ImGuiRenderer::ImGuiTexture>& item) {
                    return item.get() == backendTex;
                });
                RADRAY_ASSERT(iter != renderer->_aliveTexs.end());
                auto texIns = std::move(*iter);
                *iter = std::move(renderer->_aliveTexs.back());
                renderer->_aliveTexs.pop_back();
                frame._waitForFreeTexs.emplace_back(std::move(texIns));
                tex->SetTexID(ImTextureID_Invalid);
                tex->BackendUserData = nullptr;
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    if (totalVtxCount > 0 &&
        (frame._vb == nullptr || frame._vbSize < totalVtxCount)) {
        if (frame._vb != nullptr) {
            frame._tempBufs.emplace_back(std::move(frame._vb));
        }
        const int32_t vertCount = totalVtxCount + static_cast<int32_t>(ImGuiVertexBufferExtraCount);
        render::BufferDescriptor vbDesc{
            .Size = static_cast<uint64_t>(vertCount) * sizeof(ImDrawVert),
            .Memory = render::MemoryType::Upload,
            .Usage = render::BufferUse::Vertex | render::BufferUse::MapWrite,
            .Hints = render::ResourceHint::None};
        auto vbOpt = renderer->_device->CreateBuffer(vbDesc);
        if (!vbOpt.HasValue()) {
            return;
        }
        frame._vb = vbOpt.Release();
        frame._vb->SetDebugName(fmt::format("imgui_vb_{:08X}", debugViewportId));
        frame._vbSize = vertCount;
    }
    if (totalIdxCount > 0 &&
        (frame._ib == nullptr || frame._ibSize < totalIdxCount)) {
        if (frame._ib != nullptr) {
            frame._tempBufs.emplace_back(std::move(frame._ib));
        }
        const int32_t idxCount = totalIdxCount + static_cast<int32_t>(ImGuiIndexBufferExtraCount);
        render::BufferDescriptor ibDesc{
            .Size = static_cast<uint64_t>(idxCount) * sizeof(ImDrawIdx),
            .Memory = render::MemoryType::Upload,
            .Usage = render::BufferUse::Index | render::BufferUse::MapWrite,
            .Hints = render::ResourceHint::None};
        auto ibOpt = renderer->_device->CreateBuffer(ibDesc);
        if (!ibOpt.HasValue()) {
            return;
        }
        frame._ib = ibOpt.Release();
        frame._ib->SetDebugName(fmt::format("imgui_ib_{:08X}", debugViewportId));
        frame._ibSize = idxCount;
    }

    if (totalVtxCount > 0) {
        const uint64_t bytes = static_cast<uint64_t>(totalVtxCount) * sizeof(ImDrawVert);
        auto* dst = static_cast<std::byte*>(frame._vb->Map(0, bytes));
        for (const ImDrawData* drawData : drawDataList) {
            if (drawData == nullptr || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
                continue;
            }
            for (const ImDrawList* drawList : drawData->CmdLists) {
                const uint64_t copyBytes = static_cast<uint64_t>(drawList->VtxBuffer.Size) * sizeof(ImDrawVert);
                std::memcpy(dst, drawList->VtxBuffer.Data, copyBytes);
                dst += copyBytes;
            }
        }
        frame._vb->Unmap(0, bytes);
    }
    if (totalIdxCount > 0) {
        const uint64_t bytes = static_cast<uint64_t>(totalIdxCount) * sizeof(ImDrawIdx);
        auto* dst = static_cast<std::byte*>(frame._ib->Map(0, bytes));
        for (const ImDrawData* drawData : drawDataList) {
            if (drawData == nullptr || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
                continue;
            }
            for (const ImDrawList* drawList : drawData->CmdLists) {
                const uint64_t copyBytes = static_cast<uint64_t>(drawList->IdxBuffer.Size) * sizeof(ImDrawIdx);
                std::memcpy(dst, drawList->IdxBuffer.Data, copyBytes);
                dst += copyBytes;
            }
        }
        frame._ib->Unmap(0, bytes);
    }

    uint32_t vtxOffset = 0;
    uint32_t idxOffset = 0;
    frame._drawData.reserve(drawDataList.size());
    for (const ImDrawData* drawData : drawDataList) {
        if (drawData == nullptr || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
            continue;
        }

        auto& dstDrawData = frame._drawData.emplace_back();
        dstDrawData.ViewportId = drawData->OwnerViewport != nullptr ? drawData->OwnerViewport->ID : 0;
        dstDrawData.DisplayPos = drawData->DisplayPos;
        dstDrawData.DisplaySize = drawData->DisplaySize;
        dstDrawData.FramebufferScale = drawData->FramebufferScale;
        dstDrawData.VtxOffset = vtxOffset;
        dstDrawData.IdxOffset = idxOffset;
        dstDrawData.TotalVtxCount = drawData->TotalVtxCount;
        dstDrawData.TotalIdxCount = drawData->TotalIdxCount;
        dstDrawData.Cmds.reserve(drawData->CmdLists.Size);
        for (const ImDrawList* drawList : drawData->CmdLists) {
            auto& dl = dstDrawData.Cmds.emplace_back();
            dl.VtxBufferSize = drawList->VtxBuffer.Size;
            dl.IdxBufferSize = drawList->IdxBuffer.Size;
            dl.Cmd.reserve(drawList->CmdBuffer.Size);
            for (const ImDrawCmd& srcCmd : drawList->CmdBuffer) {
                auto& dstCmd = dl.Cmd.emplace_back();
                dstCmd.ClipRect = srcCmd.ClipRect;
                dstCmd.VtxOffset = srcCmd.VtxOffset;
                dstCmd.IdxOffset = srcCmd.IdxOffset;
                dstCmd.ElemCount = srcCmd.ElemCount;
                dstCmd.UserCallback = srcCmd.UserCallback;

                if (srcCmd.TexRef._TexData != nullptr) {
                    dstCmd.Texture = static_cast<ImGuiRenderer::ImGuiTexture*>(srcCmd.TexRef._TexData->BackendUserData);
                    dstCmd.HasExternalTexture = false;
                } else if (srcCmd.TexRef._TexID != ImTextureID_Invalid) {
                    dstCmd.HasExternalTexture = true;
                }
            }
        }

        vtxOffset += static_cast<uint32_t>(drawData->TotalVtxCount);
        idxOffset += static_cast<uint32_t>(drawData->TotalIdxCount);
    }
}

void ImGuiRenderer::ExtractDrawData(uint32_t frameIndex) {
    RADRAY_ASSERT(frameIndex < _frames.size());

    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    vector<ImDrawData*> drawDataList;
    drawDataList.reserve(platformIO.Viewports.Size);
    for (ImGuiViewport* viewport : platformIO.Viewports) {
        if (viewport == nullptr || viewport->DrawData == nullptr) {
            continue;
        }
        drawDataList.push_back(viewport->DrawData);
    }
    ExtractDrawDataToFrame(this, _frames[frameIndex].get(), drawDataList);
}

uint32_t ImGuiRenderer::GetViewportDrawDataCount(uint32_t frameIndex) const noexcept {
    RADRAY_ASSERT(frameIndex < _frames.size());
    return static_cast<uint32_t>(_frames[frameIndex]->_drawData.size());
}

std::optional<uint32_t> ImGuiRenderer::FindViewportDrawDataIndex(uint32_t frameIndex, ImGuiViewport* viewport) const noexcept {
    RADRAY_ASSERT(frameIndex < _frames.size());
    if (viewport == nullptr) {
        return std::nullopt;
    }

    const auto& drawDataList = _frames[frameIndex]->_drawData;
    const auto iter = std::ranges::find_if(drawDataList, [viewport](const DrawData& drawData) {
        return drawData.ViewportId == viewport->ID;
    });
    if (iter == drawDataList.end()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(iter - drawDataList.begin());
}

static void OnRenderBeginFrame(ImGuiRenderer::Frame* framePtr, render::CommandBuffer* cmdBuffer) {
    if (framePtr == nullptr) {
        return;
    }
    auto& frame = *framePtr;
    if (cmdBuffer == nullptr) {
        return;
    }
    if (frame._uploadTexReqs.empty()) {
        return;
    }

    vector<render::ResourceBarrierDescriptor> barriers;
    barriers.reserve(frame._uploadTexReqs.size());
    for (const auto& payload : frame._uploadTexReqs) {
        barriers.emplace_back(render::BarrierTextureDescriptor{
            .Target = payload._dst,
            .Before = payload._isNew ? render::TextureState::Undefined : render::TextureState::ShaderRead,
            .After = render::TextureState::CopyDestination});
    }
    cmdBuffer->ResourceBarrier(barriers);

    for (const auto& payload : frame._uploadTexReqs) {
        cmdBuffer->CopyBufferToTexture(payload._dst, render::SubresourceRange{0, 1, 0, 1}, payload._src, 0);
    }

    barriers.clear();
    for (const auto& payload : frame._uploadTexReqs) {
        barriers.emplace_back(render::BarrierTextureDescriptor{
            .Target = payload._dst,
            .Before = render::TextureState::CopyDestination,
            .After = render::TextureState::ShaderRead});
    }
    cmdBuffer->ResourceBarrier(barriers);
    frame._uploadTexReqs.clear();
}

void ImGuiRenderer::OnRenderBegin(uint32_t frameIndex, render::CommandBuffer* cmdBuffer) {
    RADRAY_ASSERT(frameIndex < _frames.size());
    OnRenderBeginFrame(_frames[frameIndex].get(), cmdBuffer);
}

static void OnRenderFrame(ImGuiRenderer* renderer, ImGuiRenderer::Frame* framePtr, uint32_t drawDataIndex, render::GraphicsCommandEncoder* encoder) {
    if (renderer == nullptr || framePtr == nullptr) {
        return;
    }
    auto& frame = *framePtr;
    if (drawDataIndex >= frame._drawData.size()) {
        return;
    }
    const auto& drawData = frame._drawData[drawDataIndex];
    if (encoder == nullptr || drawData.DisplaySize.x <= 0.0f || drawData.DisplaySize.y <= 0.0f) {
        return;
    }

    const int32_t fbWidth = static_cast<int32_t>(drawData.DisplaySize.x * drawData.FramebufferScale.x);
    const int32_t fbHeight = static_cast<int32_t>(drawData.DisplaySize.y * drawData.FramebufferScale.y);
    if (fbWidth <= 0 || fbHeight <= 0) {
        return;
    }

    ImGuiPlatformIO& platIO = ImGui::GetPlatformIO();

    SetupRenderStateForFrame(renderer, framePtr, drawDataIndex, encoder, fbWidth, fbHeight);

    const ImVec2 clipOff = drawData.DisplayPos;
    const ImVec2 clipScale = drawData.FramebufferScale;
    int32_t globalVtxOffset = static_cast<int32_t>(drawData.VtxOffset);
    uint32_t globalIdxOffset = drawData.IdxOffset;
    for (const auto& drawList : drawData.Cmds) {
        for (const auto& cmd : drawList.Cmd) {
            if (cmd.UserCallback != nullptr) {
                if (cmd.UserCallback == platIO.DrawCallback_ResetRenderState) {
                    SetupRenderStateForFrame(renderer, framePtr, drawDataIndex, encoder, fbWidth, fbHeight);
                }
                continue;
            }

            if (cmd.HasExternalTexture) {
                // TODO: External ImTexID needs a descriptor-aware API. A raw render::Texture* is not enough to bind here.
                continue;
            }
            if (cmd.Texture == nullptr) {
                continue;
            }

            ImVec2 clipMin((cmd.ClipRect.x - clipOff.x) * clipScale.x, (cmd.ClipRect.y - clipOff.y) * clipScale.y);
            ImVec2 clipMax((cmd.ClipRect.z - clipOff.x) * clipScale.x, (cmd.ClipRect.w - clipOff.y) * clipScale.y);
            clipMin.x = std::max(clipMin.x, 0.0f);
            clipMin.y = std::max(clipMin.y, 0.0f);
            clipMax.x = std::min(clipMax.x, static_cast<float>(fbWidth));
            clipMax.y = std::min(clipMax.y, static_cast<float>(fbHeight));
            if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) {
                continue;
            }

            Rect scissor{
                .X = static_cast<int32_t>(clipMin.x),
                .Y = static_cast<int32_t>(clipMin.y),
                .Width = static_cast<uint32_t>(clipMax.x - clipMin.x),
                .Height = static_cast<uint32_t>(clipMax.y - clipMin.y)};
            encoder->SetScissor(scissor);
            encoder->BindDescriptorSet(ImGuiTextureSetIndex, cmd.Texture->_descriptorSet.get());
            encoder->DrawIndexed(
                cmd.ElemCount,
                1,
                cmd.IdxOffset + globalIdxOffset,
                static_cast<int32_t>(cmd.VtxOffset) + globalVtxOffset,
                0);
        }
        globalIdxOffset += drawList.IdxBufferSize;
        globalVtxOffset += drawList.VtxBufferSize;
    }

    encoder->SetScissor(Rect{0, 0, static_cast<uint32_t>(fbWidth), static_cast<uint32_t>(fbHeight)});
}

void ImGuiRenderer::OnRenderViewport(uint32_t frameIndex, ImGuiViewport* viewport, render::GraphicsCommandEncoder* encoder) {
    RADRAY_ASSERT(frameIndex < _frames.size());
    const auto drawDataIndex = FindViewportDrawDataIndex(frameIndex, viewport);
    if (!drawDataIndex.has_value()) {
        return;
    }
    OnRenderFrame(this, _frames[frameIndex].get(), drawDataIndex.value(), encoder);
}

static void OnRenderCompleteFrame(ImGuiRenderer::Frame* framePtr) {
    if (framePtr == nullptr) {
        return;
    }
    auto& frame = *framePtr;
    frame._uploadTexReqs.clear();
    frame._tempBufs.clear();
    frame._waitForFreeTexs.clear();
}

void ImGuiRenderer::OnRenderComplete(uint32_t frameIndex) {
    RADRAY_ASSERT(frameIndex < _frames.size());
    OnRenderCompleteFrame(_frames[frameIndex].get());
}

void ImGuiRenderer::OnSwapChainRecreate(const AppSwapChainRecreateContext& ctx) {
    (void)ctx;
    // Swapchain recreation can happen after ExtractDrawData; draw data is not swapchain-owned.
}

static void SetupRenderStateForFrame(ImGuiRenderer* renderer, ImGuiRenderer::Frame* framePtr, uint32_t drawDataIndex, render::GraphicsCommandEncoder* encoder, int32_t fbWidth, int32_t fbHeight) {
    if (renderer == nullptr || framePtr == nullptr || encoder == nullptr) {
        return;
    }
    const auto& frame = *framePtr;
    if (drawDataIndex >= frame._drawData.size()) {
        return;
    }
    const auto& drawData = frame._drawData[drawDataIndex];

    encoder->BindRootSignature(renderer->_rootSig.get());
    encoder->BindGraphicsPipelineState(renderer->_pso.get());
    if (drawData.TotalVtxCount > 0) {
        render::VertexBufferView vbv{
            .Target = frame._vb.get(),
            .Offset = 0,
            .Size = static_cast<uint64_t>(frame._vbSize) * sizeof(ImDrawVert)};
        encoder->BindVertexBuffer(std::span{&vbv, 1});

        render::IndexBufferView ibv{
            .Target = frame._ib.get(),
            .Offset = 0,
            .Stride = sizeof(ImDrawIdx)};
        encoder->BindIndexBuffer(ibv);
    }

    Viewport vp{
        .X = 0.0f,
        .Y = 0.0f,
        .Width = static_cast<float>(fbWidth),
        .Height = static_cast<float>(fbHeight),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f};
    if (renderer->_device->GetBackend() == render::RenderBackend::Vulkan) {
        vp.Y = static_cast<float>(fbHeight);
        vp.Height = -static_cast<float>(fbHeight);
    }
    encoder->SetViewport(vp);

    const float L = drawData.DisplayPos.x;
    const float R = drawData.DisplayPos.x + drawData.DisplaySize.x;
    const float T = drawData.DisplayPos.y;
    const float B = drawData.DisplayPos.y + drawData.DisplaySize.y;
    ImGuiPushConstants push{};
    push.Scale[0] = 2.0f / (R - L);
    push.Scale[1] = 2.0f / (T - B);
    push.Translate[0] = (R + L) / (L - R);
    push.Translate[1] = (T + B) / (B - T);
    encoder->PushConstants(renderer->_pushConstantId, &push, sizeof(push));
}

void ImGuiRenderer::SetupRenderState(uint32_t frameIndex, uint32_t drawDataIndex, render::GraphicsCommandEncoder* encoder, int32_t fbWidth, int32_t fbHeight) {
    RADRAY_ASSERT(frameIndex < _frames.size());
    SetupRenderStateForFrame(this, _frames[frameIndex].get(), drawDataIndex, encoder, fbWidth, fbHeight);
}

NativeWindow* ImGuiSystem::ViewportWindow::GetWindow() const noexcept {
    return Window != nullptr ? Window->_window.get() : nullptr;
}

render::SwapChain* ImGuiSystem::ViewportWindow::GetSwapChain() const noexcept {
    return Window != nullptr ? Window->_swapchain.Get() : nullptr;
}

render::TextureView* ImGuiSystem::ViewportWindow::GetOrCreateBackBufferView(const render::SwapChainFrame& frame, uint32_t ownerFlightIndex) const noexcept {
    return Window != nullptr ? Window->GetOrCreateBackBufferView(frame, ownerFlightIndex) : nullptr;
}

Nullable<unique_ptr<ImGuiSystem>> ImGuiSystem::Create(const ImGuiSystemDescriptor& desc) {
    if (desc.MainWindow == nullptr || desc.WindowSystem == nullptr || desc.Device == nullptr || desc.DirectQueue == nullptr) {
        RADRAY_ERR_LOG("ImGuiSystemDescriptor requires MainWindow, WindowSystem, Device and DirectQueue");
        return nullptr;
    }
    if (desc.RenderTargetFormat == render::TextureFormat::UNKNOWN) {
        RADRAY_ERR_LOG("ImGuiSystemDescriptor RenderTargetFormat must be valid");
        return nullptr;
    }
    if (desc.FlightDataCount == 0 || desc.BackBufferCount == 0) {
        RADRAY_ERR_LOG("ImGuiSystemDescriptor FlightDataCount and BackBufferCount must be greater than 0");
        return nullptr;
    }

    IMGUI_CHECKVERSION();
    ImGuiContextRAII imgui{};
    imgui.SetCurrent();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard |
                      ImGuiConfigFlags_DockingEnable |
                      ImGuiConfigFlags_ViewportsEnable;
    ImGui::StyleColorsDark();
    NativeWindow* mainWindow = desc.MainWindow->_window.get();
    float mainScale = mainWindow->GetDpiScale();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.Colors[ImGuiCol_WindowBg].w = 1;
    style.ScaleAllSizes(mainScale);
    style.FontScaleDpi = mainScale;
    io.Fonts->AddFontDefault();
    auto renderer = ImGuiRenderer::Create(ImGuiRendererDescriptor{
        desc.Device,
        desc.RenderTargetFormat,
        desc.FlightDataCount});
    if (!renderer.HasValue()) {
        return nullptr;
    }

    auto system = make_unique<ImGuiSystem>(std::move(imgui), mainWindow);
    renderer->_system = system.get();
    system->_renderer = renderer.Release();
    system->_windowSystem = desc.WindowSystem;
    system->_directQueue = desc.DirectQueue;
    system->_renderTargetFormat = desc.RenderTargetFormat;
    system->_flightDataCount = desc.FlightDataCount;
    system->_backBufferCount = desc.BackBufferCount;
    system->_presentMode = desc.PresentMode;
    system->_context.SetCurrent();
    InitImGuiRendererBackend(system->_renderer.get());
    InitImGuiNativePlatform(system.get(), desc.MainWindow);
    return system;
}

ImGuiSystem::ImGuiSystem(
    ImGuiContextRAII context,
    NativeWindow* window)
    : _context(std::move(context)),
      _window(window) {}

bool ImGuiSystem::BeginFrame(uint32_t frameIndex, float deltaTimeSeconds) {
    if (!_context.IsValid() || _renderer == nullptr) {
        return false;
    }

    if (_frameActive) {
        RADRAY_ERR_LOG("ImGuiSystem::BeginFrame called before EndFrame");
        RADRAY_ASSERT(false);
        return false;
    }

    _context.SetCurrent();
    ImGuiIO& io = ImGui::GetIO();
    const Eigen::Vector2i windowSize = _window != nullptr ? _window->GetSize() : Eigen::Vector2i{0, 0};
    io.DisplaySize = ImVec2{
        static_cast<float>(std::max(windowSize.x(), 0)),
        static_cast<float>(std::max(windowSize.y(), 0))};
    io.DisplayFramebufferScale = ImVec2{1.0f, 1.0f};
    io.DeltaTime = std::isfinite(deltaTimeSeconds) && deltaTimeSeconds > 0.0f ? deltaTimeSeconds : (1.0f / 60.0f);

#ifdef RADRAY_PLATFORM_WINDOWS
    UpdateMouseState(this);
#endif

    ImGui::NewFrame();
    _activeFrameIndex = frameIndex;
    _frameActive = true;
    return true;
}

void ImGuiSystem::EndFrame() {
    if (!_context.IsValid() || _renderer == nullptr) {
        return;
    }

    if (!_frameActive) {
        RADRAY_ERR_LOG("ImGuiSystem::EndFrame called before BeginFrame");
        RADRAY_ASSERT(false);
        return;
    }

    _context.SetCurrent();
    ImGui::Render();
    ImGuiIO& postRenderIo = ImGui::GetIO();
    if ((postRenderIo.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0 &&
        (postRenderIo.BackendFlags & ImGuiBackendFlags_PlatformHasViewports) != 0 &&
        (postRenderIo.BackendFlags & ImGuiBackendFlags_RendererHasViewports) != 0) {
        ImGui::UpdatePlatformWindows();
    }

    _activeFrameIndex = std::numeric_limits<uint32_t>::max();
    _frameActive = false;
}

void ImGuiSystem::ViewportWindow::AttachInput(ImGuiSystem* system) {
    NativeWindow* window = GetWindow();
    if (system == nullptr || window == nullptr) {
        return;
    }

    Connections.emplace_back(window->EventTouch().connect([system, window](int x, int y, MouseButton button, Action action) {
        system->_context.SetCurrent();
        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
#ifndef RADRAY_PLATFORM_WINDOWS
        Eigen::Vector2i pos{x, y};
        if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
            pos = window->ClientToScreen(pos);
        }
        io.AddMousePosEvent(static_cast<float>(pos.x()), static_cast<float>(pos.y()));
#else
        (void)window;
        (void)x;
        (void)y;
#endif

        const int imguiButton = MapMouseButtonToImGui(button);
        if (imguiButton >= 0 && action != Action::REPEATED && action != Action::UNKNOWN) {
            io.AddMouseButtonEvent(imguiButton, action == Action::PRESSED);
        }
    }));
    Connections.emplace_back(window->EventMouseLeave().connect([system]() {
        system->_context.SetCurrent();
#ifndef RADRAY_PLATFORM_WINDOWS
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
#endif
    }));
    Connections.emplace_back(window->EventMouseWheel().connect([system](int delta) {
        system->_context.SetCurrent();
        ImGui::GetIO().AddMouseWheelEvent(0.0f, static_cast<float>(delta) / 120.0f);
    }));
    Connections.emplace_back(window->EventKeyboard().connect([system](KeyCode key, Action action) {
        system->_context.SetCurrent();
        if (action == Action::UNKNOWN) {
            return;
        }
        const bool down = action != Action::RELEASED;
        switch (key) {
            case KeyCode::LEFT_CONTROL: system->_leftCtrl = down; break;
            case KeyCode::RIGHT_CONTROL: system->_rightCtrl = down; break;
            case KeyCode::LEFT_SHIFT: system->_leftShift = down; break;
            case KeyCode::RIGHT_SHIFT: system->_rightShift = down; break;
            case KeyCode::LEFT_ALT: system->_leftAlt = down; break;
            case KeyCode::RIGHT_ALT: system->_rightAlt = down; break;
            case KeyCode::LEFT_SUPER: system->_leftSuper = down; break;
            case KeyCode::RIGHT_SUPER: system->_rightSuper = down; break;
            default: break;
        }
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, system->_leftCtrl || system->_rightCtrl);
        io.AddKeyEvent(ImGuiMod_Shift, system->_leftShift || system->_rightShift);
        io.AddKeyEvent(ImGuiMod_Alt, system->_leftAlt || system->_rightAlt);
        io.AddKeyEvent(ImGuiMod_Super, system->_leftSuper || system->_rightSuper);
        const ImGuiKey imguiKey = MapKeyboardToImGuiKey(key);
        if (imguiKey != ImGuiKey_None) {
            io.AddKeyEvent(imguiKey, down);
        }
    }));
    Connections.emplace_back(window->EventTextInput().connect([system](std::string_view text) {
        system->_context.SetCurrent();
        if (!text.empty()) {
            ImGui::GetIO().AddInputCharactersUTF8(text.data());
        }
    }));
    Connections.emplace_back(window->EventFocused().connect([system](bool focused) {
        system->_context.SetCurrent();
        ImGuiIO& io = ImGui::GetIO();
        if (!focused) {
            system->_leftCtrl = false;
            system->_rightCtrl = false;
            system->_leftShift = false;
            system->_rightShift = false;
            system->_leftAlt = false;
            system->_rightAlt = false;
            system->_leftSuper = false;
            system->_rightSuper = false;
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            io.AddKeyEvent(ImGuiMod_Shift, false);
            io.AddKeyEvent(ImGuiMod_Alt, false);
            io.AddKeyEvent(ImGuiMod_Super, false);
        }
        io.AddFocusEvent(focused);
    }));
    Connections.emplace_back(window->EventCloseRequested().connect([system, viewport = Viewport]() {
        system->_context.SetCurrent();
        if (viewport != nullptr) {
            viewport->PlatformRequestClose = true;
        }
    }));
    Connections.emplace_back(window->EventMoved().connect([system, viewport = Viewport](int, int) {
        system->_context.SetCurrent();
        if (viewport != nullptr) {
            viewport->PlatformRequestMove = true;
        }
    }));
    Connections.emplace_back(window->EventResized().connect([system, viewport = Viewport](int, int) {
        system->_context.SetCurrent();
        if (viewport != nullptr) {
            viewport->PlatformRequestResize = true;
        }
    }));
}

ImGuiSystem::~ImGuiSystem() noexcept {
    this->Destroy();
}

bool ImGuiSystem::IsValid() const noexcept {
    return _context.IsValid();
}

void ImGuiSystem::Destroy() noexcept {
    for (const auto& viewportWindow : _viewportWindows) {
        if (viewportWindow != nullptr) {
            viewportWindow->Connections.clear();
        }
    }
    if (!_context.IsValid()) {
        _window = nullptr;
        return;
    }

    _context.SetCurrent();
    if (ImGui::GetIO().BackendRendererUserData != nullptr) {
        ShutdownImGuiRendererBackend();
    }
    if (ImGui::GetIO().BackendPlatformUserData != nullptr) {
        ShutdownImGuiNativePlatform();
    }
    _viewportWindows.clear();
    _renderer.reset();
    _context.Destroy();
    _window = nullptr;
}

ImGuiKey MapKeyboardToImGuiKey(KeyCode key) noexcept {
    switch (key) {
        case KeyCode::SPACE: return ImGuiKey_Space;
        case KeyCode::APOSTROPHE: return ImGuiKey_Apostrophe;
        case KeyCode::COMMA: return ImGuiKey_Comma;
        case KeyCode::MINUS: return ImGuiKey_Minus;
        case KeyCode::PERIOD: return ImGuiKey_Period;
        case KeyCode::SLASH: return ImGuiKey_Slash;
        case KeyCode::NUM0: return ImGuiKey_0;
        case KeyCode::NUM1: return ImGuiKey_1;
        case KeyCode::NUM2: return ImGuiKey_2;
        case KeyCode::NUM3: return ImGuiKey_3;
        case KeyCode::NUM4: return ImGuiKey_4;
        case KeyCode::NUM5: return ImGuiKey_5;
        case KeyCode::NUM6: return ImGuiKey_6;
        case KeyCode::NUM7: return ImGuiKey_7;
        case KeyCode::NUM8: return ImGuiKey_8;
        case KeyCode::NUM9: return ImGuiKey_9;
        case KeyCode::SEMICOLON: return ImGuiKey_Semicolon;
        case KeyCode::EQUAL: return ImGuiKey_Equal;
        case KeyCode::A: return ImGuiKey_A;
        case KeyCode::B: return ImGuiKey_B;
        case KeyCode::C: return ImGuiKey_C;
        case KeyCode::D: return ImGuiKey_D;
        case KeyCode::E: return ImGuiKey_E;
        case KeyCode::F: return ImGuiKey_F;
        case KeyCode::G: return ImGuiKey_G;
        case KeyCode::H: return ImGuiKey_H;
        case KeyCode::I: return ImGuiKey_I;
        case KeyCode::J: return ImGuiKey_J;
        case KeyCode::K: return ImGuiKey_K;
        case KeyCode::L: return ImGuiKey_L;
        case KeyCode::M: return ImGuiKey_M;
        case KeyCode::N: return ImGuiKey_N;
        case KeyCode::O: return ImGuiKey_O;
        case KeyCode::P: return ImGuiKey_P;
        case KeyCode::Q: return ImGuiKey_Q;
        case KeyCode::R: return ImGuiKey_R;
        case KeyCode::S: return ImGuiKey_S;
        case KeyCode::T: return ImGuiKey_T;
        case KeyCode::U: return ImGuiKey_U;
        case KeyCode::V: return ImGuiKey_V;
        case KeyCode::W: return ImGuiKey_W;
        case KeyCode::X: return ImGuiKey_X;
        case KeyCode::Y: return ImGuiKey_Y;
        case KeyCode::Z: return ImGuiKey_Z;
        case KeyCode::LEFT_BRACKET: return ImGuiKey_LeftBracket;
        case KeyCode::BACKSLASH: return ImGuiKey_Backslash;
        case KeyCode::RIGHT_BRACKET: return ImGuiKey_RightBracket;
        case KeyCode::GRAVE_ACCENT: return ImGuiKey_GraveAccent;
        case KeyCode::WORLD_1: return ImGuiKey_Oem102;
        case KeyCode::ESCAPE: return ImGuiKey_Escape;
        case KeyCode::ENTER: return ImGuiKey_Enter;
        case KeyCode::TAB: return ImGuiKey_Tab;
        case KeyCode::BACKSPACE: return ImGuiKey_Backspace;
        case KeyCode::INSERT: return ImGuiKey_Insert;
        case KeyCode::DELETE: return ImGuiKey_Delete;
        case KeyCode::RIGHT: return ImGuiKey_RightArrow;
        case KeyCode::LEFT: return ImGuiKey_LeftArrow;
        case KeyCode::DOWN: return ImGuiKey_DownArrow;
        case KeyCode::UP: return ImGuiKey_UpArrow;
        case KeyCode::PAGE_UP: return ImGuiKey_PageUp;
        case KeyCode::PAGE_DOWN: return ImGuiKey_PageDown;
        case KeyCode::HOME: return ImGuiKey_Home;
        case KeyCode::END: return ImGuiKey_End;
        case KeyCode::CAPS_LOCK: return ImGuiKey_CapsLock;
        case KeyCode::SCROLL_LOCK: return ImGuiKey_ScrollLock;
        case KeyCode::NUM_LOCK: return ImGuiKey_NumLock;
        case KeyCode::PRINT_SCREEN: return ImGuiKey_PrintScreen;
        case KeyCode::PAUSE: return ImGuiKey_Pause;
        case KeyCode::F1: return ImGuiKey_F1;
        case KeyCode::F2: return ImGuiKey_F2;
        case KeyCode::F3: return ImGuiKey_F3;
        case KeyCode::F4: return ImGuiKey_F4;
        case KeyCode::F5: return ImGuiKey_F5;
        case KeyCode::F6: return ImGuiKey_F6;
        case KeyCode::F7: return ImGuiKey_F7;
        case KeyCode::F8: return ImGuiKey_F8;
        case KeyCode::F9: return ImGuiKey_F9;
        case KeyCode::F10: return ImGuiKey_F10;
        case KeyCode::F11: return ImGuiKey_F11;
        case KeyCode::F12: return ImGuiKey_F12;
        case KeyCode::F13: return ImGuiKey_F13;
        case KeyCode::F14: return ImGuiKey_F14;
        case KeyCode::F15: return ImGuiKey_F15;
        case KeyCode::F16: return ImGuiKey_F16;
        case KeyCode::F17: return ImGuiKey_F17;
        case KeyCode::F18: return ImGuiKey_F18;
        case KeyCode::F19: return ImGuiKey_F19;
        case KeyCode::F20: return ImGuiKey_F20;
        case KeyCode::F21: return ImGuiKey_F21;
        case KeyCode::F22: return ImGuiKey_F22;
        case KeyCode::F23: return ImGuiKey_F23;
        case KeyCode::F24: return ImGuiKey_F24;
        case KeyCode::KP_0: return ImGuiKey_Keypad0;
        case KeyCode::KP_1: return ImGuiKey_Keypad1;
        case KeyCode::KP_2: return ImGuiKey_Keypad2;
        case KeyCode::KP_3: return ImGuiKey_Keypad3;
        case KeyCode::KP_4: return ImGuiKey_Keypad4;
        case KeyCode::KP_5: return ImGuiKey_Keypad5;
        case KeyCode::KP_6: return ImGuiKey_Keypad6;
        case KeyCode::KP_7: return ImGuiKey_Keypad7;
        case KeyCode::KP_8: return ImGuiKey_Keypad8;
        case KeyCode::KP_9: return ImGuiKey_Keypad9;
        case KeyCode::KP_DECIMAL: return ImGuiKey_KeypadDecimal;
        case KeyCode::KP_DIVIDE: return ImGuiKey_KeypadDivide;
        case KeyCode::KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
        case KeyCode::KP_SUBTRACT: return ImGuiKey_KeypadSubtract;
        case KeyCode::KP_ADD: return ImGuiKey_KeypadAdd;
        case KeyCode::KP_ENTER: return ImGuiKey_KeypadEnter;
        case KeyCode::KP_EQUAL: return ImGuiKey_KeypadEqual;
        case KeyCode::LEFT_SHIFT: return ImGuiKey_LeftShift;
        case KeyCode::LEFT_CONTROL: return ImGuiKey_LeftCtrl;
        case KeyCode::LEFT_ALT: return ImGuiKey_LeftAlt;
        case KeyCode::LEFT_SUPER: return ImGuiKey_LeftSuper;
        case KeyCode::RIGHT_SHIFT: return ImGuiKey_RightShift;
        case KeyCode::RIGHT_CONTROL: return ImGuiKey_RightCtrl;
        case KeyCode::RIGHT_ALT: return ImGuiKey_RightAlt;
        case KeyCode::RIGHT_SUPER: return ImGuiKey_RightSuper;
        case KeyCode::MENU: return ImGuiKey_Menu;
        default: return ImGuiKey_None;
    }
}

int MapMouseButtonToImGui(MouseButton button) noexcept {
    switch (button) {
        case MouseButton::BUTTON_LEFT: return 0;
        case MouseButton::BUTTON_RIGHT: return 1;
        case MouseButton::BUTTON_MIDDLE: return 2;
        case MouseButton::BUTTON_4: return 3;
        case MouseButton::BUTTON_5: return 4;
        default: return -1;
    }
}

}  // namespace radray

#endif
