#include <radray/imgui/imgui_app.h>

#include <radray/errors.h>
#include <radray/stopwatch.h>

#ifdef RADRAY_PLATFORM_WINDOWS
#include <imgui_impl_win32.h>
#include <radray/render/backend/d3d12_impl.h>
#include <radray/render/backend/vulkan_impl.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandlerEx(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, ImGuiIO& io);
#endif

namespace radray {

static void* _ImguiAllocBridge(size_t size, void* user_data) noexcept {
    RADRAY_UNUSED(user_data);
#ifdef RADRAY_ENABLE_MIMALLOC
    return mi_malloc(size);
#else
    return std::malloc(size);
#endif
}

static void _ImguiFreeBridge(void* ptr, void* user_data) noexcept {
    RADRAY_UNUSED(user_data);
#ifdef RADRAY_ENABLE_MIMALLOC
    mi_free(ptr);
#else
    std::free(ptr);
#endif
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

ImGuiRenderer::ImGuiRenderer(
    render::Device* device,
    render::TextureFormat rtFormat,
    uint32_t inflightFrameCount)
    : _device(device) {
    const auto backendType = device->GetBackend();
    unique_ptr<render::Shader> shaderVS;
    unique_ptr<render::Shader> shaderPS;
    if (backendType == render::RenderBackend::D3D12) {
        render::ShaderDescriptor descVS{GetImGuiShaderDXIL_VS(), render::ShaderBlobCategory::DXIL};
        shaderVS = device->CreateShader(descVS).Unwrap();
        render::ShaderDescriptor descPS{GetImGuiShaderDXIL_PS(), render::ShaderBlobCategory::DXIL};
        shaderPS = device->CreateShader(descPS).Unwrap();
    } else if (backendType == render::RenderBackend::Vulkan) {
        render::ShaderDescriptor descVS{GetImGuiShaderSPIRV_VS(), render::ShaderBlobCategory::SPIRV};
        shaderVS = device->CreateShader(descVS).Unwrap();
        render::ShaderDescriptor descPS{GetImGuiShaderSPIRV_PS(), render::ShaderBlobCategory::SPIRV};
        shaderPS = device->CreateShader(descPS).Unwrap();
    } else {
        throw ImGuiApplicationException("{} {} {}", Errors::RADRAYIMGUI, Errors::UnsupportedPlatform, backendType);
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
        render::CompareFunction::Always,
        0};
    render::RootSignatureSetElement rsElems[] = {
        {0, 0, render::ResourceBindType::Texture, 1, render::ShaderStage::Pixel},
        {0, 0, render::ResourceBindType::Sampler, 1, render::ShaderStage::Pixel, std::span(&sampler, 1)}};
    if (backendType == render::RenderBackend::D3D12) {
        rsElems[1].Slot = 0;
    } else if (backendType == render::RenderBackend::Vulkan) {
        rsElems[1].Slot = 1;
    }
    render::RootSignatureDescriptorSet descSet{rsElems};
    render::RootSignatureDescriptor rsDesc{
        {},
        std::span{&descSet, 1},
        render::RootSignatureConstant{0, 0, 64, render::ShaderStage::Vertex}};
    _rootSig = _device->CreateRootSignature(rsDesc).Unwrap();
    render::VertexElement vertexElems[] = {
        {offsetof(ImDrawVert, pos), "POSITION", 0, render::VertexFormat::FLOAT32X2, 0},
        {offsetof(ImDrawVert, uv), "TEXCOORD", 0, render::VertexFormat::FLOAT32X2, 2},
        {offsetof(ImDrawVert, col), "COLOR", 0, render::VertexFormat::UNORM8X4, 1}};
    render::VertexBufferLayout vbLayout{
        sizeof(ImDrawVert),
        render::VertexStepMode::Vertex,
        vertexElems};
    auto rtState = render::ColorTargetState::Default(rtFormat);
    rtState.Blend = render::BlendState{
        {render::BlendFactor::SrcAlpha,
         render::BlendFactor::OneMinusSrcAlpha,
         render::BlendOperation::Add},
        {render::BlendFactor::One,
         render::BlendFactor::OneMinusSrcAlpha,
         render::BlendOperation::Add}};
    render::GraphicsPipelineStateDescriptor psoDesc{
        _rootSig.get(),
        render::ShaderEntry{shaderVS.get(), "VSMain"},
        render::ShaderEntry{shaderPS.get(), "PSMain"},
        std::span{&vbLayout, 1},
        render::PrimitiveState::Default(),
        std::nullopt,
        render::MultiSampleState{1, std::numeric_limits<uint32_t>::max(), false},
        std::span{&rtState, 1}};
    psoDesc.Primitive.Cull = render::CullMode::None;
    _pso = _device->CreateGraphicsPipelineState(psoDesc).Unwrap();
    _frames.reserve(inflightFrameCount);
    for (uint32_t i = 0; i < inflightFrameCount; i++) {
        _frames.emplace_back(make_unique<Frame>());
    }
}

ImGuiRenderer::~ImGuiRenderer() noexcept {
    _frames.clear();
    _aliveTexs.clear();
    _pso.reset();
    _rootSig.reset();
}

void ImGuiRenderer::ExtractDrawData(uint32_t frameIndex, ImDrawData* drawData) {
    if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        return;
    }
    auto& frame = *_frames[frameIndex].get();
    frame._drawData.Cmds.clear();
    if (drawData->Textures != nullptr) {
        for (ImTextureData* tex : *drawData->Textures) {
            if (tex->Status != ImTextureStatus_OK) {
                if (tex->Status == ImTextureStatus_WantCreate) {
                    IM_ASSERT(tex->TexID == ImTextureID_Invalid && tex->BackendUserData == nullptr);
                    IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
                    string texName = format("imgui_tex_{}", tex->UniqueID);
                    render::TextureDescriptor texDesc{
                        render::TextureDimension::Dim2D,
                        static_cast<uint32_t>(tex->Width),
                        static_cast<uint32_t>(tex->Height),
                        1,
                        1,
                        0,
                        render::TextureFormat::RGBA8_UNORM,
                        render::TextureUse::Resource | render::TextureUse::CopyDestination,
                        {},
                        texName};
                    unique_ptr<render::Texture> texObj = _device->CreateTexture(texDesc).Unwrap();
                    render::TextureViewDescriptor texViewDesc{
                        texObj.get(),
                        render::TextureViewDimension::Dim2D,
                        texDesc.Format,
                        render::SubresourceRange::AllSub(),
                        render::TextureUse::Resource};
                    unique_ptr<render::TextureView> srv = _device->CreateTextureView(texViewDesc).Unwrap();
                    unique_ptr<render::DescriptorSet> descSet = _device->CreateDescriptorSet(_rootSig.get(), 0).Unwrap();
                    descSet->SetResource(0, 0, srv.get());
                    const auto& ptr = _aliveTexs.emplace_back(make_unique<ImGuiTexture>(std::move(texObj), std::move(srv), std::move(descSet)));
                    tex->SetTexID(std::bit_cast<ImU64>(ptr->_srv.get()));
                    auto t = _aliveTexs.begin();
                    tex->BackendUserData = ptr.get();
                }
                if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
                    auto ptr = std::bit_cast<ImGuiRenderer::ImGuiTexture*>(tex->BackendUserData);
                    IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
                    const int uploadW = tex->Width;
                    const int uploadH = tex->Height;
                    const int uploadPitchSrc = uploadW * tex->BytesPerPixel;
                    const int uploadSize = uploadPitchSrc * uploadH;
                    string uploadName = format("imgui_tex_upload_{}", tex->UniqueID);
                    render::BufferDescriptor uploadDesc{
                        static_cast<uint64_t>(uploadSize),
                        render::MemoryType::Upload,
                        render::BufferUse::CopySource | render::BufferUse::MapWrite,
                        {},
                        uploadName};
                    unique_ptr<render::Buffer> uploadBuffer = _device->CreateBuffer(uploadDesc).Unwrap();
                    void* dst = uploadBuffer->Map(0, uploadDesc.Size);
                    std::memcpy(dst, tex->GetPixels(), uploadSize);
                    frame._uploadTexReqs.emplace_back(ptr->_tex.get(), uploadBuffer.get(), tex->Status == ImTextureStatus_WantCreate);
                    frame._tempBufs.emplace_back(std::move(uploadBuffer));
                    tex->SetStatus(ImTextureStatus_OK);
                }
                if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames >= (int)_frames.size()) {
                    auto ptr = std::bit_cast<ImGuiRenderer::ImGuiTexture*>(tex->BackendUserData);
                    auto it = std::find_if(_aliveTexs.begin(), _aliveTexs.end(), [ptr](const auto& t) { return t.get() == ptr; });
                    RADRAY_ASSERT(it != _aliveTexs.end());
                    auto texIns = std::move(*it);
                    std::swap(*it, _aliveTexs.back());
                    _aliveTexs.pop_back();
                    frame._waitForFreeTexs.emplace_back(std::move(texIns));
                    tex->SetTexID(ImTextureID_Invalid);
                    tex->SetStatus(ImTextureStatus_Destroyed);
                    tex->BackendUserData = nullptr;
                }
            }
        }
    }
    if (frame._vb == nullptr || frame._vbSize < drawData->TotalVtxCount) {
        if (frame._vb) {
            frame._tempBufs.emplace_back(std::move(frame._vb));
        }
        int vertCount = drawData->TotalVtxCount + 5000;
        string vbName = format("imgui_vb_{}", frameIndex);
        render::BufferDescriptor desc{
            vertCount * sizeof(ImDrawVert),
            render::MemoryType::Upload,
            render::BufferUse::Vertex | render::BufferUse::MapWrite,
            {},
            vbName};
        frame._vb = _device->CreateBuffer(desc).Unwrap();
        frame._vbMapped = frame._vb->Map(0, desc.Size);
        frame._vbSize = vertCount;
    }
    if (frame._ib == nullptr || frame._ibSize < drawData->TotalIdxCount) {
        if (frame._ib) {
            frame._tempBufs.emplace_back(std::move(frame._ib));
        }
        int idxCount = drawData->TotalIdxCount + 10000;
        string ibName = format("imgui_ib_{}", frameIndex);
        render::BufferDescriptor desc{
            idxCount * sizeof(ImDrawIdx),
            render::MemoryType::Upload,
            render::BufferUse::Index | render::BufferUse::MapWrite,
            {},
            ibName};
        frame._ib = _device->CreateBuffer(desc).Unwrap();
        frame._ibMapped = frame._ib->Map(0, desc.Size);
        frame._ibSize = idxCount;
    }
    {
        void* dst = frame._vbMapped;
        for (const ImDrawList* drawList : drawData->CmdLists) {
            std::memcpy(dst, drawList->VtxBuffer.Data, drawList->VtxBuffer.Size * sizeof(ImDrawVert));
            dst = static_cast<ImDrawVert*>(dst) + drawList->VtxBuffer.Size;
        }
    }
    {
        void* dst = frame._ibMapped;
        for (const ImDrawList* drawList : drawData->CmdLists) {
            std::memcpy(dst, drawList->IdxBuffer.Data, drawList->IdxBuffer.Size * sizeof(ImDrawIdx));
            dst = static_cast<ImDrawIdx*>(dst) + drawList->IdxBuffer.Size;
        }
    }
    frame._drawData.DisplayPos = drawData->DisplayPos;
    frame._drawData.DisplaySize = drawData->DisplaySize;
    frame._drawData.FramebufferScale = drawData->FramebufferScale;
    frame._drawData.TotalVtxCount = drawData->TotalVtxCount;
    frame._drawData.Cmds.reserve(drawData->CmdLists.Size);
    for (const auto drawList : drawData->CmdLists) {
        auto& dl = frame._drawData.Cmds.emplace_back();
        dl.VtxBufferSize = drawList->VtxBuffer.Size;
        dl.IdxBufferSize = drawList->IdxBuffer.Size;
        dl.Cmd.reserve(drawList->CmdBuffer.Size);
        for (const auto& cmd : drawList->CmdBuffer) {
            auto& dc = dl.Cmd.emplace_back();
            dc.ClipRect = cmd.ClipRect;
            dc.TexRef = cmd.TexRef;
            dc.VtxOffset = cmd.VtxOffset;
            dc.IdxOffset = cmd.IdxOffset;
            dc.ElemCount = cmd.ElemCount;
            dc.UserCallback = cmd.UserCallback;
        }
    }
}

void ImGuiRenderer::OnRenderBegin(uint32_t frameIndex, render::CommandBuffer* cmdBuffer) {
    auto& frame = *_frames[frameIndex].get();
    const auto drawData = &frame._drawData;
    if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        return;
    }
    if (frame._uploadTexReqs.size() > 0) {
        vector<render::BarrierTextureDescriptor> barriers;
        barriers.reserve(frame._uploadTexReqs.size());
        for (const auto& payload : frame._uploadTexReqs) {
            auto& barrierBefore = barriers.emplace_back();
            barrierBefore.Target = payload._dst;
            barrierBefore.Before = payload._isNew ? render::TextureUse::Uninitialized : render::TextureUse::Resource;
            barrierBefore.After = render::TextureUse::CopyDestination;
            barrierBefore.IsFromOrToOtherQueue = false;
            barrierBefore.IsSubresourceBarrier = false;
        }
        cmdBuffer->ResourceBarrier({}, barriers);
        for (const auto& payload : frame._uploadTexReqs) {
            render::SubresourceRange range{0, 1, 0, 1};
            cmdBuffer->CopyBufferToTexture(payload._dst, range, payload._src, 0);
        }
        barriers.clear();
        for (const auto& payload : frame._uploadTexReqs) {
            auto& barrierBefore = barriers.emplace_back();
            barrierBefore.Target = payload._dst;
            barrierBefore.Before = render::TextureUse::CopyDestination;
            barrierBefore.After = render::TextureUse::Resource;
            barrierBefore.IsFromOrToOtherQueue = false;
            barrierBefore.IsSubresourceBarrier = false;
        }
        cmdBuffer->ResourceBarrier({}, barriers);
        frame._uploadTexReqs.clear();
    }
}

void ImGuiRenderer::OnRender(uint32_t frameIndex, render::CommandEncoder* encoder) {
    auto& frame = *_frames[frameIndex].get();
    const auto drawData = &frame._drawData;
    if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        return;
    }
    int fbWidth = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    int fbHeight = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    this->SetupRenderState(frameIndex, encoder, fbWidth, fbHeight);
    ImVec2 clipOff = drawData->DisplayPos;
    ImVec2 clipScale = drawData->FramebufferScale;
    int globalVtxOffset = 0;
    int globalIdxOffset = 0;
    for (const auto& drawList : drawData->Cmds) {
        for (const auto& cmd : drawList.Cmd) {
            if (cmd.UserCallback != nullptr) {
                if (cmd.UserCallback == ImDrawCallback_ResetRenderState) {
                    this->SetupRenderState(frameIndex, encoder, fbWidth, fbHeight);
                }
            } else {
                ImVec2 clipMin((cmd.ClipRect.x - clipOff.x) * clipScale.x, (cmd.ClipRect.y - clipOff.y) * clipScale.y);
                ImVec2 clipMax((cmd.ClipRect.z - clipOff.x) * clipScale.x, (cmd.ClipRect.w - clipOff.y) * clipScale.y);
                if (clipMin.x < 0.0f) {
                    clipMin.x = 0.0f;
                }
                if (clipMin.y < 0.0f) {
                    clipMin.y = 0.0f;
                }
                if (clipMax.x > (float)fbWidth) {
                    clipMax.x = (float)fbWidth;
                }
                if (clipMax.y > (float)fbHeight) {
                    clipMax.y = (float)fbHeight;
                }
                if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) {
                    continue;
                }
                Rect scissor{};
                scissor.X = (int)clipMin.x;
                scissor.Y = (int)clipMin.y;
                scissor.Width = (int)(clipMax.x - clipMin.x);
                scissor.Height = (int)(clipMax.y - clipMin.y);
                encoder->SetScissor(scissor);
                if (cmd.TexRef._TexData) {
                    auto tex = std::bit_cast<ImGuiRenderer::ImGuiTexture*>(cmd.TexRef._TexData->BackendUserData);
                    encoder->BindDescriptorSet(0, tex->_descSet.get());
                } else if (cmd.TexRef._TexID) {
                    auto texView = std::bit_cast<render::TextureView*>(cmd.TexRef._TexID);
                    auto it = frame._tempTexSets.find(texView);
                    if (it == frame._tempTexSets.end()) {
                        unique_ptr<render::DescriptorSet> descSet = _device->CreateDescriptorSet(_rootSig.get(), 0).Unwrap();
                        descSet->SetResource(0, 0, texView);
                        auto emplaceResult = frame._tempTexSets.emplace(texView, std::move(descSet));
                        it = emplaceResult.first;
                    }
                    encoder->BindDescriptorSet(0, it->second.get());
                }
                encoder->DrawIndexed(cmd.ElemCount, 1, cmd.IdxOffset + globalIdxOffset, cmd.VtxOffset + globalVtxOffset, 0);
            }
        }
        globalIdxOffset += drawList.IdxBufferSize;
        globalVtxOffset += drawList.VtxBufferSize;
    }
    encoder->SetScissor({0, 0, (uint32_t)fbWidth, (uint32_t)fbHeight});
}

void ImGuiRenderer::OnRenderComplete(uint32_t frameIndex) {
    auto& frame = *_frames[frameIndex].get();
    frame._tempBufs.clear();
    frame._tempTexSets.clear();
    frame._waitForFreeTexs.clear();
}

void ImGuiRenderer::SetupRenderState(int frameIndex, render::CommandEncoder* encoder, int fbWidth, int fbHeight) {
    const auto& frame = *_frames[frameIndex].get();
    const auto drawData = &frame._drawData;
    encoder->BindRootSignature(_rootSig.get());
    encoder->BindGraphicsPipelineState(_pso.get());
    if (drawData->TotalVtxCount > 0) {
        render::VertexBufferView vbv{
            frame._vb.get(),
            0,
            frame._vbSize * sizeof(ImDrawVert)};
        encoder->BindVertexBuffer(std::span{&vbv, 1});
        render::IndexBufferView ibv{
            frame._ib.get(),
            0,
            sizeof(ImDrawIdx)};
        encoder->BindIndexBuffer(ibv);
    }
    Viewport vp{0, 0, (float)fbWidth, (float)fbHeight, 0.0f, 1.0f};
    if (_device->GetBackend() == render::RenderBackend::Vulkan) {
        vp.Y = (float)fbHeight;
        vp.Height = -(float)fbHeight;
    }
    encoder->SetViewport(vp);
    float L = drawData->DisplayPos.x;
    float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
    float T = drawData->DisplayPos.y;
    float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
    Eigen::Matrix4f proj = OrthoLH(L, R, B, T, 0.0f, 1.0f);
    encoder->PushConstant(proj.data(), sizeof(proj));
}

void ImGuiApplication::Setup(const ImGuiAppConfig& config) {
    this->OnStart(config);
}

void ImGuiApplication::Run() {
    if (_enableMultiThreading) {
        this->LoopMultiThreaded();
    } else {
        this->LoopSingleThreaded();
    }
}

void ImGuiApplication::Destroy() noexcept {
    if (_cmdQueue) _cmdQueue->Wait();

    this->OnDestroy();

    // render
    if (_imgui) {
        _imgui->SetCurrent();
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = nullptr;
        io.BackendRendererUserData = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    }
    _imguiRenderer.reset();
    _defaultRTVs.clear();
    _backBuffers.clear();
    _imageAvailableSemaphores.clear();
    _renderFinishSemaphores.clear();
    _inFlightFences.clear();
    _swapchain.reset();
    _cmdQueue = nullptr;
    _device.reset();
    if (_vkIns) render::DestroyVulkanInstance(std::move(_vkIns));
    // imgui
    if (_imgui) {
        _imgui->SetCurrent();
#ifdef RADRAY_PLATFORM_WINDOWS
        ImGui_ImplWin32_Shutdown();
#endif
    }
    _imgui.reset();
    // window
    _window.reset();
}

void ImGuiApplication::Init(const ImGuiAppConfig& config_) {
    auto config = config_;
    _rtWidth = config.Width;
    _rtHeight = config.Height;
    _backBufferCount = config.BackBufferCount;
    _inFlightFrameCount = config.InFlightFrameCount;
    _rtFormat = config.RTFormat;
    _enableVSync = config.EnableVSync;
    _enableMultiThreading = config.EnableMultiThreading;
    _enableFrameDropping = config.EnableFrameDropping;
    // window
#ifdef RADRAY_PLATFORM_WINDOWS
    ImGui_ImplWin32_EnableDpiAwareness();
    std::function<Win32MsgProc> imguiProc = [this](void* hwnd_, uint32_t msg_, uint64_t wparam_, int64_t lparam_) -> int64_t {
        if (!_imgui) {
            return 0;
        }
        ImGui::SetCurrentContext(_imgui->Get());
        ImGuiIO& io = ImGui::GetIO();
        return ImGui_ImplWin32_WndProcHandlerEx(std::bit_cast<HWND>(hwnd_), msg_, wparam_, lparam_, io);
    };
    Win32WindowCreateDescriptor desc{};
    desc.Title = config.Title;
    desc.Width = config.Width;
    desc.Height = config.Height;
    desc.X = -1;
    desc.Y = -1;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    desc.ExtraWndProcs = std::span{&imguiProc, 1};
    _window = CreateNativeWindow(desc).Unwrap();
#endif
    if (!_window) {
        throw ImGuiApplicationException("{}: {}", Errors::RADRAYIMGUI, "fail create window");
    }
    // imgui
    IMGUI_CHECKVERSION();
    ImGui::SetAllocatorFunctions(_ImguiAllocBridge, _ImguiFreeBridge, nullptr);
    _imgui = make_unique<ImGuiContextRAII>();
    _imgui->SetCurrent();
    ImGuiIO& io = ImGui::GetIO();
#ifdef RADRAY_PLATFORM_WINDOWS
    {
        WindowNativeHandler wnh = _window->GetNativeHandler();
        if (wnh.Type != WindowHandlerTag::HWND) {
            throw ImGuiApplicationException("{}: {}", Errors::RADRAYIMGUI, "unknown window handler type");
        }
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        float mainScale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(mainScale);
        style.FontScaleDpi = mainScale;
        ImGui_ImplWin32_Init(wnh.Handle);
        io.Fonts->AddFontDefault();
    }
#endif
    // render
    if (config.Backend == render::RenderBackend::Vulkan) {
        render::VulkanInstanceDescriptor insDesc{
            config.AppName,
            1,
            "RadRay",
            1,
            config.EnableValidation,
            config.EnableValidation};
        _vkIns = CreateVulkanInstance(insDesc).Unwrap();
        render::VulkanCommandQueueDescriptor queueDesc = {render::QueueType::Direct, 1};
        render::VulkanDeviceDescriptor devDesc{};
        if (config.DeviceDesc.has_value()) {
            devDesc = std::get<render::VulkanDeviceDescriptor>(config.DeviceDesc.value());
        } else {
            devDesc.Queues = std::span{&queueDesc, 1};
        }
        _device = CreateDevice(devDesc).Unwrap();
    } else if (config.Backend == render::RenderBackend::D3D12) {
        render::D3D12DeviceDescriptor devDesc{
            std::nullopt,
            config.EnableValidation,
            config.EnableValidation};
        if (config.DeviceDesc.has_value()) {
            devDesc = std::get<render::D3D12DeviceDescriptor>(config.DeviceDesc.value());
        }
        _device = CreateDevice(devDesc).Unwrap();
    }
    if (!_device) {
        throw ImGuiApplicationException("{}: {}", Errors::RADRAYIMGUI, "fail create device");
    }
    _cmdQueue = _device->GetCommandQueue(render::QueueType::Direct, 0).Unwrap();
    this->RecreateSwapChain();
    _inFlightFences.resize(_inFlightFrameCount);
    for (auto& fence : _inFlightFences) {
        fence = StaticCastUniquePtr<render::vulkan::FenceVulkan>(_device->CreateFence().Unwrap());
    }
    _imguiRenderer = make_unique<ImGuiRenderer>(_device.get(), _rtFormat, _inFlightFrameCount);
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");
    io.BackendRendererUserData = _imguiRenderer.get();
    io.BackendRendererName = "radray_imgui_imguiRenderer_impl";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
}

void ImGuiApplication::RecreateSwapChain() {
    if (_swapchain) {
        _cmdQueue->Wait();
        _swapchain.reset();
    }
    render::SwapChainDescriptor swapchainDesc{
        _cmdQueue,
        _window->GetNativeHandler().Handle,
        _rtWidth,
        _rtHeight,
        _backBufferCount,
        _inFlightFrameCount,
        _rtFormat,
        _enableVSync};
    _swapchain = _device->CreateSwapChain(swapchainDesc).Unwrap();
    if (_device->GetBackend() == render::RenderBackend::Vulkan) {
        _renderFinishSemaphores.resize(_backBufferCount);
        for (auto& semaphore : _renderFinishSemaphores) {
            semaphore = StaticCastUniquePtr<render::vulkan::SemaphoreVulkan>(_device->CreateSemaphoreDevice().Unwrap());
        }
        _imageAvailableSemaphores.resize(_inFlightFrameCount);
        for (auto& semaphore : _imageAvailableSemaphores) {
            semaphore = StaticCastUniquePtr<render::vulkan::SemaphoreVulkan>(_device->CreateSemaphoreDevice().Unwrap());
        }
    }
    _defaultRTVs.resize(_backBufferCount);
    for (auto& rtView : _defaultRTVs) {
        rtView.reset();
    }
    _backBuffers.resize(_backBufferCount);
    for (auto& rt : _backBuffers) {
        rt = nullptr;
    }
    _frameState.resize(_inFlightFrameCount);
    for (auto& state : _frameState) {
        state = false;
    }
    _frameCount = 0;
    if (_imguiRenderer) {
        for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
            _imguiRenderer->OnRenderComplete(i);
        }
    }
}

render::TextureView* ImGuiApplication::GetDefaultRTV(uint32_t backBufferIndex) {
    if (_defaultRTVs[backBufferIndex] == nullptr) {
        render::Texture* backBuffer = _backBuffers[backBufferIndex];
        render::TextureViewDescriptor rtvDesc{
            backBuffer,
            render::TextureViewDimension::Dim2D,
            _rtFormat,
            render::SubresourceRange::AllSub(),
            render::TextureUse::RenderTarget};
        _defaultRTVs[backBufferIndex] = _device->CreateTextureView(rtvDesc).Unwrap();
    }
    return _defaultRTVs[backBufferIndex].get();
}

void ImGuiApplication::LoopSingleThreaded() {
    while (true) {
        _window->DispatchEvents();
        _needClose = _window->ShouldClose();
        if (_needClose) {
            break;
        }
        this->OnUpdate();
        _imgui->SetCurrent();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        this->OnImGui();
        ImGui::Render();

        const uint32_t frameIndex = static_cast<uint32_t>(_frameCount % _inFlightFrameCount);
        {
            render::Fence* fence = _inFlightFences[frameIndex].get();
            if (_enableFrameDropping) {
                auto status = fence->GetStatus();
                if (status == render::FenceStatus::Incomplete) {
                    continue;
                }
                fence->Reset();
            } else {
                fence->Wait();
            }
        }
        if (_frameState[frameIndex]) {
            _imguiRenderer->OnRenderComplete(frameIndex);
            _frameState[frameIndex] = false;
        }
        _imguiRenderer->ExtractDrawData(frameIndex, ImGui::GetDrawData());
        {
            render::Semaphore* imageAvailableSemaphore = nullptr;
            if (_device->GetBackend() == render::RenderBackend::Vulkan) {
                imageAvailableSemaphore = _imageAvailableSemaphores[frameIndex].get();
            }
            auto rtOpt = _swapchain->AcquireNext(imageAvailableSemaphore, nullptr);
            if (!rtOpt.HasValue()) {
                continue;
            }
            uint32_t backBufferIndex = _swapchain->GetCurrentBackBufferIndex();
            _backBuffers[backBufferIndex] = rtOpt.Release();
        }
        auto submitCmdBufs = this->OnRender(frameIndex);
        {
            render::Semaphore* renderFinishSemaphore = nullptr;
            render::Semaphore* imageAvailableSemaphore = nullptr;
            if (_device->GetBackend() == render::RenderBackend::Vulkan) {
                uint32_t backBufferIndex = _swapchain->GetCurrentBackBufferIndex();
                renderFinishSemaphore = _renderFinishSemaphores[backBufferIndex].get();
                imageAvailableSemaphore = _imageAvailableSemaphores[frameIndex].get();
            }
            render::Fence* fence = _inFlightFences[frameIndex].get();
            render::CommandQueueSubmitDescriptor submitDesc{
                submitCmdBufs,
                fence,
                imageAvailableSemaphore ? std::span{&imageAvailableSemaphore, 1} : std::span<render::Semaphore*>{},
                renderFinishSemaphore ? std::span{&renderFinishSemaphore, 1} : std::span<render::Semaphore*>{}};
            _cmdQueue->Submit(submitDesc);
            _swapchain->Present(renderFinishSemaphore ? std::span{&renderFinishSemaphore, 1} : std::span<render::Semaphore*>{});
            _frameState[frameIndex] = true;
            _frameCount++;
        }
    }
}

void ImGuiApplication::LoopMultiThreaded() {
    _freeFrames = make_unique<BoundedChannel<uint32_t>>(_inFlightFrameCount);
    _submitFrames = make_unique<BoundedChannel<uint32_t>>(_inFlightFrameCount);
    for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
        _freeFrames->TryWrite(i);
    }

    _renderThread = make_unique<std::thread>([this]() {
        _frameCount = 0;
        vector<uint32_t> slotIdx;
        slotIdx.resize(_inFlightFrameCount, std::numeric_limits<uint32_t>::max());
        Stopwatch sw = Stopwatch::StartNew();
        while (true) {
            if (_needClose) {
                break;
            }
            const uint32_t processIndex = static_cast<uint32_t>(_frameCount % _inFlightFrameCount);
            if (_frameState[processIndex]) {
                render::Fence* fence = _inFlightFences[processIndex].get();
                fence->Wait();
                _imguiRenderer->OnRenderComplete(slotIdx[processIndex]);
                _frameState[processIndex] = false;
                _freeFrames->WaitWrite(slotIdx[processIndex]);
                slotIdx[processIndex] = std::numeric_limits<uint32_t>::max();

                double last = _gpuTime;
                _gpuTime = sw.Elapsed().count() * 0.000001;
                _gpuDeltaTime = _gpuTime - last;
            }
            if (slotIdx[processIndex] == std::numeric_limits<uint32_t>::max()) {
                uint32_t reqIndex;
                if (!_submitFrames->WaitRead(reqIndex)) {
                    break;
                }
                slotIdx[processIndex] = reqIndex;
            }
            {
                render::Semaphore* imageAvailableSemaphore = nullptr;
                if (_device->GetBackend() == render::RenderBackend::Vulkan) {
                    imageAvailableSemaphore = _imageAvailableSemaphores[processIndex].get();
                }
                auto rtOpt = _swapchain->AcquireNext(imageAvailableSemaphore, nullptr);
                if (!rtOpt.HasValue()) {
                    continue;
                }
                uint32_t backBufferIndex = _swapchain->GetCurrentBackBufferIndex();
                _backBuffers[backBufferIndex] = rtOpt.Release();
            }
            auto submitCmdBufs = this->OnRender(slotIdx[processIndex]);
            {
                render::Semaphore* renderFinishSemaphore = nullptr;
                render::Semaphore* imageAvailableSemaphore = nullptr;
                if (_device->GetBackend() == render::RenderBackend::Vulkan) {
                    uint32_t backBufferIndex = _swapchain->GetCurrentBackBufferIndex();
                    renderFinishSemaphore = _renderFinishSemaphores[backBufferIndex].get();
                    imageAvailableSemaphore = _imageAvailableSemaphores[processIndex].get();
                }
                render::Fence* fence = _inFlightFences[processIndex].get();
                render::CommandQueueSubmitDescriptor submitDesc{
                    submitCmdBufs,
                    fence,
                    imageAvailableSemaphore ? std::span{&imageAvailableSemaphore, 1} : std::span<render::Semaphore*>{},
                    renderFinishSemaphore ? std::span{&renderFinishSemaphore, 1} : std::span<render::Semaphore*>{}};
                _cmdQueue->Submit(submitDesc);
                _swapchain->Present(renderFinishSemaphore ? std::span{&renderFinishSemaphore, 1} : std::span<render::Semaphore*>{});
                _frameState[processIndex] = true;
                _frameCount++;
            }
        }
    });
    {
        Stopwatch sw = Stopwatch::StartNew();
        while (true) {
            auto last = _time;
            _time = sw.Elapsed().count() * 0.000001;
            _deltaTime = _time - last;

            _window->DispatchEvents();
            _needClose = _window->ShouldClose();
            if (_needClose) {
                break;
            }
            this->OnUpdate();
            _imgui->SetCurrent();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            this->OnImGui();
            ImGui::Render();

            uint32_t frameIndex;
            bool isConsumed = false;
            if (_enableFrameDropping) {
                isConsumed = _freeFrames->TryRead(frameIndex);
            } else {
                isConsumed = _freeFrames->WaitRead(frameIndex);
            }
            if (!isConsumed) {
                continue;
            }
            _imguiRenderer->ExtractDrawData(frameIndex, ImGui::GetDrawData());
            _submitFrames->WaitWrite(frameIndex);
        }
    }
    _submitFrames->Complete();
    _freeFrames->Complete();
    _renderThread->join();
}

void ImGuiApplication::OnStart(const ImGuiAppConfig& config) { RADRAY_UNUSED(config); }

void ImGuiApplication::OnDestroy() noexcept {}

void ImGuiApplication::OnUpdate() {}

void ImGuiApplication::OnImGui() {}

}  // namespace radray
