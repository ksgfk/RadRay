#include <radray/imgui/dear_imgui.h>

#include <cstdlib>
#include <bit>

#include <radray/errors.h>
#include <radray/utility.h>
#include <radray/basic_math.h>
#include <radray/stopwatch.h>

#ifdef RADRAY_ENABLE_IMGUI

#ifdef RADRAY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandlerEx(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, ImGuiIO& io);
#endif

namespace radray {

const std::string_view ECradrayimgui = "radrayimgui";

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

bool InitImGui() {
    IMGUI_CHECKVERSION();
#ifdef RADRAY_PLATFORM_WINDOWS
    ImGui_ImplWin32_EnableDpiAwareness();
#endif
    ImGui::SetAllocatorFunctions(_ImguiAllocBridge, _ImguiFreeBridge, nullptr);
    return true;
}

bool InitPlatformImGui(const ImGuiPlatformInitDescriptor& desc) {
#ifdef RADRAY_PLATFORM_WINDOWS
    if (desc.Platform == PlatformId::Windows) {
        WindowNativeHandler wnh = desc.Window->GetNativeHandler();
        if (wnh.Type != WindowHandlerTag::HWND) {
            RADRAY_ERR_LOG("{} {}::{}", ECradrayimgui, "desc", "Window");
            return false;
        }

        auto old = ImGui::GetCurrentContext();
        auto ctxGuard = MakeScopeGuard([old]() { ImGui::SetCurrentContext(old); });
        ImGui::SetCurrentContext(desc.Context);

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        float mainScale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
        style.ScaleAllSizes(mainScale);
        style.FontScaleDpi = mainScale;
        ImGui_ImplWin32_Init(wnh.Handle);
        io.Fonts->AddFontDefault();

        ImGui::SetCurrentContext(old);
        return true;
    }
#endif
    return false;
}

bool InitRendererImGui(ImGuiContext* context) {
    auto old = ImGui::GetCurrentContext();
    auto ctxGuard = MakeScopeGuard([old]() { ImGui::SetCurrentContext(old); });
    ImGui::SetCurrentContext(context);

    IMGUI_CHECKVERSION();
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");
    ImGuiRendererData* bd = IM_NEW(ImGuiRendererData)();
    io.BackendRendererUserData = (void*)bd;
    io.BackendRendererName = "radray_imgui_renderer_impl";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    return true;
}

void TerminateRendererImGui(ImGuiContext* context) {
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    ImGuiRendererData* bd = (ImGuiRendererData*)io.BackendRendererUserData;
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    IM_DELETE(bd);
}

void TerminatePlatformImGui(ImGuiContext* context) {
    ImGui::SetCurrentContext(context);
#ifdef RADRAY_PLATFORM_WINDOWS
    ImGui_ImplWin32_Shutdown();
#endif
}

void SetWin32DpiAwarenessImGui() {
#ifdef RADRAY_PLATFORM_WINDOWS
    ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
}

Nullable<Win32WNDPROC*> GetImGuiWin32WNDPROC() noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    return ImGui_ImplWin32_WndProcHandler;
#else
    return nullptr;
#endif
}

Nullable<std::function<Win32WNDPROC>> GetImGuiWin32WNDPROCEx(ImGuiContext* context) noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    auto old = ImGui::GetCurrentContext();
    auto ctxGuard = MakeScopeGuard([old]() { ImGui::SetCurrentContext(old); });
    ImGui::SetCurrentContext(context);
    ImGuiIO* io = &ImGui::GetIO();
    return [io](HWND hwnd, uint32_t uMsg, uint64_t wParam, int64_t lParam) -> LRESULT {
        return ImGui_ImplWin32_WndProcHandlerEx(hwnd, uMsg, wParam, lParam, *io);
    };
#else
    return nullptr;
#endif
}

Nullable<unique_ptr<ImGuiDrawContext>> CreateImGuiDrawContext(const ImGuiDrawDescriptor& desc) noexcept {
    using namespace ::radray::render;

    auto device = desc.Device;
    RenderBackend backendType = device->GetBackend();
    shared_ptr<Shader> shaderVS;
    shared_ptr<Shader> shaderPS;
    if (backendType == RenderBackend::D3D12) {
        ShaderDescriptor descVS{};
        descVS.Source = GetImGuiShaderDXIL_VS();
        descVS.Category = ShaderBlobCategory::DXIL;
        shaderVS = device->CreateShader(descVS).Unwrap();
        ShaderDescriptor descPS{};
        descPS.Source = GetImGuiShaderDXIL_PS();
        descPS.Category = ShaderBlobCategory::DXIL;
        shaderPS = device->CreateShader(descPS).Unwrap();
    } else if (backendType == RenderBackend::Vulkan) {
        ShaderDescriptor descVS{};
        descVS.Source = GetImGuiShaderSPIRV_VS();
        descVS.Category = ShaderBlobCategory::SPIRV;
        shaderVS = device->CreateShader(descVS).Unwrap();
        ShaderDescriptor descPS{};
        descPS.Source = GetImGuiShaderSPIRV_PS();
        descPS.Category = ShaderBlobCategory::SPIRV;
        shaderPS = device->CreateShader(descPS).Unwrap();
    } else {
        RADRAY_ERR_LOG("{} {} {}", ECradrayimgui, ECUnsupportedPlatform, backendType);
        return nullptr;
    }

    SamplerDescriptor sampler{};
    sampler.AddressS = AddressMode::ClampToEdge;
    sampler.AddressT = AddressMode::ClampToEdge;
    sampler.AddressR = AddressMode::ClampToEdge;
    sampler.MigFilter = FilterMode::Linear;
    sampler.MagFilter = FilterMode::Linear;
    sampler.MipmapFilter = FilterMode::Linear;
    sampler.LodMin = 0.0f;
    sampler.LodMax = std::numeric_limits<float>::max();
    sampler.Compare = CompareFunction::Always;
    sampler.AnisotropyClamp = 0.0f;
    RootSignatureSetElement rsElems[2];
    RootSignatureSetElement& rsTex = rsElems[0];
    rsTex.Slot = 0;
    rsTex.Space = 0;
    rsTex.Type = ResourceBindType::Texture;
    rsTex.Count = 1;
    rsTex.Stages = ShaderStage::Pixel;
    RootSignatureSetElement& rsSampler = rsElems[1];
    if (backendType == RenderBackend::D3D12) {
        rsSampler.Slot = 0;
    } else if (backendType == RenderBackend::Vulkan) {
        rsSampler.Slot = 1;
    }
    rsSampler.Space = 0;
    rsSampler.Type = ResourceBindType::Sampler;
    rsSampler.Count = 1;
    rsSampler.Stages = ShaderStage::Pixel;
    rsSampler.StaticSamplers = std::span(&sampler, 1);
    RootSignatureBindingSet rsSet;
    rsSet.Elements = rsElems;
    auto layout = device->CreateDescriptorSetLayout(rsSet).Unwrap();

    RootSignatureConstant rsConst{};
    rsConst.Slot = 0;
    rsConst.Space = 0;
    rsConst.Size = 64;
    rsConst.Stages = ShaderStage::Vertex;
    DescriptorSetLayout* layouts = layout.get();
    RootSignatureDescriptor rsDesc{};
    rsDesc.Constant = rsConst;
    rsDesc.BindingSets = std::span(&layouts, 1);
    auto rs = device->CreateRootSignature(rsDesc).Unwrap();

    VertexElement vertexElems[3];
    VertexElement& posElem = vertexElems[0];
    posElem.Offset = offsetof(ImDrawVert, pos);
    posElem.Semantic = "POSITION";
    posElem.SemanticIndex = 0;
    posElem.Format = VertexFormat::FLOAT32X2;
    posElem.Location = 0;
    VertexElement& uvElem = vertexElems[1];
    uvElem.Offset = offsetof(ImDrawVert, uv);
    uvElem.Semantic = "TEXCOORD";
    uvElem.SemanticIndex = 0;
    uvElem.Format = VertexFormat::FLOAT32X2;
    uvElem.Location = 2;
    VertexElement& colorElem = vertexElems[2];
    colorElem.Offset = offsetof(ImDrawVert, col);
    colorElem.Semantic = "COLOR";
    colorElem.SemanticIndex = 0;
    colorElem.Format = VertexFormat::UNORM8X4;
    colorElem.Location = 1;
    VertexBufferLayout vbLayout{};
    vbLayout.ArrayStride = sizeof(ImDrawVert);
    vbLayout.StepMode = VertexStepMode::Vertex;
    vbLayout.Elements = vertexElems;
    PrimitiveState primState{};
    primState.Topology = PrimitiveTopology::TriangleList;
    primState.FaceClockwise = FrontFace::CW;
    primState.Cull = CullMode::None;
    primState.Poly = PolygonMode::Fill;
    primState.StripIndexFormat = std::nullopt;
    primState.UnclippedDepth = false;
    primState.Conservative = false;
    MultiSampleState msState{};
    msState.Count = 1;
    msState.Mask = std::numeric_limits<uint32_t>::max();
    msState.AlphaToCoverageEnable = false;
    BlendState rtBlendState{};
    rtBlendState.Color.Src = BlendFactor::SrcAlpha;
    rtBlendState.Color.Dst = BlendFactor::OneMinusSrcAlpha;
    rtBlendState.Color.Op = BlendOperation::Add;
    rtBlendState.Alpha.Src = BlendFactor::One;
    rtBlendState.Alpha.Dst = BlendFactor::OneMinusSrcAlpha;
    rtBlendState.Alpha.Op = BlendOperation::Add;
    ColorTargetState rtState{};
    rtState.Format = desc.RTFormat;
    rtState.Blend = rtBlendState;
    rtState.WriteMask = ColorWrite::All;
    GraphicsPipelineStateDescriptor psoDesc{};
    psoDesc.RootSig = rs.get();
    psoDesc.VS = ShaderEntry{shaderVS.get(), "VSMain"};
    psoDesc.PS = ShaderEntry{shaderPS.get(), "PSMain"};
    psoDesc.VertexLayouts = std::span(&vbLayout, 1);
    psoDesc.Primitive = primState;
    psoDesc.DepthStencil = std::nullopt;
    psoDesc.MultiSample = msState;
    psoDesc.ColorTargets = std::span(&rtState, 1);
    auto pso = device->CreateGraphicsPipelineState(psoDesc).Unwrap();

    auto result = make_unique<ImGuiDrawContext>();
    result->_device = device;
    result->_rsLayout = layout;
    result->_rs = rs;
    result->_pso = pso;
    result->_desc = desc;
    result->_frames.reserve(desc.FrameCount);
    for (int i = 0; i < desc.FrameCount; i++) {
        result->_frames.emplace_back(ImGuiDrawContext::Frame{});
    }
    return result;
}

ImGuiDrawTexture::ImGuiDrawTexture(
    shared_ptr<render::Texture> tex,
    shared_ptr<render::TextureView> srv) noexcept
    : _tex(std::move(tex)),
      _srv(std::move(srv)) {}

ImGuiDrawTexture::~ImGuiDrawTexture() noexcept {
    _srv.reset();
    _tex.reset();
}

void ImGuiDrawContext::ExtractDrawData(int frameIndex, ImDrawData* drawData) {
    using namespace ::radray::render;

    if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        return;
    }
    ImGuiDrawContext::Frame& frame = _frames[frameIndex];

    frame._drawData.Clear();

    if (drawData->Textures != nullptr) {
        for (ImTextureData* tex : *drawData->Textures) {
            if (tex->Status != ImTextureStatus_OK) {
                this->ExtractTexture(frameIndex, tex);
            }
        }
    }
    if (frame._vb == nullptr || frame._vbSize < drawData->TotalVtxCount) {
        int vertCount = drawData->TotalVtxCount + 5000;
        BufferDescriptor desc{};
        desc.Size = vertCount * sizeof(ImDrawVert);
        desc.Memory = MemoryType::Upload;
        desc.Usage = BufferUse::Vertex | BufferUse::CopySource;
        desc.Hints = ResourceHint::Dedicated;
        string vbName = ::radray::format("imgui_vb_{}", frameIndex);
        desc.Name = vbName;
        frame._vb = _device->CreateBuffer(desc).Unwrap();
        frame._vbSize = vertCount;
    }
    if (frame._ib == nullptr || frame._ibSize < drawData->TotalIdxCount) {
        int idxCount = drawData->TotalIdxCount + 10000;
        BufferDescriptor desc{};
        desc.Size = idxCount * sizeof(ImDrawIdx);
        desc.Memory = MemoryType::Upload;
        desc.Usage = BufferUse::Index | BufferUse::CopySource;
        desc.Hints = ResourceHint::Dedicated;
        string ibName = ::radray::format("imgui_ib_{}", frameIndex);
        desc.Name = ibName;
        frame._ib = _device->CreateBuffer(desc).Unwrap();
        frame._ibSize = idxCount;
    }
    {
        void* dst = frame._vb->Map(0, drawData->TotalVtxCount * sizeof(ImDrawVert));
        for (const ImDrawList* drawList : drawData->CmdLists) {
            std::memcpy(dst, drawList->VtxBuffer.Data, drawList->VtxBuffer.Size * sizeof(ImDrawVert));
            dst = static_cast<ImDrawVert*>(dst) + drawList->VtxBuffer.Size;
        }
        frame._vb->Unmap(0, drawData->TotalVtxCount * sizeof(ImDrawVert));
    }
    {
        void* dst = frame._ib->Map(0, drawData->TotalIdxCount * sizeof(ImDrawIdx));
        for (const ImDrawList* drawList : drawData->CmdLists) {
            std::memcpy(dst, drawList->IdxBuffer.Data, drawList->IdxBuffer.Size * sizeof(ImDrawIdx));
            dst = static_cast<ImDrawIdx*>(dst) + drawList->IdxBuffer.Size;
        }
        frame._ib->Unmap(0, drawData->TotalIdxCount * sizeof(ImDrawIdx));
    }

    {
        frame._drawData.DisplayPos = drawData->DisplayPos;
        frame._drawData.DisplaySize = drawData->DisplaySize;
        frame._drawData.FramebufferScale = drawData->FramebufferScale;
        frame._drawData.TotalVtxCount = drawData->TotalVtxCount;
        frame._drawData.CmdLists.reserve(drawData->CmdLists.Size);
        for (const ImDrawList* drawList : drawData->CmdLists) {
            ImGuiDrawContext::DrawList& dl = frame._drawData.CmdLists.emplace_back();
            dl.VtxBufferSize = drawList->VtxBuffer.Size;
            dl.IdxBufferSize = drawList->IdxBuffer.Size;
            dl.CmdBuffer.reserve(drawList->CmdBuffer.Size);
            for (const ImDrawCmd& cmd : drawList->CmdBuffer) {
                ImGuiDrawContext::DrawCmd& dc = dl.CmdBuffer.emplace_back();
                dc.ClipRect = cmd.ClipRect;
                dc.TexRef = cmd.TexRef;
                dc.VtxOffset = cmd.VtxOffset;
                dc.IdxOffset = cmd.IdxOffset;
                dc.ElemCount = cmd.ElemCount;
                dc.UserCallback = cmd.UserCallback;
            }
        }
    }
}

void ImGuiDrawContext::ExtractTexture(int frameIndex, ImTextureData* tex) {
    using namespace ::radray::render;

    ImGuiDrawContext::Frame& frame = _frames[frameIndex];

    if (tex->Status == ImTextureStatus_WantCreate) {
        IM_ASSERT(tex->TexID == ImTextureID_Invalid && tex->BackendUserData == nullptr);
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);

        string texName = format("imgui_tex_{}", tex->UniqueID);
        TextureDescriptor texDesc{};
        texDesc.Dim = TextureDimension::Dim2D;
        texDesc.Width = tex->Width;
        texDesc.Height = tex->Height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = TextureFormat::RGBA8_UNORM;
        texDesc.Usage = TextureUse::Resource | TextureUse::CopyDestination;
        texDesc.Name = texName;
        shared_ptr<Texture> texObj = _device->CreateTexture(texDesc).Unwrap();

        TextureViewDescriptor viewDesc{};
        viewDesc.Target = texObj.get();
        viewDesc.Dim = TextureViewDimension::Dim2D;
        viewDesc.Format = TextureFormat::RGBA8_UNORM;
        viewDesc.Range = SubresourceRange::AllSub();
        viewDesc.Usage = TextureUse::Resource;
        shared_ptr<TextureView> srv = _device->CreateTextureView(viewDesc).Unwrap();

        auto texId = std::bit_cast<ImU64>(srv.get());
        tex->SetTexID(texId);
        tex->BackendUserData = texObj.get();

        auto key = texObj.get();
        _texs.emplace(key, make_unique<ImGuiDrawTexture>(std::move(texObj), std::move(srv)));
    }

    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
        auto texObjPtr = std::bit_cast<Texture*>(tex->BackendUserData);
        IM_ASSERT(_texs.find(texObjPtr) != _texs.end());
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);

        const int upload_w = tex->Width;
        const int upload_h = tex->Height;
        int upload_pitch_src = upload_w * tex->BytesPerPixel;
        int upload_size = upload_pitch_src * upload_h;

        string uploadName = format("imgui_tex_upload_{}", tex->UniqueID);
        BufferDescriptor uploadDesc{};
        uploadDesc.Size = Align(upload_size, _device->GetDetail().UploadTextureAlignment);
        uploadDesc.Memory = MemoryType::Upload;
        uploadDesc.Usage = BufferUse::CopySource;
        uploadDesc.Name = uploadName;
        shared_ptr<Buffer> uploadBuffer = _device->CreateBuffer(uploadDesc).Unwrap();
        {
            void* dst = uploadBuffer->Map(0, upload_size);
            std::memcpy(dst, tex->GetPixels(), upload_size);
            uploadBuffer->Unmap(0, upload_size);
        }
        frame._tempUploadBuffers.emplace_back(uploadBuffer);
        frame._needCopyTexs.emplace_back(UploadTexturePayload{texObjPtr, uploadBuffer.get(), tex->Status == ImTextureStatus_WantCreate});
        tex->SetStatus(ImTextureStatus_OK);
    }
    if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames >= _desc.FrameCount) {
        auto texObjPtr = std::bit_cast<Texture*>(tex->BackendUserData);
        frame._waitDestroyTexs.emplace_back(texObjPtr);
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
        tex->BackendUserData = nullptr;
    }
}

void ImGuiDrawContext::BeforeDraw(int frameIndex, render::CommandBuffer* cmdBuffer) {
    using namespace ::radray::render;

    ImGuiDrawContext::Frame& frame = _frames[frameIndex];

    frame._usableDescSetIndex = 0;

    if (frame._needCopyTexs.size() > 0) {
        vector<BarrierTextureDescriptor> barriers;
        barriers.reserve(frame._needCopyTexs.size());
        for (const ImGuiDrawContext::UploadTexturePayload& payload : frame._needCopyTexs) {
            BarrierTextureDescriptor& barrierBefore = barriers.emplace_back();
            barrierBefore.Target = payload._tex;
            if (payload._isNew) {
                barrierBefore.Before = TextureUse::Uninitialized;
            } else {
                barrierBefore.Before = TextureUse::Resource;
            }
            barrierBefore.After = TextureUse::CopyDestination;
            barrierBefore.IsFromOrToOtherQueue = false;
            barrierBefore.IsSubresourceBarrier = false;
        }
        cmdBuffer->ResourceBarrier({}, barriers);
        for (const ImGuiDrawContext::UploadTexturePayload& payload : frame._needCopyTexs) {
            SubresourceRange range{};
            range.BaseArrayLayer = 0;
            range.ArrayLayerCount = 1;
            range.BaseMipLevel = 0;
            range.MipLevelCount = 1;
            cmdBuffer->CopyBufferToTexture(payload._tex, range, payload._upload, 0);
        }
        barriers.clear();
        for (const ImGuiDrawContext::UploadTexturePayload& payload : frame._needCopyTexs) {
            BarrierTextureDescriptor& barrierBefore = barriers.emplace_back();
            barrierBefore.Target = payload._tex;
            barrierBefore.Before = TextureUse::CopyDestination;
            barrierBefore.After = TextureUse::Resource;
            barrierBefore.IsFromOrToOtherQueue = false;
            barrierBefore.IsSubresourceBarrier = false;
        }
        cmdBuffer->ResourceBarrier({}, barriers);
    }
}

void ImGuiDrawContext::Draw(int frameIndex, render::CommandEncoder* encoder) {
    using namespace ::radray::render;

    ImGuiDrawContext::Frame& frame = _frames[frameIndex];
    const ImGuiDrawContext::DrawData* drawData = &frame._drawData;

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
    TextureView* lastBindTex = nullptr;
    for (const ImGuiDrawContext::DrawList& dl : drawData->CmdLists) {
        const ImGuiDrawContext::DrawList* draw_list = &dl;
        for (size_t cmdi = 0; cmdi < draw_list->CmdBuffer.size(); cmdi++) {
            const ImGuiDrawContext::DrawCmd* pcmd = &draw_list->CmdBuffer[cmdi];
            if (pcmd->UserCallback != nullptr) {
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState) {
                    this->SetupRenderState(frameIndex, encoder, fbWidth, fbHeight);
                } else {
                    // pcmd->UserCallback(draw_list, pcmd);
                    RADRAY_ABORT("{} {} '{}'", ECradrayimgui, ECInvalidArgument, "UserCallback");
                }
            } else {
                ImVec2 clipMin((pcmd->ClipRect.x - clipOff.x) * clipScale.x, (pcmd->ClipRect.y - clipOff.y) * clipScale.y);
                ImVec2 clipMax((pcmd->ClipRect.z - clipOff.x) * clipScale.x, (pcmd->ClipRect.w - clipOff.y) * clipScale.y);
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
                auto texView = std::bit_cast<TextureView*>(pcmd->GetTexID());
                if (lastBindTex != texView) {
                    DescriptorSet* descSetRaw = nullptr;
                    auto it = _descSetCache.find(texView);
                    if (it == _descSetCache.end()) {
                        shared_ptr<DescriptorSet> descSet = _device->CreateDescriptorSet(_rsLayout.get()).Unwrap();
                        descSet->SetResource(0, 0, texView);
                        descSetRaw = descSet.get();
                        _descSetCache.emplace(texView, std::move(descSet));
                    } else {
                        descSetRaw = it->second.get();
                    }
                    encoder->BindDescriptorSet(0, descSetRaw);
                    lastBindTex = texView;
                }
                encoder->DrawIndexed(pcmd->ElemCount, 1, pcmd->IdxOffset + globalIdxOffset, pcmd->VtxOffset + globalVtxOffset, 0);
            }
        }
        globalIdxOffset += draw_list->IdxBufferSize;
        globalVtxOffset += draw_list->VtxBufferSize;
    }
    {
        Rect scissor{};
        scissor.X = 0;
        scissor.Y = 0;
        scissor.Width = fbWidth;
        scissor.Height = fbHeight;
        encoder->SetScissor(scissor);
    }
}

void ImGuiDrawContext::AfterDraw(int frameIndex) {
    using namespace ::radray::render;

    ImGuiDrawContext::Frame& frame = _frames[frameIndex];

    for (Texture* texToDestroy : frame._waitDestroyTexs) {
        auto it = _texs.find(texToDestroy);
        if (it != _texs.end()) {
            TextureView* viewPtr = it->second->_srv.get();
            auto itSet = _descSetCache.find(viewPtr);
            if (itSet != _descSetCache.end()) {
                _descSetCache.erase(itSet);
            }
            _texs.erase(it);
        }
    }
    frame._waitDestroyTexs.clear();
    frame._needCopyTexs.clear();
    frame._tempUploadBuffers.clear();
}

void ImGuiDrawContext::SetupRenderState(int frameIndex, render::CommandEncoder* encoder, int fbWidth, int fbHeight) {
    using namespace ::radray::render;

    ImGuiDrawContext::Frame& frame = _frames[frameIndex];
    const ImGuiDrawContext::DrawData* drawData = &frame._drawData;

    encoder->BindRootSignature(_rs.get());
    encoder->BindGraphicsPipelineState(_pso.get());
    if (drawData->TotalVtxCount > 0) {
        VertexBufferView vbv{};
        vbv.Target = frame._vb.get();
        vbv.Offset = 0;
        vbv.Size = frame._vbSize * sizeof(ImDrawVert);
        encoder->BindVertexBuffer(std::span{&vbv, 1});
        IndexBufferView ibv{};
        ibv.Target = frame._ib.get();
        ibv.Offset = 0;
        ibv.Stride = sizeof(ImDrawIdx);
        encoder->BindIndexBuffer(ibv);
    }
    Viewport vp{};
    vp.X = 0.0f;
    vp.Y = 0.0f;
    vp.Width = (float)fbWidth;
    vp.Height = (float)fbHeight;
    if (_device->GetBackend() == RenderBackend::Vulkan) {
        vp.Y = (float)fbHeight;
        vp.Height = -(float)fbHeight;
    }
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    encoder->SetViewport(vp);
    float L = drawData->DisplayPos.x;
    float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
    float T = drawData->DisplayPos.y;
    float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
    Eigen::Matrix4f proj = ::radray::OrthoLH(L, R, B, T, 0.0f, 1.0f);
    encoder->PushConstant(proj.data(), sizeof(proj));
}

ImTextureID ImGuiDrawContext::DrawCmd::GetTexID() const noexcept {
    ImTextureID tex_id = TexRef._TexData ? TexRef._TexData->TexID : TexRef._TexID;
    if (TexRef._TexData != NULL) {
        IM_ASSERT(tex_id != ImTextureID_Invalid);
    }
    return tex_id;
}

void ImGuiDrawContext::DrawData::Clear() noexcept {
    CmdLists.clear();
}

ImGuiApplication::Frame::Frame(size_t index, shared_ptr<render::CommandBuffer> cmdBuffer) noexcept
    : _frameIndex(index),
      _cmdBuffer(std::move(cmdBuffer)) {}

ImGuiApplication::ImGuiApplication() noexcept = default;

ImGuiApplication::~ImGuiApplication() noexcept = default;

void ImGuiApplication::Destroy() noexcept {
    _resizingConn.disconnect();
    _resizedConn.disconnect();
    _cmdQueue->Wait();
    this->OnDestroy();
    _cmdQueue = nullptr;
    ::radray::TerminateRendererImGui(_imguiContext->Get());
    ::radray::TerminatePlatformImGui(_imguiContext->Get());
    _imguiDrawContext.reset();
    _frames.clear();
    _rtViews.clear();
    _swapchain.reset();
    _device.reset();
    _vkIns.reset();
    _window.reset();
    _win32ImguiProc.reset();
    _imguiContext.reset();
}

void ImGuiApplication::NewSwapChain() {
    _swapchain.reset();
    render::SwapChainDescriptor swapchainDesc{};
    swapchainDesc.PresentQueue = _cmdQueue;
    swapchainDesc.NativeHandler = _window->GetNativeHandler().Handle;
    swapchainDesc.Width = static_cast<uint32_t>(_renderRtSize.x());
    swapchainDesc.Height = static_cast<uint32_t>(_renderRtSize.y());
    swapchainDesc.BackBufferCount = _frameCount;
    swapchainDesc.Format = _rtFormat;
    swapchainDesc.EnableSync = _enableVSync;
    _swapchain = _device->CreateSwapChain(swapchainDesc).Unwrap();
}

void ImGuiApplication::RecreateSwapChain() {
    _cmdQueue->Wait();
    _isRenderAcquiredRt = false;
    _rtViews.clear();
    _currRenderFrame = 0;
    while (_freeFrame->Size() > 0) {
        size_t _;
        _freeFrame->TryRead(_);
    }
    while (_waitFrame->Size() > 0) {
        size_t _;
        _waitFrame->TryRead(_);
    }
    for (auto& frame : _frames) {
        frame->_rt = nullptr;
        frame->_rtView = nullptr;
        frame->_completeFrame = std::numeric_limits<uint64_t>::max();
    }
    this->NewSwapChain();
    for (size_t i = 0; i < _frames.size(); i++) {
        _freeFrame->TryWrite(i);
    }
}

void ImGuiApplication::Run() {
    this->OnStart();
    if (_multithreadRender) {
        this->RunMultiThreadRender();
    } else {
        this->RunSingleThreadRender();
    }
}

void ImGuiApplication::Init(const ImGuiApplicationDescriptor& desc_) {
    _frameCount = desc_.FrameCount;
    _rtFormat = desc_.RTFormat;
    _enableVSync = desc_.EnableVSync;
    _isWaitFrame = desc_.IsWaitFrame;
    _multithreadRender = desc_.IsRenderMultiThread;
    _freeFrame = make_unique<BoundedChannel<size_t>>(desc_.FrameCount);
    _waitFrame = make_unique<BoundedChannel<size_t>>(desc_.FrameCount);

    ImGuiApplicationDescriptor appDesc = desc_;
    if (!appDesc.IsRenderMultiThread) {
        if (appDesc.IsWaitFrame) {
            RADRAY_WARN_LOG("{}: IsWaitFrame is only effective when IsRenderMultiThread is true.", ECradrayimgui);
        }
        appDesc.IsWaitFrame = false;
    }

    _imguiContext = radray::make_unique<radray::ImGuiContextRAII>();

    PlatformId platform = ::radray::GetPlatform();
    if (platform == PlatformId::Windows) {
        _win32ImguiProc = make_shared<std::function<::radray::Win32WNDPROC>>(::radray::GetImGuiWin32WNDPROCEx(_imguiContext->Get()).Release());
        weak_ptr<std::function<::radray::Win32WNDPROC>> weakImguiProc = _win32ImguiProc;
        Win32WindowCreateDescriptor desc{};
        desc.Title = appDesc.AppName;
        desc.Width = appDesc.WindowSize.x();
        desc.Height = appDesc.WindowSize.y();
        desc.X = -1;
        desc.Y = -1;
        desc.Resizable = appDesc.Resizeable;
        desc.StartMaximized = false;
        desc.Fullscreen = appDesc.IsFullscreen;
        desc.ExtraWndProcs = std::span{&weakImguiProc, 1};
        auto windowOpt = ::radray::CreateNativeWindow(desc);
        _window = windowOpt.Release();
        ImGuiPlatformInitDescriptor imguiDesc{};
        imguiDesc.Platform = PlatformId::Windows;
        imguiDesc.Window = _window.get();
        imguiDesc.Context = _imguiContext->Get();
        ::radray::InitPlatformImGui(imguiDesc);
        ::radray::InitRendererImGui(imguiDesc.Context);
    }
    if (!_window) {
        throw ImGuiApplicationException("{}: {}", ECradrayimgui, "fail create window");
    }
    _resizingConn = _window->EventResizing().connect(&ImGuiApplication::OnResizing, this);
    _resizedConn = _window->EventResized().connect(&ImGuiApplication::OnResized, this);

    if (platform == PlatformId::Windows && appDesc.Backend == render::RenderBackend::D3D12) {
        render::D3D12DeviceDescriptor desc{};
        if (appDesc.DeviceDesc.has_value()) {
            desc = std::get<render::D3D12DeviceDescriptor>(appDesc.DeviceDesc.value());
        } else {
            desc.IsEnableDebugLayer = appDesc.EnableValidation;
            desc.IsEnableGpuBasedValid = appDesc.EnableValidation;
        }
        _device = CreateDevice(desc).Release();
    } else if (appDesc.Backend == render::RenderBackend::Vulkan) {
        render::VulkanInstanceDescriptor insDesc{};
        insDesc.AppName = appDesc.AppName;
        insDesc.AppVersion = 1;
        insDesc.EngineName = "RadRay";
        insDesc.EngineVersion = 1;
        insDesc.IsEnableDebugLayer = appDesc.EnableValidation;
        _vkIns = CreateVulkanInstance(insDesc).Unwrap();
        render::VulkanCommandQueueDescriptor queueDesc[] = {
            {render::QueueType::Direct, 1}};
        render::VulkanDeviceDescriptor devDesc{};
        if (appDesc.DeviceDesc.has_value()) {
            devDesc = std::get<render::VulkanDeviceDescriptor>(appDesc.DeviceDesc.value());
        } else {
            devDesc.Queues = queueDesc;
        }
        _device = CreateDevice(devDesc).Release();
    } else {
        throw ImGuiApplicationException("{}: {}", ECradrayimgui, ECUnsupportedPlatform);
    }
    if (!_device) {
        throw ImGuiApplicationException("{}: {}", ECradrayimgui, "fail create device");
    }

    _cmdQueue = _device->GetCommandQueue(render::QueueType::Direct, 0).Release();
    _renderRtSize = appDesc.WindowSize;
    this->NewSwapChain();
    _frames.reserve(_swapchain->GetBackBufferCount());
    for (size_t i = 0; i < _swapchain->GetBackBufferCount(); ++i) {
        auto cmdBufferOpt = _device->CreateCommandBuffer(_cmdQueue);
        if (!cmdBufferOpt.HasValue()) {
            throw ImGuiApplicationException("{}: {}", ECradrayimgui, "failed create cmdBuffer");
        }
        _frames.emplace_back(make_unique<ImGuiApplication::Frame>(i, cmdBufferOpt.Release()));
    }
    _currRenderFrame = 0;
    {
        ImGuiDrawDescriptor imguiDrawDesc{};
        imguiDrawDesc.Device = _device.get();
        imguiDrawDesc.RTFormat = appDesc.RTFormat;
        imguiDrawDesc.FrameCount = static_cast<int>(_frames.size());
        auto ctxOpt = ::radray::CreateImGuiDrawContext(imguiDrawDesc);
        if (!ctxOpt.HasValue()) {
            throw ImGuiApplicationException("{}: {}", ECradrayimgui, "failed create ImGuiDrawContext");
        }
        _imguiDrawContext = ctxOpt.Release();
    }
    for (size_t i = 0; i < _frames.size(); i++) {
        _freeFrame->TryWrite(i);
    }
}

void ImGuiApplication::RunMultiThreadRender() {
    _renderThread = radray::make_unique<std::thread>(&ImGuiApplication::RenderUpdateMultiThread, this);
    while (true) {
        this->MainUpdate();
        if (_needClose) {
            break;
        }
    }
    _freeFrame->Complete();
    _waitFrame->Complete();
    _renderThread->join();
}

void ImGuiApplication::RunSingleThreadRender() {
    Stopwatch sw{};
    sw.Start();

    while (true) {
        this->MainUpdate();
        if (_needClose) {
            break;
        }

        this->ExecuteBeforeAcquire();
        if (_isResizingRender) {
            continue;
        }
        if (_renderRtSize.x() <= 0 || _renderRtSize.y() <= 0) {
            continue;
        }
        radray::render::Texture* rt = nullptr;
        if (_isRenderAcquiredRt) {
            rt = _swapchain->GetCurrentBackBuffer().Release();
        } else {
            sw.Stop();
            _renderTime = sw.ElapsedNanoseconds() / 1'000'000.0;
            sw.Start();
            radray::Nullable<radray::render::Texture*> rtOpt = _swapchain->AcquireNext();
            if (!rtOpt.HasValue()) {
                continue;
            }
            rt = rtOpt.Get();
            _isRenderAcquiredRt = true;
        }
        if (_needClose) {
            break;
        }
        {
            size_t completeFrameIndex = _currRenderFrame % _frames.size();
            if (_frames[completeFrameIndex]->_completeFrame == _currRenderFrame) {
                _imguiDrawContext->AfterDraw(completeFrameIndex);
                _frames[completeFrameIndex]->_completeFrame = std::numeric_limits<uint64_t>::max();
                if (!_freeFrame->WaitWrite(completeFrameIndex)) {
                    break;
                }
            }
        }
        size_t frameIndex = std::numeric_limits<size_t>::max();
        if (!_waitFrame->TryRead(frameIndex)) {
            continue;
        }
        _isRenderAcquiredRt = false;
        ImGuiApplication::Frame* frame = _frames[frameIndex].get();
        frame->_completeFrame = _currRenderFrame + _frames.size();
        frame->_rt = rt;
        frame->_rtView = this->SafeGetRTView(frame->_rt);
        this->OnRender(frame);
        _swapchain->Present();
        _currRenderFrame++;

        sw.Stop();
    }
}

void ImGuiApplication::MainUpdate() {
    Stopwatch sw{};
    sw.Start();

    _imguiContext->SetCurrent();
    _window->DispatchEvents();
    if (_window->ShouldClose()) {
        _needClose = true;
        return;
    }

    this->OnUpdate();

    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    this->OnImGui();
    ImGui::Render();

    Nullable<ImGuiApplication::Frame*> frameOpt = this->GetAvailableFrame();
    if (frameOpt.HasValue()) {
        ImGuiApplication::Frame* frame = frameOpt.Get();
        ImDrawData* drawData = ImGui::GetDrawData();
        _imguiDrawContext->ExtractDrawData((int)frame->_frameIndex, drawData);
        _waitFrame->WaitWrite(frame->_frameIndex);
    }

    sw.Stop();
    _logicTime = sw.ElapsedNanoseconds() / 1'000'000.0;
}

void ImGuiApplication::RenderUpdateMultiThread() {
    ::radray::SetWin32DpiAwarenessImGui();
    Stopwatch sw{};
    sw.Start();
    while (true) {
        this->ExecuteBeforeAcquire();
        if (_isResizingRender) {
            std::this_thread::yield();
            continue;
        }
        if (_renderRtSize.x() <= 0 || _renderRtSize.y() <= 0) {
            std::this_thread::yield();
            continue;
        }
        sw.Stop();
        _renderTime = sw.ElapsedNanoseconds() / 1'000'000.0;
        sw.Start();
        radray::Nullable<radray::render::Texture*> rtOpt = _swapchain->AcquireNext();
        if (_needClose) {
            return;
        }
        if (!rtOpt.HasValue()) {
            continue;
        }
        {
            size_t completeFrameIndex = _currRenderFrame % _frames.size();
            if (_frames[completeFrameIndex]->_completeFrame == _currRenderFrame) {
                _imguiDrawContext->AfterDraw(completeFrameIndex);
                _frames[completeFrameIndex]->_completeFrame = std::numeric_limits<uint64_t>::max();
                if (!_freeFrame->WaitWrite(completeFrameIndex)) {
                    return;
                }
            }
        }
        size_t frameIndex = std::numeric_limits<size_t>::max();
        if (!_waitFrame->WaitRead(frameIndex)) {
            return;
        }
        ImGuiApplication::Frame* frame = _frames[frameIndex].get();
        frame->_completeFrame = _currRenderFrame + _frames.size();
        frame->_rt = rtOpt.Get();
        frame->_rtView = this->SafeGetRTView(frame->_rt);
        this->OnRender(frame);
        _swapchain->Present();
        _currRenderFrame++;
    }
}

render::TextureView* ImGuiApplication::SafeGetRTView(render::Texture* rt) {
    auto it = _rtViews.find(rt);
    if (it == _rtViews.end()) {
        render::TextureViewDescriptor rtViewDesc{};
        rtViewDesc.Target = rt;
        rtViewDesc.Dim = render::TextureViewDimension::Dim2D;
        rtViewDesc.Format = _rtFormat;
        rtViewDesc.Range = render::SubresourceRange::AllSub();
        rtViewDesc.Usage = render::TextureUse::RenderTarget;
        auto rtView = _device->CreateTextureView(rtViewDesc).Unwrap();
        it = _rtViews.emplace(rt, rtView).first;
    }
    return it->second.get();
}

Nullable<ImGuiApplication::Frame*> ImGuiApplication::GetAvailableFrame() {
    if (_isWaitFrame) {
        size_t frameIndex;
        if (_freeFrame->WaitRead(frameIndex)) {
            return _frames[frameIndex].get();
        } else {
            return nullptr;
        }
    } else {
        size_t frameIndex;
        if (_freeFrame->TryRead(frameIndex)) {
            return _frames[frameIndex].get();
        } else {
            return nullptr;
        }
    }
}

void ImGuiApplication::ExecuteBeforeAcquire() {
    std::function<void(void)> func;
    while (_beforeAcquire.TryRead(func)) {
        func();
    }
}

void ImGuiApplication::OnResizing(int width, int height) {
    RADRAY_UNUSED(width);
    RADRAY_UNUSED(height);
}

void ImGuiApplication::OnResized(int width, int height) {
    RADRAY_UNUSED(width);
    RADRAY_UNUSED(height);
}

void ImGuiApplication::OnStart() {}

void ImGuiApplication::OnUpdate() {}

void ImGuiApplication::OnImGui() {}

void ImGuiApplication::OnRender(ImGuiApplication::Frame* frame) {
    RADRAY_UNUSED(frame);
}

void ImGuiApplication::OnDestroy() noexcept {}

}  // namespace radray

#endif
