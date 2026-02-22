#include <radray/imgui/imgui_app.h>

#ifdef RADRAY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <imgui_impl_win32.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandlerEx(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, ImGuiIO& io);
#endif

#ifdef RADRAY_PLATFORM_MACOS
#include "imgui_osx_bridge.h"
#endif

namespace radray {

static void* _ImguiAllocBridge(size_t size, void* user_data) noexcept {
    RADRAY_UNUSED(user_data);
    return std::malloc(size);
}

static void _ImguiFreeBridge(void* ptr, void* user_data) noexcept {
    RADRAY_UNUSED(user_data);
    std::free(ptr);
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
    } else if (backendType == render::RenderBackend::Metal) {
        render::ShaderDescriptor desc{GetImGuiShaderMETALLIB(), render::ShaderBlobCategory::METALLIB};
        shaderVS = device->CreateShader(desc).Unwrap();
    } else {
        throw ImGuiApplicationException("unsupported backend {}", backendType);
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
        {0, 0, render::ResourceBindType::Texture, 1, render::ShaderStage::Pixel}};
    render::RootSignatureStaticSampler staticSampler{};
    staticSampler.Slot = 0;
    staticSampler.Space = 0;
    staticSampler.SetIndex = 0;
    staticSampler.Stages = render::ShaderStage::Pixel;
    staticSampler.Desc = sampler;
    if (backendType == render::RenderBackend::Vulkan || backendType == render::RenderBackend::Metal) {
        staticSampler.Slot = 1;
    }
    render::RootSignatureDescriptorSet descSet{rsElems};
    render::RootSignatureDescriptor rsDesc{
        {},
        std::span{&descSet, 1},
        std::span{&staticSampler, 1},
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
        render::ShaderEntry{backendType == render::RenderBackend::Metal ? shaderVS.get() : shaderPS.get(), "PSMain"},
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
                    string texName = fmt::format("imgui_tex_{}", tex->UniqueID);
                    render::TextureDescriptor texDesc{
                        render::TextureDimension::Dim2D,
                        static_cast<uint32_t>(tex->Width),
                        static_cast<uint32_t>(tex->Height),
                        1,
                        1,
                        0,
                        render::TextureFormat::RGBA8_UNORM,
                        render::MemoryType::Device,
                        render::TextureUse::Resource | render::TextureUse::CopyDestination,
                        {},
                        texName};
                    unique_ptr<render::Texture> texObj = _device->CreateTexture(texDesc).Unwrap();
                    render::TextureViewDescriptor texViewDesc{
                        texObj.get(),
                        render::TextureDimension::Dim2D,
                        texDesc.Format,
                        render::SubresourceRange::AllSub(),
                        render::TextureUse::Resource};
                    unique_ptr<render::TextureView> srv = _device->CreateTextureView(texViewDesc).Unwrap();
                    unique_ptr<render::DescriptorSet> descSet = _device->CreateDescriptorSet(_rootSig.get(), 0).Unwrap();
                    descSet->SetResource(0, 0, srv.get());
                    const auto& ptr = _aliveTexs.emplace_back(make_unique<ImGuiTexture>(std::move(texObj), std::move(srv), std::move(descSet)));
                    tex->SetTexID(std::bit_cast<ImU64>(ptr->_srv.get()));
                    tex->BackendUserData = ptr.get();
                }
                if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
                    auto ptr = std::bit_cast<ImGuiRenderer::ImGuiTexture*>(tex->BackendUserData);
                    IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
                    const int uploadW = tex->Width;
                    const int uploadH = tex->Height;
                    const int uploadPitchSrc = uploadW * tex->BytesPerPixel;
                    const int uploadSize = uploadPitchSrc * uploadH;
                    string uploadName = fmt::format("imgui_tex_upload_{}", tex->UniqueID);
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
        string vbName = fmt::format("imgui_vb_{}", frameIndex);
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
        string ibName = fmt::format("imgui_ib_{}", frameIndex);
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

void ImGuiRenderer::OnRender(uint32_t frameIndex, render::GraphicsCommandEncoder* encoder) {
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
    frame._uploadTexReqs.clear();
    frame._tempBufs.clear();
    frame._tempTexSets.clear();
    frame._waitForFreeTexs.clear();
}

void ImGuiRenderer::SetupRenderState(int frameIndex, render::GraphicsCommandEncoder* encoder, int fbWidth, int fbHeight) {
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
    _sw = Stopwatch::StartNew();
    while (true) {
        if (_enableMultiThreading) {
            this->LoopMultiThreaded();
        } else {
            this->LoopSingleThreaded();
        }
        if (_needClose) {
            break;
        }
        _needReLoop = false;
    }
}

void ImGuiApplication::Destroy() noexcept {
    _needClose = true;
    if (_submitFrames) {
        _submitFrames->Complete();
    }
    if (_freeFrames) {
        _freeFrames->Complete();
    }
    if (_renderThread && _renderThread->joinable()) {
        try {
            _renderThread->join();
        } catch (...) {
            RADRAY_ERR_LOG("join render thread failed");
        }
    }

    if (_cmdQueue) _cmdQueue->Wait();
    _resizingConn.disconnect();
    _resizedConn.disconnect();

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
#ifdef RADRAY_PLATFORM_MACOS
        ImGuiOSXBridge_Shutdown();
#endif
    }
    _imgui.reset();
    // window
    _window.reset();
}

void ImGuiApplication::Init(const ImGuiAppConfig& config_) {
    auto config = config_;
    _rtWidth = (int32_t)config.Width;
    _rtHeight = (int32_t)config.Height;
    _backBufferCount = config.BackBufferCount;
    _inFlightFrameCount = config.InFlightFrameCount;
    _rtFormat = config.RTFormat;
    _enableValidation = config.EnableValidation;
    _presentMode = config.PresentMode;
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
#ifdef RADRAY_PLATFORM_MACOS
    CocoaWindowCreateDescriptor desc{};
    desc.Title = config.Title;
    desc.Width = config.Width;
    desc.Height = config.Height;
    desc.X = -1;
    desc.Y = -1;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    _window = CreateNativeWindow(desc).Unwrap();
#endif
    if (!_window) {
        throw ImGuiApplicationException("create window failed");
    }
    _resizedConn = _window->EventResized().connect(&ImGuiApplication::OnResized, this);
    _resizingConn = _window->EventResizing().connect(&ImGuiApplication::OnResizing, this);
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
            throw ImGuiApplicationException("unknown window handler type");
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
#ifdef RADRAY_PLATFORM_MACOS
    {
        WindowNativeHandler wnh = _window->GetNativeHandler();
        if (wnh.Type != WindowHandlerTag::NS_VIEW) {
            throw ImGuiApplicationException("unknown window handler type");
        }
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGuiOSXBridge_Init(wnh.Handle);
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
            false};
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
    } else if (config.Backend == render::RenderBackend::Metal) {
        render::MetalDeviceDescriptor devDesc{};
        if (config.DeviceDesc.has_value()) {
            devDesc = std::get<render::MetalDeviceDescriptor>(config.DeviceDesc.value());
        }
        _device = CreateDevice(devDesc).Unwrap();
    }
    if (!_device) {
        throw ImGuiApplicationException("create device failed");
    }
    _cmdQueue = _device->GetCommandQueue(render::QueueType::Direct, 0).Unwrap();
    this->RecreateSwapChain();
    _inFlightFences.resize(_inFlightFrameCount);
    for (auto& fence : _inFlightFences) {
        fence = _device->CreateFence().Unwrap();
    }
    _imguiRenderer = make_unique<ImGuiRenderer>(_device.get(), _rtFormat, _inFlightFrameCount);
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");
    io.BackendRendererUserData = _imguiRenderer.get();
    io.BackendRendererName = "radray_imgui_imguiRenderer_impl";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
}

void ImGuiApplication::RecreateSwapChain() {
    std::unique_lock<std::mutex> lock;
    if (_renderThread != nullptr) {
        lock = std::unique_lock<std::mutex>{_shareMutex};
    }
    if (_rtWidth <= 0 || _rtHeight <= 0) {
        return;
    }
    if (_swapchain) {
        _cmdQueue->Wait();
        _swapchain.reset();
    }
    render::SwapChainDescriptor swapchainDesc{
        _cmdQueue,
        _window->GetNativeHandler().Handle,
        (uint32_t)_rtWidth,
        (uint32_t)_rtHeight,
        _backBufferCount,
        _inFlightFrameCount,
        _rtFormat,
        _presentMode};
    _swapchain = _device->CreateSwapChain(swapchainDesc).Unwrap();
    if (_device->GetBackend() == render::RenderBackend::Vulkan) {
        _renderFinishSemaphores.resize(_backBufferCount);
        for (auto& semaphore : _renderFinishSemaphores) {
            semaphore = _device->CreateSemaphoreDevice().Unwrap();
        }
        _imageAvailableSemaphores.resize(_inFlightFrameCount);
        for (auto& semaphore : _imageAvailableSemaphores) {
            semaphore = _device->CreateSemaphoreDevice().Unwrap();
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
}

render::TextureView* ImGuiApplication::GetDefaultRTV(uint32_t backBufferIndex) {
    if (_defaultRTVs[backBufferIndex] == nullptr) {
        render::Texture* backBuffer = _backBuffers[backBufferIndex];
        render::TextureViewDescriptor rtvDesc{
            backBuffer,
            render::TextureDimension::Dim2D,
            _rtFormat,
            render::SubresourceRange::AllSub(),
            render::TextureUse::RenderTarget};
        _defaultRTVs[backBufferIndex] = _device->CreateTextureView(rtvDesc).Unwrap();
    }
    return _defaultRTVs[backBufferIndex].get();
}

void ImGuiApplication::RequestRecreateSwapChain(std::function<void()> setValueFunc) {
    if (_renderThread != nullptr) {
        std::unique_lock<std::mutex> lock{_shareMutex};
        setValueFunc();
        _needRecreate = true;
    } else {
        setValueFunc();
        _needRecreate = true;
    }
}

Eigen::Vector2i ImGuiApplication::GetRTSize() const noexcept {
    return Eigen::Vector2i{_rtWidth, _rtHeight};
}

void ImGuiApplication::LoopSingleThreaded() {
    _frameCount = 0;
    _renderFrameStates.resize(_inFlightFrameCount);
    for (auto& i : _renderFrameStates) {
        i = RenderFrameState::Invalid();
    }
    while (true) {
        if (_needReLoop) {
            break;
        }
        _window->DispatchEvents();
        _needClose = _window->ShouldClose();
        if (_needClose) {
            break;
        }
        this->OnUpdate();
        _imgui->SetCurrent();
#ifdef RADRAY_PLATFORM_WINDOWS
        ImGui_ImplWin32_NewFrame();
#endif
#ifdef RADRAY_PLATFORM_MACOS
        ImGuiOSXBridge_NewFrame(_window->GetNativeHandler().Handle);
#endif
        ImGui::NewFrame();
        this->OnImGui();
        ImGui::Render();
        _nowCpuTimePoint = _sw.Elapsed().count() * 0.000001;

        const uint32_t frameIndex = static_cast<uint32_t>(_frameCount % _inFlightFrameCount);
        auto& state = _renderFrameStates[frameIndex];
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
        if (state.IsSubmitted) {
            this->OnRenderComplete(state.InFlightFrameIndex);
            state = RenderFrameState::Invalid();

            _nowGpuTimePoint = _sw.Elapsed().count() * 0.000001;
        }
        if (_rtWidth <= 0 || _rtHeight <= 0) {
            continue;
        }
        if (_needRecreate) {
            this->RecreateSwapChain();
            this->OnRecreateSwapChain();
            for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
                if (_renderFrameStates[i].IsValid()) {
                    this->OnRenderComplete(_renderFrameStates[i].InFlightFrameIndex);
                    _renderFrameStates[i] = RenderFrameState::Invalid();
                }
                _inFlightFences[i]->Reset();
            }
            _needRecreate = false;
        }
        state.InFlightFrameIndex = frameIndex;
        this->OnExtractDrawData(frameIndex);
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
            // Metal drawable textures change each frame; invalidate cached view
            if (_device->GetBackend() == render::RenderBackend::Metal) {
                _defaultRTVs[backBufferIndex].reset();
            }
        }
        auto submitCmdBufs = this->OnRender(state.InFlightFrameIndex);
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
            state.IsSubmitted = true;
            _frameCount++;
        }
    }
    _cmdQueue->Wait();
    for (auto& i : _renderFrameStates) {
        if (i.IsSubmitted) {
            this->OnRenderComplete(i.InFlightFrameIndex);
        }
        i = RenderFrameState::Invalid();
    }
    for (auto& i : _inFlightFences) {
        i->Reset();
    }
}

void ImGuiApplication::LoopMultiThreaded() {
    _freeFrames = make_unique<BoundedChannel<uint32_t>>(_inFlightFrameCount);
    _submitFrames = make_unique<BoundedChannel<uint32_t>>(_inFlightFrameCount);
    for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
        _freeFrames->TryWrite(i);
    }
    _frameCount = 0;
    _renderFrameStates.resize(_inFlightFrameCount);
    for (auto& i : _renderFrameStates) {
        i = RenderFrameState::Invalid();
    }
    _renderThread = make_unique<std::thread>([this]() {
#ifdef RADRAY_PLATFORM_WINDOWS
        ImGui_ImplWin32_EnableDpiAwareness();
#endif
        while (true) {
            if (_needReLoop) {
                break;
            }
            if (_needClose) {
                break;
            }
            const uint32_t processIndex = static_cast<uint32_t>(_frameCount % _inFlightFrameCount);
            auto& state = _renderFrameStates[processIndex];
            if (state.IsSubmitted) {
                render::Fence* fence = _inFlightFences[processIndex].get();
                fence->Wait();
                this->OnRenderComplete(state.InFlightFrameIndex);
                _freeFrames->WaitWrite(state.InFlightFrameIndex);
                state = RenderFrameState::Invalid();

                _nowGpuTimePoint = _sw.Elapsed().count() * 0.000001;
            }
            if (!state.IsValid()) {
                uint32_t req;
                if (!_submitFrames->WaitRead(req)) {
                    break;
                }
                state.InFlightFrameIndex = req;
            }
            {
                int32_t rtWidth, rtHeight;
                bool needResize;
                {
                    std::unique_lock<std::mutex> lock{_shareMutex};
                    rtWidth = _rtWidth;
                    rtHeight = _rtHeight;
                    needResize = _needRecreate;
                    _needRecreate = false;
                }
                if (rtWidth > 0 && rtHeight > 0) {
                    if (needResize) {
                        this->RecreateSwapChain();
                        this->OnRecreateSwapChain();
                        for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
                            if (_renderFrameStates[i].IsValid()) {
                                this->OnRenderComplete(_renderFrameStates[i].InFlightFrameIndex);
                                _freeFrames->WaitWrite(_renderFrameStates[i].InFlightFrameIndex);
                                _renderFrameStates[i] = RenderFrameState::Invalid();
                            }
                            _inFlightFences[i]->Reset();
                        }
                        continue;
                    }
                } else {
                    if (state.IsValid()) {
                        _freeFrames->WaitWrite(state.InFlightFrameIndex);
                        state = RenderFrameState::Invalid();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    continue;
                }
            }
            {
                render::Semaphore* imageAvailableSemaphore = nullptr;
                if (_device->GetBackend() == render::RenderBackend::Vulkan) {
                    imageAvailableSemaphore = _imageAvailableSemaphores[processIndex].get();
                }
                auto rtOpt = _swapchain->AcquireNext(imageAvailableSemaphore, nullptr);
                if (!rtOpt.HasValue()) {
                    auto [w, h] = _window->GetSize();
                    this->RequestRecreateSwapChain([this, w, h]() {
                        _rtWidth = w;
                        _rtHeight = h;
                    });
                    if (state.IsValid()) {
                        _freeFrames->WaitWrite(state.InFlightFrameIndex);
                        state = RenderFrameState::Invalid();
                    }
                    std::this_thread::yield();
                    continue;
                }
                uint32_t backBufferIndex = _swapchain->GetCurrentBackBufferIndex();
                _backBuffers[backBufferIndex] = rtOpt.Release();
                if (_device->GetBackend() == render::RenderBackend::Metal) {
                    _defaultRTVs[backBufferIndex].reset();
                }
            }
            auto submitCmdBufs = this->OnRender(state.InFlightFrameIndex);
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
                state.IsSubmitted = true;
                _frameCount++;
            }
        }
        _freeFrames->Complete();
    });
    {
        while (true) {
            if (_needReLoop) {
                break;
            }
            _window->DispatchEvents();
            _needClose = _window->ShouldClose();
            if (_needClose) {
                break;
            }
            this->OnUpdate();
            _imgui->SetCurrent();
#ifdef RADRAY_PLATFORM_WINDOWS
            ImGui_ImplWin32_NewFrame();
#endif
#ifdef RADRAY_PLATFORM_MACOS
            ImGuiOSXBridge_NewFrame(_window->GetNativeHandler().Handle);
#endif
            ImGui::NewFrame();
            this->OnImGui();
            ImGui::Render();

            if (_rtWidth > 0 && _rtHeight > 0) {
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
                this->OnExtractDrawData(frameIndex);
                if (!_submitFrames->WaitWrite(frameIndex)) {
                    break;
                }
            }
            _nowCpuTimePoint = _sw.Elapsed().count() * 0.000001;
        }
    }
    _submitFrames->Complete();
    _renderThread->join();
    _submitFrames.reset();
    _freeFrames.reset();
    _renderThread.reset();

    _cmdQueue->Wait();
    for (auto& i : _renderFrameStates) {
        if (i.IsSubmitted) {
            this->OnRenderComplete(i.InFlightFrameIndex);
        }
        i = RenderFrameState::Invalid();
    }
    for (auto& i : _inFlightFences) {
        i->Reset();
    }
}

void ImGuiApplication::OnStart(const ImGuiAppConfig& config) {
    RADRAY_UNUSED(config);
}

void ImGuiApplication::OnDestroy() noexcept {}

void ImGuiApplication::OnUpdate() {}

void ImGuiApplication::OnImGui() {}

void ImGuiApplication::OnExtractDrawData(uint32_t frameIndex) {
    _imguiRenderer->ExtractDrawData(frameIndex, ImGui::GetDrawData());
}

void ImGuiApplication::OnRenderComplete(uint32_t frameIndex) {
    _imguiRenderer->OnRenderComplete(frameIndex);
}

void ImGuiApplication::OnRecreateSwapChain() {}

void ImGuiApplication::OnResizing(int width, int height) {
    this->RequestRecreateSwapChain([this, width, height]() {
        _rtWidth = width;
        _rtHeight = height;
    });
}

void ImGuiApplication::OnResized(int width, int height) {
    this->RequestRecreateSwapChain([this, width, height]() {
        _rtWidth = width;
        _rtHeight = height;
    });
}

ImGuiApplication::SimpleFPSCounter::SimpleFPSCounter(const ImGuiApplication& app, double rate) noexcept
    : _app(app),
      _rate(rate) {}

void ImGuiApplication::SimpleFPSCounter::OnUpdate() {
    _cpuAccum++;
    auto now = _app._nowCpuTimePoint;
    auto last = _cpuLastPoint;
    auto delta = now - last;
    if (delta >= _rate) {
        _cpuAvgTime = delta / _cpuAccum;
        _cpuFps = _cpuAccum / (delta * 0.001);
        _cpuLastPoint = now;
        _cpuAccum = 0;
    }
}

void ImGuiApplication::SimpleFPSCounter::OnRender() {
    _gpuAccum++;
    double now = _app._nowGpuTimePoint;
    auto last = _gpuLastPoint;
    auto delta = now - last;
    if (delta >= _rate) {
        _gpuAvgTime = delta / _gpuAccum;
        _gpuFps = _gpuAccum / (delta * 0.001);
        _gpuLastPoint = now;
        _gpuAccum = 0;
    }
}

double ImGuiApplication::SimpleFPSCounter::GetGPUAverageTime() const noexcept {
    return _gpuAvgTime.load();
}

double ImGuiApplication::SimpleFPSCounter::GetGPUFPS() const noexcept {
    return _gpuFps.load();
}

ImGuiApplication::SimpleMonitorIMGUI::SimpleMonitorIMGUI(ImGuiApplication& app) noexcept
    : _app(app) {}

void ImGuiApplication::SimpleMonitorIMGUI::SetData(double cpuAvgTime, double cpuFps, double gpuAvgTime, double gpuFps) {
    _cpuAvgTime = cpuAvgTime;
    _cpuFps = cpuFps;
    _gpuAvgTime = gpuAvgTime;
    _gpuFps = gpuFps;
}

void ImGuiApplication::SimpleMonitorIMGUI::SetData(const SimpleFPSCounter& counter) {
    this->SetData(counter.GetCPUAverageTime(), counter.GetCPUFPS(), counter.GetGPUAverageTime(), counter.GetGPUFPS());
}

void ImGuiApplication::SimpleMonitorIMGUI::OnImGui() {
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    int location = 0;
    const float PAD = 10.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 workPos = viewport->WorkPos;
    ImVec2 workSize = viewport->WorkSize;
    ImVec2 windowPos, windowPosPivot;
    windowPos.x = (location & 1) ? (workPos.x + workSize.x - PAD) : (workPos.x + PAD);
    windowPos.y = (location & 2) ? (workPos.y + workSize.y - PAD) : (workPos.y + PAD);
    windowPosPivot.x = (location & 1) ? 1.0f : 0.0f;
    windowPosPivot.y = (location & 2) ? 1.0f : 0.0f;
    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, windowPosPivot);
    windowFlags |= ImGuiWindowFlags_NoMove;
    ImGui::SetNextWindowBgAlpha(0.35f);
    if (ImGui::Begin("RadrayMonitor", &_showMonitor, windowFlags)) {
        if (_app._device->GetBackend() == radray::render::RenderBackend::Metal) {
            ImGui::Text("Backend: %s", radray::render::format_as(_app._device->GetBackend()).data());
        } else {
            ImGui::Text("Backend: %s (validate layer %s)", radray::render::format_as(_app._device->GetBackend()).data(), _app._enableValidation ? "On" : "Off");
        }
        ImGui::Text("CPU: (%09.4f ms) (%.2f fps)", _cpuAvgTime, _cpuFps);
        ImGui::Text("GPU: (%09.4f ms) (%.2f fps)", _gpuAvgTime, _gpuFps);
        ImGui::Separator();
        auto nowModeName = radray::render::format_as(_app._presentMode);
        const radray::render::PresentMode modes[] = {
            radray::render::PresentMode::FIFO,
            radray::render::PresentMode::Mailbox,
            radray::render::PresentMode::Immediate};
        if (ImGui::BeginCombo("Present Mode", nowModeName.data())) {
            for (const auto i : modes) {
                const bool isSelected = i == _app._presentMode;
                if (ImGui::Selectable(radray::render::format_as(i).data(), isSelected)) {
                    _app.RequestRecreateSwapChain([this, i]() { _app._presentMode = i; });
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Frame Drop", &_app._enableFrameDropping);
        if (ImGui::Checkbox("Multi Thread", &_app._enableMultiThreading)) {
            _app._needReLoop = true;
        }
    }
    ImGui::End();
}

ImGuiAppConfig ImGuiApplication::ParseArgsSimple(int argc, char** argv) noexcept {
    radray::vector<radray::string> args;
    for (int i = 0; i < argc; i++) {
        args.emplace_back(argv[i]);
    }
    radray::render::RenderBackend backend{radray::render::RenderBackend::Vulkan};
    bool isMultiThread = false, enableValid = false;
    {
        auto bIt = std::find_if(args.begin(), args.end(), [](const radray::string& arg) { return arg == "--backend"; });
        if (bIt != args.end() && (bIt + 1) != args.end()) {
            radray::string backendStr = *(bIt + 1);
            std::transform(backendStr.begin(), backendStr.end(), backendStr.begin(), [](char c) { return std::tolower(c); });
            if (backendStr == "vulkan") {
                backend = radray::render::RenderBackend::Vulkan;
            } else if (backendStr == "d3d12") {
                backend = radray::render::RenderBackend::D3D12;
            } else if (backendStr == "metal") {
                backend = radray::render::RenderBackend::Metal;
            } else {
                RADRAY_WARN_LOG("unsupported backend: {}, using default Vulkan backend.", backendStr);
            }
        }
    }
    {
        auto mtIt = std::find_if(args.begin(), args.end(), [](const radray::string& arg) { return arg == "--multithread"; });
        if (mtIt != args.end()) {
            isMultiThread = true;
        }
    }
    {
        auto validIt = std::find_if(args.begin(), args.end(), [](const radray::string& arg) { return arg == "--valid-layer"; });
        if (validIt != args.end()) {
            enableValid = true;
        }
    }
    return {
        "",
        "",
        1280,
        720,
        backend,
        std::nullopt,
        3,
        2,
        radray::render::TextureFormat::BGRA8_UNORM,
        radray::render::PresentMode::Mailbox,
        isMultiThread,
        false,
        enableValid};
}

}  // namespace radray
