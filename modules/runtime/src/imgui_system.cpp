#include <radray/runtime/imgui_system.h>

#include <algorithm>
#include <limits>

#include <imgui_internal.h>

#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/utility.h>


namespace radray {

extern bool InitImGuiInternal(ImGuiContext* ctx, NativeWindow* window);
extern void ShutdownImGuiInternal(ImGuiContext* ctx);

struct ImGuiViewportPlatformData {
    ImGuiViewport* Viewport{nullptr};
    AppWindow* Window{nullptr};
};

static ImGuiRenderer* GetImGuiRenderer() noexcept {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr) {
        return nullptr;
    }
    return static_cast<ImGuiRenderer*>(ImGui::GetIO(ctx).BackendRendererUserData);
}

static ImGuiSystem* GetImGuiSystem() noexcept {
    ImGuiRenderer* renderer = GetImGuiRenderer();
    return renderer != nullptr ? renderer->_system : nullptr;
}

static ImGuiViewportPlatformData* FindViewportPlatformData(ImGuiSystem* system, ImGuiViewport* viewport) noexcept {
    if (system == nullptr || viewport == nullptr) {
        return nullptr;
    }
    auto it = std::find_if(
        system->_viewportPlatformData.begin(),
        system->_viewportPlatformData.end(),
        [viewport](const unique_ptr<ImGuiViewportPlatformData>& data) {
            return data != nullptr && data->Viewport == viewport;
        });
    return it != system->_viewportPlatformData.end() ? it->get() : nullptr;
}

static AppWindow* GetViewportAppWindow(ImGuiSystem* system, ImGuiViewport* viewport) noexcept {
    if (system == nullptr || viewport == nullptr) {
        return nullptr;
    }
    if (ImGuiViewportPlatformData* data = FindViewportPlatformData(system, viewport)) {
        return data->Window;
    }
    return viewport == ImGui::GetMainViewport() ? system->_mainWnd : nullptr;
}

static int32_t ImGuiSizeToInt(float v) noexcept {
    return static_cast<int32_t>(std::max(v, 1.0f));
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

ImGuiRenderer::ImGuiRenderer(Application* app, AppWindow* mainWnd)
    : _app(app),
      _mainWnd(mainWnd) {}

ImGuiRenderer::~ImGuiRenderer() noexcept {
    _pso.reset();
    _rs.reset();
    _ps.reset();
    _vs.reset();
    _system = nullptr;
    _app = nullptr;
}

Nullable<unique_ptr<ImGuiRenderer>> ImGuiRenderer::Create(Application* app, AppWindow* mainWnd) {
    RADRAY_ASSERT(app != nullptr);
    RADRAY_ASSERT(mainWnd != nullptr);
    GpuSurface* mainSurface = mainWnd->_surface.get();
    RADRAY_ASSERT(mainSurface != nullptr);
    const render::SwapChainDescriptor mainSurfaceDesc = mainSurface->GetDesc();
    const render::TextureFormat backBufferFormat = mainSurfaceDesc.Format;
    RADRAY_ASSERT(backBufferFormat != render::TextureFormat::UNKNOWN);
    RADRAY_ASSERT(mainSurfaceDesc.BackBufferCount != 0);
    RADRAY_ASSERT(mainSurfaceDesc.FlightFrameCount != 0);
    render::Device* device = app->_gpu->GetDevice();
    RADRAY_ASSERT(device != nullptr);
    unique_ptr<render::Shader> vs, ps;
    {
        render::ShaderDescriptor vsDesc{};
        render::ShaderDescriptor psDesc{};
        vsDesc.Stages = render::ShaderStage::Vertex;
        psDesc.Stages = render::ShaderStage::Pixel;
        switch (device->GetBackend()) {
            case render::RenderBackend::D3D12: {
                render::HlslShaderDesc vsRefl{};
                vsRefl.ConstantBuffers.push_back(render::HlslShaderBufferDesc{
                    .Name = "gPush",
                    .Variables = {},
                    .Type = render::HlslCBufferType::CBUFFER,
                    .Size = 16,
                    .IsViewInHlsl = true});
                vsRefl.BoundResources.push_back(render::HlslInputBindDesc{
                    .Name = "gPush",
                    .Type = render::HlslShaderInputType::CBUFFER,
                    .BindPoint = 0,
                    .BindCount = 1,
                    .Space = 0});
                render::HlslShaderDesc psRefl{};
                psRefl.BoundResources.push_back(render::HlslInputBindDesc{
                    .Name = "gTexture",
                    .Type = render::HlslShaderInputType::TEXTURE,
                    .BindPoint = 0,
                    .BindCount = 1,
                    .ReturnType = render::HlslResourceReturnType::FLOAT,
                    .Dimension = render::HlslSRVDimension::TEXTURE2D,
                    .Space = 1,
                    .VkBinding = 0,
                    .VkSet = 1});
                psRefl.BoundResources.push_back(render::HlslInputBindDesc{
                    .Name = "gSampler",
                    .Type = render::HlslShaderInputType::SAMPLER,
                    .BindPoint = 1,
                    .BindCount = 1,
                    .Space = 1,
                    .VkBinding = 1,
                    .VkSet = 1});
                vsDesc.Source = GetImGuiVertexShaderDXIL();
                vsDesc.Category = render::ShaderBlobCategory::DXIL;
                vsDesc.Reflection = vsRefl;
                psDesc.Source = GetImGuiPixelShaderDXIL();
                psDesc.Category = render::ShaderBlobCategory::DXIL;
                psDesc.Reflection = psRefl;
                break;
            }
            case render::RenderBackend::Vulkan: {
                render::SpirvShaderDesc vsRefl{};
                vsRefl.PushConstants.push_back(render::SpirvPushConstantRange{
                    .Name = "gPush",
                    .Offset = 0,
                    .Size = 16,
                });
                render::SpirvShaderDesc psRefl{};
                psRefl.ResourceBindings.push_back(render::SpirvResourceBinding{
                    .Name = "gTexture",
                    .Kind = render::SpirvResourceKind::SeparateImage,
                    .Set = 1,
                    .Binding = 0,
                    .HlslRegister = 0,
                    .HlslSpace = 1,
                    .ArraySize = 1,
                    .ImageInfo = render::SpirvImageInfo{
                        .Dim = render::SpirvImageDim::Dim2D,
                    },
                    .ReadOnly = true});
                psRefl.ResourceBindings.push_back(render::SpirvResourceBinding{
                    .Name = "gSampler",
                    .Kind = render::SpirvResourceKind::SeparateSampler,
                    .Set = 1,
                    .Binding = 1,
                    .HlslRegister = 1,
                    .HlslSpace = 1,
                    .ArraySize = 1,
                    .ImageInfo = std::nullopt,
                    .ReadOnly = true});
                vsDesc.Source = GetImGuiVertexShaderSPIRV();
                vsDesc.Category = render::ShaderBlobCategory::SPIRV;
                vsDesc.Reflection = vsRefl;
                psDesc.Source = GetImGuiPixelShaderSPIRV();
                psDesc.Category = render::ShaderBlobCategory::SPIRV;
                psDesc.Reflection = psRefl;
                break;
            }
            default:
                RADRAY_ERR_LOG("ImGuiRenderer does not support render backend {}", device->GetBackend());
                return nullptr;
        }
        auto vsOpt = device->CreateShader(vsDesc);
        if (!vsOpt.HasValue()) {
            return nullptr;
        }
        vs = vsOpt.Release();
        auto psOpt = device->CreateShader(psDesc);
        if (!psOpt.HasValue()) {
            return nullptr;
        }
        ps = psOpt.Release();
    }
    unique_ptr<render::RootSignature> rs;
    {
        render::Shader* shaders[] = {vs.get(), ps.get()};
        render::SamplerDescriptor samplerDesc{};
        samplerDesc.AddressS = render::AddressMode::ClampToEdge;
        samplerDesc.AddressT = render::AddressMode::ClampToEdge;
        samplerDesc.AddressR = render::AddressMode::ClampToEdge;
        samplerDesc.MinFilter = render::FilterMode::Linear;
        samplerDesc.MagFilter = render::FilterMode::Linear;
        samplerDesc.MipmapFilter = render::FilterMode::Linear;
        samplerDesc.LodMin = 0.0f;
        samplerDesc.LodMax = std::numeric_limits<float>::max();
        samplerDesc.Compare = std::nullopt;
        samplerDesc.AnisotropyClamp = 1;
        const render::StaticSamplerDescriptor staticSamplers[] = {
            render::StaticSamplerDescriptor{
                .Name = "gSampler",
                .Set = render::DescriptorSetIndex{1},
                .Binding = 1,
                .Stages = render::ShaderStage::Pixel,
                .Desc = samplerDesc,
            },
        };
        render::RootSignatureDescriptor rsDesc{};
        rsDesc.Shaders = shaders;
        rsDesc.StaticSamplers = staticSamplers;
        auto rsOpt = device->CreateRootSignature(rsDesc);
        if (!rsOpt.HasValue()) {
            return nullptr;
        }
        rs = rsOpt.Release();
    }
    unique_ptr<render::GraphicsPipelineState> pso;
    {
        const render::VertexElement vertexElements[] = {
            render::VertexElement{
                .Offset = offsetof(ImDrawVert, pos),
                .Semantic = "POSITION",
                .SemanticIndex = 0,
                .Format = render::VertexFormat::FLOAT32X2,
                .Location = 0,
            },
            render::VertexElement{
                .Offset = offsetof(ImDrawVert, uv),
                .Semantic = "TEXCOORD",
                .SemanticIndex = 0,
                .Format = render::VertexFormat::FLOAT32X2,
                .Location = 1,
            },
            render::VertexElement{
                .Offset = offsetof(ImDrawVert, col),
                .Semantic = "COLOR",
                .SemanticIndex = 0,
                .Format = render::VertexFormat::UNORM8X4,
                .Location = 2,
            },
        };
        const render::VertexBufferLayout vertexLayouts[] = {
            render::VertexBufferLayout{
                .ArrayStride = sizeof(ImDrawVert),
                .StepMode = render::VertexStepMode::Vertex,
                .Elements = vertexElements,
            },
        };
        render::BlendState blend{};
        blend.Color.Src = render::BlendFactor::SrcAlpha;
        blend.Color.Dst = render::BlendFactor::OneMinusSrcAlpha;
        blend.Color.Op = render::BlendOperation::Add;
        blend.Alpha.Src = render::BlendFactor::One;
        blend.Alpha.Dst = render::BlendFactor::OneMinusSrcAlpha;
        blend.Alpha.Op = render::BlendOperation::Add;
        const render::ColorTargetState colorTargets[] = {
            render::ColorTargetState{
                .Format = backBufferFormat,
                .Blend = blend,
                .WriteMask = render::ColorWrite::All,
            },
        };
        render::PrimitiveState primitive{};
        primitive.Topology = render::PrimitiveTopology::TriangleList;
        primitive.FaceClockwise = render::FrontFace::CW;
        primitive.Cull = render::CullMode::None;
        primitive.Poly = render::PolygonMode::Fill;
        primitive.StripIndexFormat = std::nullopt;
        primitive.UnclippedDepth = false;
        primitive.Conservative = false;
        render::MultiSampleState multiSample{};
        multiSample.Count = 1;
        multiSample.Mask = 0xFFFFFFFF;
        multiSample.AlphaToCoverageEnable = false;
        render::GraphicsPipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = rs.get();
        psoDesc.VS = render::ShaderEntry{
            .Target = vs.get(),
            .EntryPoint = "VSMain",
        };
        psoDesc.PS = render::ShaderEntry{
            .Target = ps.get(),
            .EntryPoint = "PSMain",
        };
        psoDesc.VertexLayouts = vertexLayouts;
        psoDesc.Primitive = primitive;
        psoDesc.DepthStencil = std::nullopt;
        psoDesc.MultiSample = multiSample;
        psoDesc.ColorTargets = colorTargets;
        auto psoOpt = device->CreateGraphicsPipelineState(psoDesc);
        if (!psoOpt.HasValue()) {
            return nullptr;
        }
        pso = psoOpt.Release();
    }
    auto renderer = make_unique<ImGuiRenderer>(app, mainWnd);
    renderer->_vs = std::move(vs);
    renderer->_ps = std::move(ps);
    renderer->_rs = std::move(rs);
    renderer->_pso = std::move(pso);
    return renderer;
}

void ImGuiRenderer::PlatformCreateWindow(ImGuiViewport* viewport) {
}

void ImGuiRenderer::PlatformDestroyWindow(ImGuiViewport* viewport) {
}

void ImGuiRenderer::PlatformShowWindow(ImGuiViewport* viewport) {
    ImGuiSystem* system = GetImGuiSystem();
    AppWindow* window = GetViewportAppWindow(system, viewport);
    if (window != nullptr && window->_window != nullptr) {
        window->_window->Show();
    }
}

void ImGuiRenderer::PlatformSetWindowPos(ImGuiViewport* viewport, ImVec2 pos) {
    ImGuiSystem* system = GetImGuiSystem();
    AppWindow* window = GetViewportAppWindow(system, viewport);
    if (window != nullptr && window->_window != nullptr) {
        window->_window->SetPosition(static_cast<int32_t>(pos.x), static_cast<int32_t>(pos.y));
    }
}

ImVec2 ImGuiRenderer::PlatformGetWindowPos(ImGuiViewport* viewport) {
    ImGuiSystem* system = GetImGuiSystem();
    AppWindow* window = GetViewportAppWindow(system, viewport);
    if (window == nullptr || window->_window == nullptr) {
        return ImVec2{};
    }
    WindowVec2i pos = window->_window->GetPosition();
    return ImVec2(static_cast<float>(pos.X), static_cast<float>(pos.Y));
}

void ImGuiRenderer::PlatformSetWindowSize(ImGuiViewport* viewport, ImVec2 size) {
    ImGuiSystem* system = GetImGuiSystem();
    AppWindow* window = GetViewportAppWindow(system, viewport);
    if (window != nullptr && window->_window != nullptr) {
        window->_window->SetSize(ImGuiSizeToInt(size.x), ImGuiSizeToInt(size.y));
    }
}

ImVec2 ImGuiRenderer::PlatformGetWindowSize(ImGuiViewport* viewport) {
    ImGuiSystem* system = GetImGuiSystem();
    AppWindow* window = GetViewportAppWindow(system, viewport);
    if (window == nullptr || window->_window == nullptr) {
        return ImVec2{};
    }
    WindowVec2i size = window->_window->GetSize();
    return ImVec2(static_cast<float>(size.X), static_cast<float>(size.Y));
}

void ImGuiRenderer::PlatformSetWindowFocus(ImGuiViewport* viewport) {
    ImGuiSystem* system = GetImGuiSystem();
    AppWindow* window = GetViewportAppWindow(system, viewport);
    if (window != nullptr && window->_window != nullptr) {
        window->_window->Focus();
    }
}

bool ImGuiRenderer::PlatformGetWindowFocus(ImGuiViewport* viewport) {
    ImGuiSystem* system = GetImGuiSystem();
    AppWindow* window = GetViewportAppWindow(system, viewport);
    return window != nullptr && window->_window != nullptr && window->_window->IsFocused();
}

bool ImGuiRenderer::PlatformGetWindowMinimized(ImGuiViewport* viewport) {
    ImGuiSystem* system = GetImGuiSystem();
    AppWindow* window = GetViewportAppWindow(system, viewport);
    return window != nullptr && window->_window != nullptr && window->_window->IsMinimized();
}

void ImGuiRenderer::PlatformSetWindowTitle(ImGuiViewport* viewport, const char* title) {
    ImGuiSystem* system = GetImGuiSystem();
    AppWindow* window = GetViewportAppWindow(system, viewport);
    if (window != nullptr && window->_window != nullptr) {
        window->_window->SetTitle(title != nullptr ? std::string_view{title} : std::string_view{});
    }
}

void ImGuiRenderer::PlatformSetWindowAlpha(ImGuiViewport* viewport, float alpha) {
    ImGuiSystem* system = GetImGuiSystem();
    AppWindow* window = GetViewportAppWindow(system, viewport);
    if (window != nullptr && window->_window != nullptr) {
        window->_window->SetAlpha(alpha);
    }
}

float ImGuiRenderer::PlatformGetWindowDpiScale(ImGuiViewport* viewport) {
    ImGuiSystem* system = GetImGuiSystem();
    AppWindow* window = GetViewportAppWindow(system, viewport);
    return window != nullptr && window->_window != nullptr ? window->_window->GetDpiScale() : 1.0f;
}

void ImGuiRenderer::PlatformUpdateWindow(ImGuiViewport* viewport) {
    RADRAY_UNUSED(viewport);
}

void ImGuiRenderer::CreateWindow(ImGuiViewport* viewport) {
    RADRAY_ASSERT(viewport != nullptr);
    viewport->RendererUserData = viewport->PlatformUserData;
}

void ImGuiRenderer::DestroyWindow(ImGuiViewport* viewport) {
    if (viewport != nullptr) {
        viewport->RendererUserData = nullptr;
    }
}

void ImGuiRenderer::SetWindowSize(ImGuiViewport* viewport, ImVec2 size) {
    RADRAY_UNUSED(size);
    ImGuiSystem* system = GetImGuiSystem();
    AppWindow* window = GetViewportAppWindow(system, viewport);
    if (window != nullptr) {
        std::unique_lock<std::mutex> lock{window->_stateMutex, std::defer_lock};
        if (window->_app != nullptr && window->_app->_multiThreaded) {
            lock.lock();
        }
        window->_pendingRecreate = true;
    }
}

void ImGuiRenderer::RenderWindow(ImGuiViewport* viewport, void* renderArg) {
    RADRAY_UNUSED(viewport);
    RADRAY_UNUSED(renderArg);
}

void ImGuiRenderer::SwapBuffers(ImGuiViewport* viewport, void* renderArg) {
    RADRAY_UNUSED(viewport);
    RADRAY_UNUSED(renderArg);
}

static void InstallPlatformWindowBackend(ImGuiSystem* system) {
    RADRAY_ASSERT(system != nullptr);
    RADRAY_ASSERT(system->_mainWnd != nullptr);
    RADRAY_ASSERT(system->_mainWnd->_window != nullptr);

    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport->PlatformUserData != nullptr && platformIo.Platform_DestroyWindow != nullptr) {
        platformIo.Platform_DestroyWindow(mainViewport);
    }

    WindowNativeHandler nativeHandle = system->_mainWnd->_window->GetNativeHandler();
    mainViewport->PlatformHandle = nativeHandle.Handle;
    mainViewport->PlatformHandleRaw = nativeHandle.Handle;

    platformIo.Platform_CreateWindow = ImGuiRenderer::PlatformCreateWindow;
    platformIo.Platform_DestroyWindow = ImGuiRenderer::PlatformDestroyWindow;
    platformIo.Platform_ShowWindow = ImGuiRenderer::PlatformShowWindow;
    platformIo.Platform_SetWindowPos = ImGuiRenderer::PlatformSetWindowPos;
    platformIo.Platform_GetWindowPos = ImGuiRenderer::PlatformGetWindowPos;
    platformIo.Platform_SetWindowSize = ImGuiRenderer::PlatformSetWindowSize;
    platformIo.Platform_GetWindowSize = ImGuiRenderer::PlatformGetWindowSize;
    platformIo.Platform_SetWindowFocus = ImGuiRenderer::PlatformSetWindowFocus;
    platformIo.Platform_GetWindowFocus = ImGuiRenderer::PlatformGetWindowFocus;
    platformIo.Platform_GetWindowMinimized = ImGuiRenderer::PlatformGetWindowMinimized;
    platformIo.Platform_SetWindowTitle = ImGuiRenderer::PlatformSetWindowTitle;
    platformIo.Platform_SetWindowAlpha = ImGuiRenderer::PlatformSetWindowAlpha;
    platformIo.Platform_GetWindowDpiScale = ImGuiRenderer::PlatformGetWindowDpiScale;
    platformIo.Platform_UpdateWindow = ImGuiRenderer::PlatformUpdateWindow;
}

ImGuiSystem::ImGuiSystem(
    Application* app,
    AppWindow* mainWnd,
    unique_ptr<ImGuiContextRAII> context)
    : _app(app),
      _mainWnd(mainWnd),
      _context(std::move(context)) {}

ImGuiSystem::~ImGuiSystem() noexcept {
    if (_context != nullptr && _context->IsValid()) {
        _context->SetCurrent();
        ImGui::DestroyPlatformWindows();
        ShutdownImGuiInternal(_context->Get());
    }
    if (_renderer != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.BackendRendererUserData == _renderer.get()) {
            io.BackendRendererUserData = nullptr;
            io.BackendRendererName = nullptr;
            io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
            io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
            io.BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;
        }
        _renderer->_system = nullptr;
    }
    _viewportPlatformData.clear();
}

Nullable<unique_ptr<ImGuiSystem>> ImGuiSystem::Create(const ImGuiSystemDescriptor& desc) {
    RADRAY_ASSERT(IMGUI_CHECKVERSION());
    RADRAY_ASSERT(desc.App != nullptr);
    RADRAY_ASSERT(desc.MainWnd != nullptr);
    RADRAY_ASSERT(desc.MainWnd->_app == desc.App);
    RADRAY_ASSERT(desc.App->_gpu != nullptr);
    RADRAY_ASSERT(desc.MainWnd->_window != nullptr);
    RADRAY_ASSERT(desc.MainWnd->_surface != nullptr);
    RADRAY_ASSERT(desc.MainWnd->_surface->IsValid());

    GpuSurface* surface = desc.MainWnd->_surface.get();
    NativeWindow* mainWindow = desc.MainWnd->_window.get();
    const render::SwapChainDescriptor surfaceDesc = surface->GetDesc();
    RADRAY_ASSERT(surfaceDesc.BackBufferCount != 0);
    RADRAY_ASSERT(surfaceDesc.FlightFrameCount != 0);

    auto context = make_unique<ImGuiContextRAII>();
    context->SetCurrent();
    unique_ptr<ImGuiRenderer> renderer;
    {
        auto rendererOpt = ImGuiRenderer::Create(desc.App, desc.MainWnd);
        if (!rendererOpt.HasValue()) {
            return nullptr;
        }
        renderer = rendererOpt.Release();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.Fonts->AddFontDefault();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    if (!InitImGuiInternal(context->Get(), mainWindow)) {
        return nullptr;
    }

    auto system = make_unique<ImGuiSystem>(desc.App, desc.MainWnd, std::move(context));
    system->_renderer = std::move(renderer);
    system->_renderer->_system = system.get();

    io.BackendRendererUserData = system->_renderer.get();
    io.BackendRendererName = "radray_imgui";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    if (desc.EnableViewports) {
        InstallPlatformWindowBackend(system.get());
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
        ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        platformIo.Renderer_CreateWindow = ImGuiRenderer::CreateWindow;
        platformIo.Renderer_DestroyWindow = ImGuiRenderer::DestroyWindow;
        platformIo.Renderer_SetWindowSize = ImGuiRenderer::SetWindowSize;
        platformIo.Renderer_RenderWindow = ImGuiRenderer::RenderWindow;
        platformIo.Renderer_SwapBuffers = ImGuiRenderer::SwapBuffers;
    }
    return system;
}

}  // namespace radray
