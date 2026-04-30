#ifdef RADRAY_ENABLE_IMGUI

#include <radray/runtime/imgui_system.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <imgui_internal.h>

#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/utility.h>

namespace radray {

extern bool InitImGuiInternal(ImGuiContext* ctx, NativeWindow* window);
extern void ShutdownImGuiInternal(ImGuiContext* ctx);
extern void NewFrameImGuiInternal(ImGuiContext* ctx);

static int32_t ImGuiSizeToInt(float v) noexcept {
    return static_cast<int32_t>(std::max(v, 1.0f));
}

static void UsePreparedResource(AppWindow* window, uint32_t mailboxSlot, GpuResourceHandle handle) {
    if (handle.IsValid()) {
        window->UsePreparedResource(mailboxSlot, handle);
    }
}

static void UsePreparedTextureBinding(AppWindow* window, uint32_t mailboxSlot, ImGuiTextureBinding* binding) {
    if (binding == nullptr) {
        return;
    }
    UsePreparedResource(window, mailboxSlot, binding->DescriptorSet);
    UsePreparedResource(window, mailboxSlot, binding->View);
    UsePreparedResource(window, mailboxSlot, binding->Texture);
}

void ImGuiDrawListSnapshot::Clear() noexcept {
    Vertices.clear();
    Indices.clear();
    Commands.clear();
}

void ImGuiRenderSnapshot::Clear() noexcept {
    Valid = false;
    DisplayPos = ImVec2{};
    DisplaySize = ImVec2{};
    FramebufferScale = ImVec2{};
    TotalIdxCount = 0;
    TotalVtxCount = 0;
    DrawLists.clear();
    TextureUploads.clear();
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
    if (_system != nullptr) {
        _system->DestroyTextureBindings();
    } else {
        RADRAY_ASSERT(_textureBindings.empty());
    }
    _textureBindings.clear();
    if (_app != nullptr && _app->_gpu != nullptr) {
        GpuRuntime* gpu = _app->_gpu.get();
        auto destroyHandle = [gpu](auto& handle) noexcept {
            if (handle.IsValid()) {
                gpu->DestroyResource(handle);
                handle.Invalidate();
            }
        };
        destroyHandle(_pso);
        destroyHandle(_rs);
        destroyHandle(_ps);
        destroyHandle(_vs);
    }
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
    GpuRuntime* gpu = app->_gpu.get();
    RADRAY_ASSERT(gpu != nullptr);
    GpuShaderHandle vs{};
    GpuShaderHandle ps{};
    GpuRootSignatureHandle rs{};
    GpuGraphicsPipelineStateHandle pso{};
    auto destroyHandle = [gpu](auto& handle) noexcept {
        if (handle.IsValid()) {
            gpu->DestroyResource(handle);
            handle.Invalidate();
        }
    };
    try {
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
            vs = gpu->CreateShader(vsDesc);
            ps = gpu->CreateShader(psDesc);
        }
        {
            GpuShaderHandle shaders[] = {vs, ps};
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
            GpuRootSignatureDescriptor rsDesc{};
            rsDesc.Shaders = shaders;
            rsDesc.StaticSamplers = staticSamplers;
            rs = gpu->CreateRootSignature(rsDesc);
        }
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
            GpuGraphicsPipelineStateDescriptor psoDesc{};
            psoDesc.RootSig = rs;
            psoDesc.VS = GpuShaderEntry{
                .Target = vs,
                .EntryPoint = "VSMain",
            };
            psoDesc.PS = GpuShaderEntry{
                .Target = ps,
                .EntryPoint = "PSMain",
            };
            psoDesc.VertexLayouts = vertexLayouts;
            psoDesc.Primitive = primitive;
            psoDesc.DepthStencil = std::nullopt;
            psoDesc.MultiSample = multiSample;
            psoDesc.ColorTargets = colorTargets;
            pso = gpu->CreateGraphicsPipelineState(psoDesc);
        }

        auto renderer = make_unique<ImGuiRenderer>(app, mainWnd);
        renderer->_vs = vs;
        renderer->_ps = ps;
        renderer->_rs = rs;
        renderer->_pso = pso;
        return renderer;
    } catch (const std::exception& ex) {
        RADRAY_ERR_LOG("ImGuiRenderer GPU resource creation failed: {}", ex.what());
        destroyHandle(pso);
        destroyHandle(rs);
        destroyHandle(ps);
        destroyHandle(vs);
        return nullptr;
    } catch (...) {
        RADRAY_ERR_LOG("ImGuiRenderer GPU resource creation failed");
        destroyHandle(pso);
        destroyHandle(rs);
        destroyHandle(ps);
        destroyHandle(vs);
        return nullptr;
    }
}

void ImGuiRenderer::CreateWindow(ImGuiViewport* viewport) {
    RADRAY_ASSERT(viewport != nullptr);
    RADRAY_ASSERT(viewport->RendererUserData == nullptr);
    RADRAY_ASSERT(viewport->PlatformHandleRaw != nullptr);
    ImGuiIO& io = ImGui::GetIO();
    RADRAY_ASSERT(io.BackendRendererUserData != nullptr);
    ImGuiRenderer* renderer = static_cast<ImGuiRenderer*>(io.BackendRendererUserData);
    ImGuiSystem* system = renderer->_system;

    WindowNativeHandler nativeHandler{};
#ifdef RADRAY_PLATFORM_WINDOWS
    nativeHandler.Type = WindowHandlerTag::HWND;
    nativeHandler.Handle = viewport->PlatformHandleRaw;
#else
    RADRAY_ERR_LOG("ImGui viewport borrowed surfaces are not supported on this platform");
    return;
#endif

    RADRAY_ASSERT(renderer->_mainWnd != nullptr);
    GpuSurface* mainSurface = renderer->_mainWnd->_surface.get();
    RADRAY_ASSERT(mainSurface->IsValid());
    const render::SwapChainDescriptor mainDesc = mainSurface->GetDesc();
    const WindowVec2i size{
        ImGuiSizeToInt(viewport->Size.x),
        ImGuiSizeToInt(viewport->Size.y),
    };

    GpuSurfaceDescriptor surfaceDesc{};
    surfaceDesc.NativeHandler = nativeHandler.Handle;
    surfaceDesc.Width = static_cast<uint32_t>(size.X);
    surfaceDesc.Height = static_cast<uint32_t>(size.Y);
    surfaceDesc.BackBufferCount = mainDesc.BackBufferCount;
    surfaceDesc.FlightFrameCount = mainDesc.FlightFrameCount;
    surfaceDesc.Format = mainDesc.Format;
    surfaceDesc.PresentMode = mainDesc.PresentMode;
    surfaceDesc.QueueSlot = mainSurface->GetQueueSlot();

    AppWindow* window = renderer->_app->CreateBorrowedSurfaceWindow(nativeHandler, size, surfaceDesc, 1);
    window->_borrowedMinimized = (viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0;

    auto data = make_unique<ImGuiViewportRendererData>();
    data->Viewport = viewport;
    data->Window = window;
    data->Mailboxes.resize(window->_mailboxes.size());
    data->UploadedMailboxes.resize(window->_mailboxes.size());
    ImGuiViewportRendererData* rawData = data.get();
    system->_viewportRendererData.push_back(std::move(data));
    viewport->RendererUserData = rawData;
}

void ImGuiRenderer::DestroyWindow(ImGuiViewport* viewport) {
    RADRAY_ASSERT(viewport != nullptr);
    RADRAY_ASSERT(viewport->RendererUserData != nullptr);
    ImGuiIO& io = ImGui::GetIO();
    RADRAY_ASSERT(io.BackendRendererUserData != nullptr);
    ImGuiRenderer* renderer = static_cast<ImGuiRenderer*>(io.BackendRendererUserData);
    ImGuiSystem* system = renderer->_system;
    RADRAY_ASSERT(system != nullptr);
    auto* data = static_cast<ImGuiViewportRendererData*>(viewport->RendererUserData);
    RADRAY_ASSERT(data != nullptr);
    if (data->Window != renderer->_mainWnd) {
        system->_app->DestroyWindow(data->Window);
    }
    std::erase_if(system->_viewportRendererData, [data](const unique_ptr<ImGuiViewportRendererData>& item) {
        return item.get() == data;
    });
    viewport->RendererUserData = nullptr;
}

void ImGuiRenderer::SetWindowSize(ImGuiViewport* viewport, ImVec2 size) {
    RADRAY_ASSERT(viewport->RendererUserData != nullptr);
    auto* data = static_cast<ImGuiViewportRendererData*>(viewport->RendererUserData);
    AppWindow* window = data->Window;
    std::unique_lock<std::mutex> lock{window->_stateMutex, std::defer_lock};
    if (window->_app != nullptr && window->_app->_multiThreaded) {
        lock.lock();
    }
    window->_borrowedSize = WindowVec2i{ImGuiSizeToInt(size.x), ImGuiSizeToInt(size.y)};
    window->_borrowedMinimized = (viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0;
    window->_pendingRecreate = true;
}

void ImGuiRenderer::RenderWindow(ImGuiViewport* viewport, void* renderArg) {
    RADRAY_UNUSED(viewport);
    RADRAY_UNUSED(renderArg);
}

void ImGuiRenderer::SwapBuffers(ImGuiViewport* viewport, void* renderArg) {
    RADRAY_UNUSED(viewport);
    RADRAY_UNUSED(renderArg);
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
    }
    if (_renderer != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.BackendRendererUserData == _renderer.get()) {
            ImGui::DestroyPlatformWindows();
            ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
            platformIo.Renderer_CreateWindow = nullptr;
            platformIo.Renderer_DestroyWindow = nullptr;
            platformIo.Renderer_SetWindowSize = nullptr;
            platformIo.Renderer_RenderWindow = nullptr;
            platformIo.Renderer_SwapBuffers = nullptr;
            io.BackendRendererUserData = nullptr;
            io.BackendRendererName = nullptr;
            io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
            io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
            io.BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;
        }
        this->DestroyTextureBindings();
        _renderer->_system = nullptr;
    }
    if (_context != nullptr && _context->IsValid()) {
        ShutdownImGuiInternal(_context->Get());
    }
    _viewportRendererData.clear();
}

void ImGuiSystem::NewFrame() {
    RADRAY_ASSERT(_context != nullptr);
    RADRAY_ASSERT(_context->IsValid());

    _context->SetCurrent();
    NewFrameImGuiInternal(_context->Get());
    ImGui::NewFrame();
}

void ImGuiSystem::DestroyTextureBinding(ImGuiTextureBinding* binding) noexcept {
    RADRAY_ASSERT(binding != nullptr);
    RADRAY_ASSERT(_app != nullptr);
    RADRAY_ASSERT(_app->_gpu != nullptr);
    GpuRuntime* gpu = _app->_gpu.get();
    if (binding->DescriptorSet.IsValid()) {
        gpu->DestroyResource(binding->DescriptorSet);
        binding->DescriptorSet.Invalidate();
    }
    if (binding->View.IsValid()) {
        gpu->DestroyResource(binding->View);
        binding->View.Invalidate();
    }
    if (binding->Texture.IsValid()) {
        gpu->DestroyResource(binding->Texture);
        binding->Texture.Invalidate();
    }
    binding->State = render::TextureState::Undefined;
    binding->PendingDestroy = false;
}

void ImGuiSystem::DestroyTextureBindings() noexcept {
    RADRAY_ASSERT(_renderer != nullptr);
    for (const unique_ptr<ImGuiTextureBinding>& binding : _renderer->_textureBindings) {
        this->DestroyTextureBinding(binding.get());
    }
    _renderer->_textureBindings.clear();
}

bool ImGuiSystem::HasPendingTextureBindingReferences(ImGuiTextureBinding* binding) const noexcept {
    if (binding == nullptr) {
        return false;
    }

    for (const unique_ptr<ImGuiViewportRendererData>& data : _viewportRendererData) {
        if (data == nullptr) {
            continue;
        }
        for (const ImGuiRenderSnapshot& snapshot : data->Mailboxes) {
            for (const ImGuiTextureUploadSnapshot& upload : snapshot.TextureUploads) {
                if (upload.Binding == binding) {
                    return true;
                }
            }
            for (const ImGuiDrawListSnapshot& drawList : snapshot.DrawLists) {
                for (const ImGuiRenderCommandSnapshot& cmd : drawList.Commands) {
                    if (cmd.TexID == reinterpret_cast<ImTextureID>(binding)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void ImGuiSystem::ProcessPendingTextureDestroys() noexcept {
    if (_renderer == nullptr) {
        return;
    }

    std::erase_if(_renderer->_textureBindings, [this](const unique_ptr<ImGuiTextureBinding>& item) {
        ImGuiTextureBinding* binding = item.get();
        if (binding == nullptr || !binding->PendingDestroy || this->HasPendingTextureBindingReferences(binding)) {
            return false;
        }
        this->DestroyTextureBinding(binding);
        return true;
    });
}

void ImGuiSystem::PrepareRenderData(AppWindow* window, uint32_t mailboxSlot) {
    RADRAY_ASSERT(_app != nullptr);
    RADRAY_ASSERT(_context != nullptr);
    RADRAY_ASSERT(_context->IsValid());
    RADRAY_ASSERT(window != nullptr);
    RADRAY_ASSERT(window->_app == _app);
    RADRAY_ASSERT(mailboxSlot < window->_mailboxes.size());

    _context->SetCurrent();
    ImGui::Render();

    auto dataIt = std::find_if(
        _viewportRendererData.begin(),
        _viewportRendererData.end(),
        [window](const unique_ptr<ImGuiViewportRendererData>& data) {
            RADRAY_ASSERT(data != nullptr);
            return data->Window == window;
        });
    RADRAY_ASSERT(dataIt != _viewportRendererData.end());
    ImGuiViewportRendererData* data = dataIt->get();
    RADRAY_ASSERT(data->Viewport != nullptr);
    RADRAY_ASSERT(data->Window == window);

    data->Mailboxes.resize(window->_mailboxes.size());
    data->UploadedMailboxes.resize(window->_mailboxes.size());
    RADRAY_ASSERT(mailboxSlot < data->Mailboxes.size());
    RADRAY_ASSERT(mailboxSlot < data->UploadedMailboxes.size());

    ImDrawData* drawData = data->Viewport->DrawData;
    RADRAY_ASSERT(drawData != nullptr);
    RADRAY_ASSERT(drawData->Valid);

    ImGuiRenderSnapshot& snapshot = data->Mailboxes[mailboxSlot];
    snapshot.Clear();
    data->UploadedMailboxes[mailboxSlot].Clear();
    if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        this->ProcessPendingTextureDestroys();
        return;
    }

    snapshot.Valid = true;
    snapshot.DisplayPos = drawData->DisplayPos;
    snapshot.DisplaySize = drawData->DisplaySize;
    snapshot.FramebufferScale = drawData->FramebufferScale;
    snapshot.TotalIdxCount = drawData->TotalIdxCount;
    snapshot.TotalVtxCount = drawData->TotalVtxCount;
    RADRAY_ASSERT(drawData->CmdListsCount == drawData->CmdLists.Size);
    snapshot.DrawLists.reserve(static_cast<size_t>(drawData->CmdListsCount));
    RADRAY_ASSERT(_renderer != nullptr);
    if (snapshot.TotalVtxCount > 0 && snapshot.TotalIdxCount > 0) {
        UsePreparedResource(window, mailboxSlot, _renderer->_pso);
        UsePreparedResource(window, mailboxSlot, _renderer->_rs);
    }

    RADRAY_ASSERT(_app != nullptr);
    RADRAY_ASSERT(_app->_gpu != nullptr);
    render::Device* device = _app->_gpu->GetDevice();
    RADRAY_ASSERT(device != nullptr);
    const uint64_t textureRowAlignment = std::max<uint64_t>(1, device->GetDetail().TextureDataPitchAlignment);
    if (drawData->Textures != nullptr) {
        for (ImTextureData* tex : *drawData->Textures) {
            RADRAY_ASSERT(tex != nullptr);
            if (tex->Status == ImTextureStatus_OK || tex->Status == ImTextureStatus_Destroyed) {
                continue;
            }
            if (tex->Status == ImTextureStatus_WantDestroy) {
                auto* binding = static_cast<ImGuiTextureBinding*>(tex->BackendUserData);
                if (binding != nullptr) {
                    binding->PendingDestroy = true;
                    tex->BackendUserData = nullptr;
                    tex->SetTexID(ImTextureID_Invalid);
                }
                tex->SetStatus(ImTextureStatus_Destroyed);
                continue;
            }

            RADRAY_ASSERT(tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates);
            RADRAY_ASSERT(tex->Width > 0);
            RADRAY_ASSERT(tex->Height > 0);
            RADRAY_ASSERT(tex->Pixels != nullptr);
            RADRAY_ASSERT(tex->Format == ImTextureFormat_RGBA32 || tex->Format == ImTextureFormat_Alpha8);

            auto* binding = static_cast<ImGuiTextureBinding*>(tex->BackendUserData);
            const bool isCreate = tex->Status == ImTextureStatus_WantCreate;
            if (isCreate) {
                RADRAY_ASSERT(binding == nullptr);
                auto ownedBinding = make_unique<ImGuiTextureBinding>();
                try {
                    render::TextureDescriptor textureDesc{};
                    textureDesc.Dim = render::TextureDimension::Dim2D;
                    textureDesc.Width = static_cast<uint32_t>(tex->Width);
                    textureDesc.Height = static_cast<uint32_t>(tex->Height);
                    textureDesc.DepthOrArraySize = 1;
                    textureDesc.MipLevels = 1;
                    textureDesc.SampleCount = 1;
                    textureDesc.Format = render::TextureFormat::RGBA8_UNORM;
                    textureDesc.Memory = render::MemoryType::Device;
                    textureDesc.Usage = render::TextureUse::CopyDestination | render::TextureUse::Resource;
                    ownedBinding->Texture = _app->_gpu->CreateTexture(textureDesc);
                    RADRAY_ASSERT(ownedBinding->Texture.IsValid());
                    auto* texture = static_cast<render::Texture*>(ownedBinding->Texture.NativeHandle);
                    RADRAY_ASSERT(texture != nullptr);

                    GpuTextureViewDescriptor viewDesc{};
                    viewDesc.Target = ownedBinding->Texture;
                    viewDesc.Dim = render::TextureDimension::Dim2D;
                    viewDesc.Format = render::TextureFormat::RGBA8_UNORM;
                    viewDesc.Range = render::SubresourceRange{0, 1, 0, 1};
                    viewDesc.Usage = render::TextureViewUsage::Resource;
                    ownedBinding->View = _app->_gpu->CreateTextureView(viewDesc);
                    RADRAY_ASSERT(ownedBinding->View.IsValid());
                    auto* view = static_cast<render::TextureView*>(ownedBinding->View.NativeHandle);
                    RADRAY_ASSERT(view != nullptr);

                    ownedBinding->DescriptorSet = _app->_gpu->CreateDescriptorSet(_renderer->_rs, render::DescriptorSetIndex{1});
                    RADRAY_ASSERT(ownedBinding->DescriptorSet.IsValid());
                    auto* descriptorSet = static_cast<render::DescriptorSet*>(ownedBinding->DescriptorSet.NativeHandle);
                    RADRAY_ASSERT(descriptorSet != nullptr);
                    RADRAY_ASSERT(descriptorSet->WriteResource("gTexture", view));
                    ownedBinding->State = render::TextureState::Undefined;
                } catch (...) {
                    this->DestroyTextureBinding(ownedBinding.get());
                    throw;
                }

                binding = ownedBinding.get();
                _renderer->_textureBindings.push_back(std::move(ownedBinding));
                tex->BackendUserData = binding;
                tex->SetTexID(reinterpret_cast<ImTextureID>(binding));
            } else {
                RADRAY_ASSERT(binding != nullptr);
                RADRAY_ASSERT(binding->Texture.IsValid());
                RADRAY_ASSERT(binding->Texture.NativeHandle != nullptr);
                RADRAY_ASSERT(binding->View.IsValid());
                RADRAY_ASSERT(binding->View.NativeHandle != nullptr);
                RADRAY_ASSERT(binding->DescriptorSet.IsValid());
                RADRAY_ASSERT(binding->DescriptorSet.NativeHandle != nullptr);
                RADRAY_ASSERT(!binding->PendingDestroy);
            }

            const uint32_t width = static_cast<uint32_t>(tex->Width);
            const uint32_t height = static_cast<uint32_t>(tex->Height);
            const uint64_t tightRowPitch = static_cast<uint64_t>(width) * 4;
            const uint64_t uploadRowPitch = Align(tightRowPitch, textureRowAlignment);
            ImGuiTextureUploadSnapshot& upload = snapshot.TextureUploads.emplace_back();
            upload.Binding = binding;
            UsePreparedResource(window, mailboxSlot, upload.Binding->Texture);
            upload.Width = width;
            upload.Height = height;
            upload.RowPitch = uploadRowPitch;
            upload.IsCreate = isCreate;
            upload.Pixels.resize(static_cast<size_t>(uploadRowPitch * height));
            auto* dstPixels = reinterpret_cast<uint8_t*>(upload.Pixels.data());
            if (tex->Format == ImTextureFormat_RGBA32) {
                const uint64_t srcRowPitch = static_cast<uint64_t>(tex->GetPitch());
                for (uint32_t y = 0; y < height; ++y) {
                    std::memcpy(dstPixels + uploadRowPitch * y, tex->Pixels + srcRowPitch * y, tightRowPitch);
                }
            } else {
                RADRAY_ASSERT(tex->Format == ImTextureFormat_Alpha8);
                const uint64_t srcRowPitch = static_cast<uint64_t>(tex->GetPitch());
                for (uint32_t y = 0; y < height; ++y) {
                    const auto* srcRow = tex->Pixels + srcRowPitch * y;
                    auto* dstRow = dstPixels + uploadRowPitch * y;
                    for (uint32_t x = 0; x < width; ++x) {
                        dstRow[x * 4 + 0] = 255;
                        dstRow[x * 4 + 1] = 255;
                        dstRow[x * 4 + 2] = 255;
                        dstRow[x * 4 + 3] = srcRow[x];
                    }
                }
            }
            tex->SetStatus(ImTextureStatus_OK);
        }
    }

    for (ImDrawList* srcDrawList : drawData->CmdLists) {
        RADRAY_ASSERT(srcDrawList != nullptr);
        ImGuiDrawListSnapshot& dstDrawList = snapshot.DrawLists.emplace_back();

        RADRAY_ASSERT(srcDrawList->VtxBuffer.Size >= 0);
        dstDrawList.Vertices.resize(static_cast<size_t>(srcDrawList->VtxBuffer.Size));
        if (srcDrawList->VtxBuffer.Size > 0) {
            RADRAY_ASSERT(srcDrawList->VtxBuffer.Data != nullptr);
            std::copy_n(srcDrawList->VtxBuffer.Data, srcDrawList->VtxBuffer.Size, dstDrawList.Vertices.data());
        }

        RADRAY_ASSERT(srcDrawList->IdxBuffer.Size >= 0);
        dstDrawList.Indices.resize(static_cast<size_t>(srcDrawList->IdxBuffer.Size));
        if (srcDrawList->IdxBuffer.Size > 0) {
            RADRAY_ASSERT(srcDrawList->IdxBuffer.Data != nullptr);
            std::copy_n(srcDrawList->IdxBuffer.Data, srcDrawList->IdxBuffer.Size, dstDrawList.Indices.data());
        }

        dstDrawList.Commands.reserve(static_cast<size_t>(srcDrawList->CmdBuffer.Size));
        for (const ImDrawCmd& srcCmd : srcDrawList->CmdBuffer) {
            ImGuiRenderCommandSnapshot& dstCmd = dstDrawList.Commands.emplace_back();
            dstCmd.ClipRect = srcCmd.ClipRect;
            dstCmd.TexID = srcCmd.GetTexID();
            dstCmd.ElemCount = srcCmd.ElemCount;
            dstCmd.IdxOffset = srcCmd.IdxOffset;
            dstCmd.VtxOffset = srcCmd.VtxOffset;

            if (srcCmd.UserCallback == ImDrawCallback_ResetRenderState) {
                dstCmd.Kind = ImGuiRenderCommandKind::ResetRenderState;
            } else {
                RADRAY_ASSERT(srcCmd.UserCallback == nullptr);
                dstCmd.Kind = ImGuiRenderCommandKind::DrawIndexed;
                if (srcCmd.ElemCount > 0) {
                    const ImTextureID texId = srcCmd.GetTexID();
                    RADRAY_ASSERT(texId != ImTextureID_Invalid);
                    auto* binding = reinterpret_cast<ImGuiTextureBinding*>(texId);
                    RADRAY_ASSERT(binding != nullptr);
                    UsePreparedTextureBinding(window, mailboxSlot, binding);
                }
            }
        }
    }
    this->ProcessPendingTextureDestroys();
}

void ImGuiSystem::Upload(AppWindow* window, uint32_t mailboxSlot, GpuAsyncContext* context, render::CommandBuffer* cmd) {
    RADRAY_ASSERT(_app != nullptr);
    RADRAY_ASSERT(_renderer != nullptr);
    RADRAY_ASSERT(window != nullptr);
    RADRAY_ASSERT(window->_app == _app);
    RADRAY_ASSERT(mailboxSlot < window->_mailboxes.size());
    RADRAY_ASSERT(context != nullptr);
    RADRAY_ASSERT(cmd != nullptr);
    RADRAY_ASSERT(_app->_gpu != nullptr);
    render::Device* device = _app->_gpu->GetDevice();
    RADRAY_ASSERT(device != nullptr);

    auto dataIt = std::find_if(
        _viewportRendererData.begin(),
        _viewportRendererData.end(),
        [window](const unique_ptr<ImGuiViewportRendererData>& data) {
            RADRAY_ASSERT(data != nullptr);
            return data->Window == window;
        });
    RADRAY_ASSERT(dataIt != _viewportRendererData.end());
    ImGuiViewportRendererData* data = dataIt->get();
    RADRAY_ASSERT(data != nullptr);
    data->Mailboxes.resize(window->_mailboxes.size());
    data->UploadedMailboxes.resize(window->_mailboxes.size());
    RADRAY_ASSERT(mailboxSlot < data->Mailboxes.size());
    RADRAY_ASSERT(mailboxSlot < data->UploadedMailboxes.size());

    const ImGuiRenderSnapshot& snapshot = data->Mailboxes[mailboxSlot];
    ImGuiUploadedRenderData& uploaded = data->UploadedMailboxes[mailboxSlot];
    uploaded.Clear();
    if (!snapshot.Valid || snapshot.TotalVtxCount <= 0 || snapshot.TotalIdxCount <= 0) {
        return;
    }

    auto writeBuffer = [](render::Buffer* buffer, std::span<const byte> bytes) {
        RADRAY_ASSERT(buffer != nullptr);
        RADRAY_ASSERT(!bytes.empty());
        void* mapped = buffer->Map(0, bytes.size_bytes());
        RADRAY_ASSERT(mapped != nullptr);
        std::memcpy(mapped, bytes.data(), bytes.size_bytes());
        buffer->Unmap(0, bytes.size_bytes());
    };

    const bool needsHostWriteBarrier = device->GetBackend() == render::RenderBackend::Vulkan;
    for (const ImGuiTextureUploadSnapshot& upload : snapshot.TextureUploads) {
        RADRAY_ASSERT(upload.Binding != nullptr);
        RADRAY_ASSERT(upload.Binding->Texture.IsValid());
        auto* texture = static_cast<render::Texture*>(upload.Binding->Texture.NativeHandle);
        RADRAY_ASSERT(texture != nullptr);
        RADRAY_ASSERT(upload.Width > 0);
        RADRAY_ASSERT(upload.Height > 0);
        RADRAY_ASSERT(upload.RowPitch >= static_cast<uint64_t>(upload.Width) * 4);
        RADRAY_ASSERT(!upload.Pixels.empty());

        render::BufferDescriptor uploadDesc{};
        uploadDesc.Size = upload.Pixels.size();
        uploadDesc.Memory = render::MemoryType::Upload;
        uploadDesc.Usage = render::BufferUse::MapWrite | render::BufferUse::CopySource;
        GpuBufferHandle uploadBufferHandle = context->CreateTransientBuffer(uploadDesc);
        RADRAY_ASSERT(uploadBufferHandle.IsValid());
        auto* uploadBuffer = static_cast<render::Buffer*>(uploadBufferHandle.NativeHandle);
        RADRAY_ASSERT(uploadBuffer != nullptr);
        writeBuffer(uploadBuffer, std::span<const byte>{upload.Pixels.data(), upload.Pixels.size()});

        vector<render::ResourceBarrierDescriptor> barriers;
        if (needsHostWriteBarrier) {
            barriers.emplace_back(render::BarrierBufferDescriptor{
                .Target = uploadBuffer,
                .Before = render::BufferState::HostWrite,
                .After = render::BufferState::CopySource,
            });
        }
        barriers.emplace_back(render::BarrierTextureDescriptor{
            .Target = texture,
            .Before = upload.Binding->State,
            .After = render::TextureState::CopyDestination,
        });
        cmd->ResourceBarrier(barriers);
        cmd->CopyBufferToTexture(texture, render::SubresourceRange{0, 1, 0, 1}, uploadBuffer, 0);
        render::ResourceBarrierDescriptor afterCopy = render::BarrierTextureDescriptor{
            .Target = texture,
            .Before = render::TextureState::CopyDestination,
            .After = render::TextureState::ShaderRead,
        };
        cmd->ResourceBarrier(std::span{&afterCopy, 1});
        upload.Binding->State = render::TextureState::ShaderRead;
    }

    uploaded.DrawLists.reserve(snapshot.DrawLists.size());
    for (const ImGuiDrawListSnapshot& drawList : snapshot.DrawLists) {
        ImGuiUploadedDrawList& uploadedDrawList = uploaded.DrawLists.emplace_back();
        if (drawList.Vertices.empty() || drawList.Indices.empty()) {
            continue;
        }

        const auto vertexBytes = std::as_bytes(std::span<const ImDrawVert>{drawList.Vertices.data(), drawList.Vertices.size()});
        const auto indexBytes = std::as_bytes(std::span<const ImDrawIdx>{drawList.Indices.data(), drawList.Indices.size()});
        RADRAY_ASSERT(!vertexBytes.empty());
        RADRAY_ASSERT(!indexBytes.empty());

        render::BufferDescriptor vertexUploadDesc{};
        vertexUploadDesc.Size = vertexBytes.size_bytes();
        vertexUploadDesc.Memory = render::MemoryType::Upload;
        vertexUploadDesc.Usage = render::BufferUse::MapWrite | render::BufferUse::CopySource;
        GpuBufferHandle vertexUploadHandle = context->CreateTransientBuffer(vertexUploadDesc);
        RADRAY_ASSERT(vertexUploadHandle.IsValid());
        auto* vertexUpload = static_cast<render::Buffer*>(vertexUploadHandle.NativeHandle);
        RADRAY_ASSERT(vertexUpload != nullptr);
        writeBuffer(vertexUpload, vertexBytes);

        render::BufferDescriptor indexUploadDesc{};
        indexUploadDesc.Size = indexBytes.size_bytes();
        indexUploadDesc.Memory = render::MemoryType::Upload;
        indexUploadDesc.Usage = render::BufferUse::MapWrite | render::BufferUse::CopySource;
        GpuBufferHandle indexUploadHandle = context->CreateTransientBuffer(indexUploadDesc);
        RADRAY_ASSERT(indexUploadHandle.IsValid());
        auto* indexUpload = static_cast<render::Buffer*>(indexUploadHandle.NativeHandle);
        RADRAY_ASSERT(indexUpload != nullptr);
        writeBuffer(indexUpload, indexBytes);

        render::BufferDescriptor vertexBufferDesc{};
        vertexBufferDesc.Size = vertexBytes.size_bytes();
        vertexBufferDesc.Memory = render::MemoryType::Device;
        vertexBufferDesc.Usage = render::BufferUse::CopyDestination | render::BufferUse::Vertex;
        GpuBufferHandle vertexBufferHandle = context->CreateTransientBuffer(vertexBufferDesc);
        RADRAY_ASSERT(vertexBufferHandle.IsValid());
        auto* vertexBuffer = static_cast<render::Buffer*>(vertexBufferHandle.NativeHandle);
        RADRAY_ASSERT(vertexBuffer != nullptr);

        render::BufferDescriptor indexBufferDesc{};
        indexBufferDesc.Size = indexBytes.size_bytes();
        indexBufferDesc.Memory = render::MemoryType::Device;
        indexBufferDesc.Usage = render::BufferUse::CopyDestination | render::BufferUse::Index;
        GpuBufferHandle indexBufferHandle = context->CreateTransientBuffer(indexBufferDesc);
        RADRAY_ASSERT(indexBufferHandle.IsValid());
        auto* indexBuffer = static_cast<render::Buffer*>(indexBufferHandle.NativeHandle);
        RADRAY_ASSERT(indexBuffer != nullptr);

        vector<render::ResourceBarrierDescriptor> beforeCopy;
        if (needsHostWriteBarrier) {
            beforeCopy.emplace_back(render::BarrierBufferDescriptor{
                .Target = vertexUpload,
                .Before = render::BufferState::HostWrite,
                .After = render::BufferState::CopySource,
            });
            beforeCopy.emplace_back(render::BarrierBufferDescriptor{
                .Target = indexUpload,
                .Before = render::BufferState::HostWrite,
                .After = render::BufferState::CopySource,
            });
        }
        beforeCopy.emplace_back(render::BarrierBufferDescriptor{
            .Target = vertexBuffer,
            .Before = render::BufferState::Common,
            .After = render::BufferState::CopyDestination,
        });
        beforeCopy.emplace_back(render::BarrierBufferDescriptor{
            .Target = indexBuffer,
            .Before = render::BufferState::Common,
            .After = render::BufferState::CopyDestination,
        });
        cmd->ResourceBarrier(beforeCopy);
        cmd->CopyBufferToBuffer(vertexBuffer, 0, vertexUpload, 0, vertexBytes.size_bytes());
        cmd->CopyBufferToBuffer(indexBuffer, 0, indexUpload, 0, indexBytes.size_bytes());

        const render::ResourceBarrierDescriptor afterCopy[] = {
            render::BarrierBufferDescriptor{
                .Target = vertexBuffer,
                .Before = render::BufferState::CopyDestination,
                .After = render::BufferState::Vertex,
            },
            render::BarrierBufferDescriptor{
                .Target = indexBuffer,
                .Before = render::BufferState::CopyDestination,
                .After = render::BufferState::Index,
            },
        };
        cmd->ResourceBarrier(afterCopy);
        uploadedDrawList.VertexBuffer = render::VertexBufferView{
            .Target = vertexBuffer,
            .Offset = 0,
            .Size = vertexBytes.size_bytes(),
        };
        uploadedDrawList.IndexBuffer = render::IndexBufferView{
            .Target = indexBuffer,
            .Offset = 0,
            .Stride = sizeof(ImDrawIdx),
        };
    }
    uploaded.Uploaded = true;
}

void ImGuiSystem::Render(AppWindow* window, uint32_t mailboxSlot, render::GraphicsCommandEncoder* encoder) {
    RADRAY_ASSERT(_app != nullptr);
    RADRAY_ASSERT(_renderer != nullptr);
    RADRAY_ASSERT(window != nullptr);
    RADRAY_ASSERT(window->_app == _app);
    RADRAY_ASSERT(mailboxSlot < window->_mailboxes.size());
    RADRAY_ASSERT(encoder != nullptr);

    auto dataIt = std::find_if(
        _viewportRendererData.begin(),
        _viewportRendererData.end(),
        [window](const unique_ptr<ImGuiViewportRendererData>& data) {
            RADRAY_ASSERT(data != nullptr);
            return data->Window == window;
        });
    RADRAY_ASSERT(dataIt != _viewportRendererData.end());
    ImGuiViewportRendererData* data = dataIt->get();
    RADRAY_ASSERT(data != nullptr);
    RADRAY_ASSERT(mailboxSlot < data->Mailboxes.size());
    RADRAY_ASSERT(mailboxSlot < data->UploadedMailboxes.size());

    const ImGuiRenderSnapshot& snapshot = data->Mailboxes[mailboxSlot];
    const ImGuiUploadedRenderData& uploaded = data->UploadedMailboxes[mailboxSlot];
    if (!snapshot.Valid || snapshot.TotalVtxCount <= 0 || snapshot.TotalIdxCount <= 0) {
        return;
    }
    RADRAY_ASSERT(uploaded.Uploaded);
    RADRAY_ASSERT(uploaded.DrawLists.size() == snapshot.DrawLists.size());
    RADRAY_ASSERT(snapshot.DisplaySize.x > 0.0f);
    RADRAY_ASSERT(snapshot.DisplaySize.y > 0.0f);
    RADRAY_ASSERT(snapshot.FramebufferScale.x > 0.0f);
    RADRAY_ASSERT(snapshot.FramebufferScale.y > 0.0f);

    const float fbWidth = snapshot.DisplaySize.x * snapshot.FramebufferScale.x;
    const float fbHeight = snapshot.DisplaySize.y * snapshot.FramebufferScale.y;
    RADRAY_ASSERT(fbWidth > 0.0f);
    RADRAY_ASSERT(fbHeight > 0.0f);

    struct ImGuiPushConstants {
        float Scale[2];
        float Translate[2];
    };
    auto bindState = [&] {
        RADRAY_ASSERT(_renderer->_rs.IsValid());
        RADRAY_ASSERT(_renderer->_pso.IsValid());
        auto* rootSig = static_cast<render::RootSignature*>(_renderer->_rs.NativeHandle);
        auto* pso = static_cast<render::GraphicsPipelineState*>(_renderer->_pso.NativeHandle);
        RADRAY_ASSERT(rootSig != nullptr);
        RADRAY_ASSERT(pso != nullptr);
        encoder->BindRootSignature(rootSig);
        encoder->BindGraphicsPipelineState(pso);
        encoder->SetViewport(Viewport{0.0f, 0.0f, fbWidth, fbHeight, 0.0f, 1.0f});
        const ImGuiPushConstants push{
            {2.0f / snapshot.DisplaySize.x, -2.0f / snapshot.DisplaySize.y},
            {-1.0f - snapshot.DisplayPos.x * (2.0f / snapshot.DisplaySize.x),
             1.0f - snapshot.DisplayPos.y * (-2.0f / snapshot.DisplaySize.y)},
        };
        auto pushId = rootSig->FindParameterId("gPush");
        RADRAY_ASSERT(pushId.has_value());
        encoder->PushConstants(*pushId, &push, sizeof(push));
    };
    bindState();

    for (size_t drawListIndex = 0; drawListIndex < snapshot.DrawLists.size(); ++drawListIndex) {
        const ImGuiDrawListSnapshot& drawList = snapshot.DrawLists[drawListIndex];
        const ImGuiUploadedDrawList& uploadedDrawList = uploaded.DrawLists[drawListIndex];
        if (drawList.Vertices.empty() || drawList.Indices.empty()) {
            continue;
        }
        RADRAY_ASSERT(uploadedDrawList.VertexBuffer.Target != nullptr);
        RADRAY_ASSERT(uploadedDrawList.IndexBuffer.Target != nullptr);
        encoder->BindVertexBuffer(std::span{&uploadedDrawList.VertexBuffer, 1});
        encoder->BindIndexBuffer(uploadedDrawList.IndexBuffer);

        for (const ImGuiRenderCommandSnapshot& drawCmd : drawList.Commands) {
            if (drawCmd.Kind == ImGuiRenderCommandKind::ResetRenderState) {
                bindState();
                continue;
            }

            RADRAY_ASSERT(drawCmd.Kind == ImGuiRenderCommandKind::DrawIndexed);
            if (drawCmd.ElemCount == 0) {
                continue;
            }
            RADRAY_ASSERT(drawCmd.IdxOffset <= drawList.Indices.size());
            RADRAY_ASSERT(drawCmd.ElemCount <= drawList.Indices.size() - drawCmd.IdxOffset);
            RADRAY_ASSERT(drawCmd.VtxOffset <= drawList.Vertices.size());

            float clipMinX = (drawCmd.ClipRect.x - snapshot.DisplayPos.x) * snapshot.FramebufferScale.x;
            float clipMinY = (drawCmd.ClipRect.y - snapshot.DisplayPos.y) * snapshot.FramebufferScale.y;
            float clipMaxX = (drawCmd.ClipRect.z - snapshot.DisplayPos.x) * snapshot.FramebufferScale.x;
            float clipMaxY = (drawCmd.ClipRect.w - snapshot.DisplayPos.y) * snapshot.FramebufferScale.y;
            clipMinX = std::clamp(clipMinX, 0.0f, fbWidth);
            clipMinY = std::clamp(clipMinY, 0.0f, fbHeight);
            clipMaxX = std::clamp(clipMaxX, 0.0f, fbWidth);
            clipMaxY = std::clamp(clipMaxY, 0.0f, fbHeight);
            if (clipMaxX <= clipMinX || clipMaxY <= clipMinY) {
                continue;
            }

            const int32_t scissorX = static_cast<int32_t>(std::floor(clipMinX));
            const int32_t scissorY = static_cast<int32_t>(std::floor(clipMinY));
            const int32_t scissorMaxX = static_cast<int32_t>(std::ceil(clipMaxX));
            const int32_t scissorMaxY = static_cast<int32_t>(std::ceil(clipMaxY));
            RADRAY_ASSERT(scissorMaxX > scissorX);
            RADRAY_ASSERT(scissorMaxY > scissorY);
            encoder->SetScissor(Rect{
                scissorX,
                scissorY,
                static_cast<uint32_t>(scissorMaxX - scissorX),
                static_cast<uint32_t>(scissorMaxY - scissorY),
            });

            RADRAY_ASSERT(drawCmd.TexID != ImTextureID_Invalid);
            auto* binding = reinterpret_cast<ImGuiTextureBinding*>(drawCmd.TexID);
            RADRAY_ASSERT(binding != nullptr);
            RADRAY_ASSERT(binding->DescriptorSet.IsValid());
            auto* descriptorSet = static_cast<render::DescriptorSet*>(binding->DescriptorSet.NativeHandle);
            RADRAY_ASSERT(descriptorSet != nullptr);
            RADRAY_ASSERT(binding->State == render::TextureState::ShaderRead);
            encoder->BindDescriptorSet(render::DescriptorSetIndex{1}, descriptorSet);
            encoder->DrawIndexed(drawCmd.ElemCount, 1, drawCmd.IdxOffset, static_cast<int32_t>(drawCmd.VtxOffset), 0);
        }
    }
}

void ImGuiSystem::OnSubmit(AppWindow* window, uint32_t mailboxSlot, const GpuTask& task) noexcept {
    RADRAY_ASSERT(_app != nullptr);
    RADRAY_ASSERT(_renderer != nullptr);
    RADRAY_ASSERT(window != nullptr);
    RADRAY_ASSERT(window->_app == _app);
    RADRAY_ASSERT(mailboxSlot < window->_mailboxes.size());
    RADRAY_ASSERT(task.IsValid());
    RADRAY_UNUSED(task);

    auto dataIt = std::find_if(
        _viewportRendererData.begin(),
        _viewportRendererData.end(),
        [window](const unique_ptr<ImGuiViewportRendererData>& data) {
            RADRAY_ASSERT(data != nullptr);
            return data->Window == window;
        });
    RADRAY_ASSERT(dataIt != _viewportRendererData.end());
    ImGuiViewportRendererData* data = dataIt->get();
    RADRAY_ASSERT(data != nullptr);
    data->Mailboxes.resize(window->_mailboxes.size());
    data->UploadedMailboxes.resize(window->_mailboxes.size());
    RADRAY_ASSERT(mailboxSlot < data->Mailboxes.size());
    RADRAY_ASSERT(mailboxSlot < data->UploadedMailboxes.size());

    ImGuiRenderSnapshot& snapshot = data->Mailboxes[mailboxSlot];
    snapshot.Clear();
    data->UploadedMailboxes[mailboxSlot].Clear();
    this->ProcessPendingTextureDestroys();
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
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
        ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        platformIo.Renderer_CreateWindow = ImGuiRenderer::CreateWindow;
        platformIo.Renderer_DestroyWindow = ImGuiRenderer::DestroyWindow;
        platformIo.Renderer_SetWindowSize = ImGuiRenderer::SetWindowSize;
        platformIo.Renderer_RenderWindow = ImGuiRenderer::RenderWindow;
        platformIo.Renderer_SwapBuffers = ImGuiRenderer::SwapBuffers;
    }
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    RADRAY_ASSERT(mainViewport != nullptr);
    RADRAY_ASSERT(mainViewport->RendererUserData == nullptr);
    auto mainData = make_unique<ImGuiViewportRendererData>();
    mainData->Viewport = mainViewport;
    mainData->Window = desc.MainWnd;
    mainData->Mailboxes.resize(desc.MainWnd->_mailboxes.size());
    mainData->UploadedMailboxes.resize(desc.MainWnd->_mailboxes.size());
    if (desc.EnableViewports) {
        mainViewport->RendererUserData = mainData.get();
    }
    system->_viewportRendererData.push_back(std::move(mainData));
    return system;
}

}  // namespace radray

#endif
