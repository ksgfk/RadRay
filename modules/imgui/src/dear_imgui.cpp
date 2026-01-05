#ifdef RADRAY_ENABLE_IMGUI

#include <radray/imgui/dear_imgui.h>

#include <cstdlib>
#include <bit>

#include <radray/errors.h>
#include <radray/utility.h>
#include <radray/basic_math.h>
#include <radray/stopwatch.h>

#include <radray/render/backend/vulkan_impl.h>
#include <radray/render/backend/d3d12_impl.h>

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

using namespace radray::render;

namespace radray {

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
            RADRAY_ERR_LOG("{} {}::{}", Errors::RADRAYIMGUI, "desc", "Window");
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
    auto device = desc.Device;
    RenderBackend backendType = device->GetBackend();
    unique_ptr<Shader> shaderVS;
    unique_ptr<Shader> shaderPS;
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
        RADRAY_ERR_LOG("{} {} {}", Errors::RADRAYIMGUI, Errors::UnsupportedPlatform, backendType);
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
    sampler.AnisotropyClamp = 0;
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

    RootSignatureConstant rsConst{};
    rsConst.Slot = 0;
    rsConst.Space = 0;
    rsConst.Size = 64;
    rsConst.Stages = ShaderStage::Vertex;
    RootSignatureDescriptor rsDesc{};
    rsDesc.Constant = rsConst;
    RootSignatureDescriptorSet descSet{};
    descSet.Elements = rsElems;
    rsDesc.DescriptorSets = std::span{&descSet, 1};
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
    result->_rs = std::move(rs);
    result->_pso = std::move(pso);
    result->_desc = desc;
    result->_frames.reserve(desc.FrameCount);
    for (int i = 0; i < desc.FrameCount; i++) {
        result->_frames.emplace_back(ImGuiDrawContext::Frame{});
    }
    return result;
}

ImGuiDrawTexture::ImGuiDrawTexture(
    unique_ptr<render::Texture> tex,
    unique_ptr<render::TextureView> srv) noexcept
    : _tex(std::move(tex)),
      _srv(std::move(srv)) {}

ImGuiDrawTexture::~ImGuiDrawTexture() noexcept {
    _srv.reset();
    _tex.reset();
}

void ImGuiDrawContext::ExtractDrawData(int frameIndex, ImDrawData* drawData) {
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
        if (frame._vb) {
            frame._garbageBuffers.emplace_back(std::move(frame._vb));
        }
        int vertCount = drawData->TotalVtxCount + 5000;
        BufferDescriptor desc{};
        desc.Size = vertCount * sizeof(ImDrawVert);
        desc.Memory = MemoryType::Upload;
        desc.Usage = BufferUse::Vertex | BufferUse::CopySource;
        desc.Hints = ResourceHint::Dedicated;
        string vbName = format("imgui_vb_{}", frameIndex);
        desc.Name = vbName;
        frame._vb = _device->CreateBuffer(desc).Unwrap();
        frame._vbSize = vertCount;
    }
    if (frame._ib == nullptr || frame._ibSize < drawData->TotalIdxCount) {
        if (frame._ib) {
            frame._garbageBuffers.emplace_back(std::move(frame._ib));
        }
        int idxCount = drawData->TotalIdxCount + 10000;
        BufferDescriptor desc{};
        desc.Size = idxCount * sizeof(ImDrawIdx);
        desc.Memory = MemoryType::Upload;
        desc.Usage = BufferUse::Index | BufferUse::CopySource;
        desc.Hints = ResourceHint::Dedicated;
        string ibName = format("imgui_ib_{}", frameIndex);
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
        unique_ptr<Texture> texObj = _device->CreateTexture(texDesc).Unwrap();

        TextureViewDescriptor viewDesc{};
        viewDesc.Target = texObj.get();
        viewDesc.Dim = TextureViewDimension::Dim2D;
        viewDesc.Format = TextureFormat::RGBA8_UNORM;
        viewDesc.Range = SubresourceRange::AllSub();
        viewDesc.Usage = TextureUse::Resource;
        unique_ptr<TextureView> srv = _device->CreateTextureView(viewDesc).Unwrap();

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
        uploadDesc.Size = upload_size;
        uploadDesc.Memory = MemoryType::Upload;
        uploadDesc.Usage = BufferUse::CopySource;
        uploadDesc.Name = uploadName;
        unique_ptr<Buffer> uploadBuffer = _device->CreateBuffer(uploadDesc).Unwrap();
        {
            void* dst = uploadBuffer->Map(0, upload_size);
            std::memcpy(dst, tex->GetPixels(), upload_size);
            uploadBuffer->Unmap(0, upload_size);
        }
        frame._needCopyTexs.emplace_back(UploadTexturePayload{texObjPtr, uploadBuffer.get(), tex->Status == ImTextureStatus_WantCreate});
        frame._tempUploadBuffers.emplace_back(std::move(uploadBuffer));
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
    ImGuiDrawContext::Frame& frame = _frames[frameIndex];

    frame._garbageBuffers.clear();
    frame._garbageTextures.clear();

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
                    RADRAY_ABORT("{} {} '{}'", Errors::RADRAYIMGUI, Errors::InvalidOperation, "UserCallback");
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
                        unique_ptr<DescriptorSet> descSet = _device->CreateDescriptorSet(_rs.get(), 0).Unwrap();
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
    ImGuiDrawContext::Frame& frame = _frames[frameIndex];

    for (Texture* texToDestroy : frame._waitDestroyTexs) {
        auto it = _texs.find(texToDestroy);
        if (it != _texs.end()) {
            TextureView* viewPtr = it->second->_srv.get();
            auto itSet = _descSetCache.find(viewPtr);
            if (itSet != _descSetCache.end()) {
                _descSetCache.erase(itSet);
            }
            frame._garbageTextures.emplace_back(std::move(it->second));
            _texs.erase(it);
        }
    }
    frame._waitDestroyTexs.clear();
    frame._needCopyTexs.clear();

    for (auto& buf : frame._tempUploadBuffers) {
        frame._garbageBuffers.emplace_back(std::move(buf));
    }
    frame._tempUploadBuffers.clear();
}

void ImGuiDrawContext::SetupRenderState(int frameIndex, render::CommandEncoder* encoder, int fbWidth, int fbHeight) {
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
    Eigen::Matrix4f proj = OrthoLH(L, R, B, T, 0.0f, 1.0f);
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

ImGuiApplication::ImGuiApplication() noexcept = default;

ImGuiApplication::~ImGuiApplication() noexcept = default;

class ImGuiApplication::RenderContextVulkan final : public ImGuiApplication::RenderContext {
public:
    using Base = ImGuiApplication::RenderContext;

    RenderContextVulkan(
        ImGuiApplication& app,
        const ImGuiApplicationDescriptor& appDesc)
        : Base(app) {
        render::VulkanInstanceDescriptor insDesc{};
        insDesc.AppName = appDesc.AppName;
        insDesc.AppVersion = 1;
        insDesc.EngineName = "RadRay";
        insDesc.EngineVersion = 1;
        insDesc.IsEnableDebugLayer = appDesc.EnableValidation;
        insDesc.IsEnableGpuBasedValid = appDesc.EnableValidation;
        _vkIns = CreateVulkanInstance(insDesc).Unwrap();
        render::VulkanCommandQueueDescriptor queueDesc[] = {
            {render::QueueType::Direct, 1}};
        render::VulkanDeviceDescriptor devDesc{};
        if (appDesc.DeviceDesc.has_value()) {
            devDesc = std::get<render::VulkanDeviceDescriptor>(appDesc.DeviceDesc.value());
        } else {
            devDesc.Queues = queueDesc;
        }
        _device = std::static_pointer_cast<render::vulkan::DeviceVulkan>(CreateDevice(devDesc).Unwrap());
        _cmdQueue = static_cast<render::vulkan::QueueVulkan*>(_device->GetCommandQueue(render::QueueType::Direct, 0).Unwrap());
        this->RecreateSwapChainImpl();
    }

    ~RenderContextVulkan() noexcept override {
        _rts.clear();
        _rtViews.clear();
        _imageAvailableSemaphores.clear();
        _renderFinishSemaphores.clear();
        _inFlightFences.clear();
        _swapchain.reset();
        _cmdQueue = nullptr;
        _device.reset();
        render::DestroyVulkanInstance(std::move(_vkIns));
    }

    render::Device* GetDevice() const noexcept override { return _device.get(); }
    uint32_t GetCurrentBackBufferIndex() const noexcept override { return _currentBackBufferIndex; }
    uint32_t GetCurrentFrameIndex() const noexcept override { return _currentFrameIndex; }
    render::Texture* GetBackBuffer(uint32_t index) const noexcept override { return _rts[index]; }
    render::TextureView* GetBackBufferDefaultRTV(uint32_t index) noexcept override {
        if (_rtViews[index] == nullptr) {
            auto tex = _rts[index];
            TextureViewDescriptor rtViewDesc{};
            rtViewDesc.Target = tex;
            rtViewDesc.Dim = TextureViewDimension::Dim2D;
            rtViewDesc.Format = TextureFormat::RGBA8_UNORM;
            rtViewDesc.Usage = TextureUse::RenderTarget;
            rtViewDesc.Range.BaseMipLevel = 0;
            rtViewDesc.Range.MipLevelCount = SubresourceRange::All;
            rtViewDesc.Range.BaseArrayLayer = 0;
            rtViewDesc.Range.ArrayLayerCount = SubresourceRange::All;
            _rtViews[index] = StaticCastUniquePtr<vulkan::ImageViewVulkan>(_device->CreateTextureView(rtViewDesc).Unwrap());
        }
        return _rtViews[index].get();
    }
    void Wait() noexcept override {
        for (auto& fence : _inFlightFences) {
            fence->Wait();
        }
        _cmdQueue->Wait();
    }

    void Render() override {
        if (_app._rtSize.x() <= 0 || _app._rtSize.y() <= 0) {
            return;
        }
        Fence* inFlightFence = _inFlightFences[_currentFrameIndex].get();
        inFlightFence->Wait();
        Semaphore* imageAvailableSemaphore = _imageAvailableSemaphores[_currentFrameIndex].get();
        auto rtOpt = _swapchain->AcquireNext(imageAvailableSemaphore, nullptr);
        if (!rtOpt.HasValue()) {
            return;
        }
        _currentBackBufferIndex = _swapchain->GetCurrentBackBufferIndex();
        _rts[_currentBackBufferIndex] = static_cast<vulkan::ImageVulkan*>(rtOpt.Release());
        auto submitCmdBufs = _app.OnRender();
        Semaphore* renderFinishSemaphore = _renderFinishSemaphores[_currentBackBufferIndex].get();
        render::CommandQueueSubmitDescriptor submitDesc{};
        submitDesc.CmdBuffers = submitCmdBufs;
        submitDesc.WaitSemaphores = std::span{&imageAvailableSemaphore, 1};
        submitDesc.SignalSemaphores = std::span{&renderFinishSemaphore, 1};
        submitDesc.SignalFence = inFlightFence;
        _cmdQueue->Submit(submitDesc);
        _swapchain->Present(std::span{&renderFinishSemaphore, 1});
        _currentFrameIndex = (_currentFrameIndex + 1) % static_cast<uint32_t>(_inFlightFences.size());
    }

    void RecreateSwapChainImpl() {
        if (_swapchain) {
            _cmdQueue->Wait();
            _swapchain.reset();
        }
        render::SwapChainDescriptor swapchainDesc{};
        swapchainDesc.PresentQueue = _cmdQueue;
        swapchainDesc.NativeHandler = _app._window->GetNativeHandler().Handle;
        swapchainDesc.Width = static_cast<uint32_t>(_app._rtSize.x());
        swapchainDesc.Height = static_cast<uint32_t>(_app._rtSize.y());
        swapchainDesc.BackBufferCount = _app._backBufferCount;
        swapchainDesc.Format = _app._rtFormat;
        swapchainDesc.EnableSync = _app._enableVSync;
        _swapchain = StaticCastUniquePtr<render::vulkan::SwapChainVulkan>(_device->CreateSwapChain(swapchainDesc).Unwrap());
        _renderFinishSemaphores.resize(swapchainDesc.BackBufferCount);
        for (auto& semaphore : _renderFinishSemaphores) {
            semaphore = StaticCastUniquePtr<render::vulkan::SemaphoreVulkan>(_device->CreateSemaphoreDevice().Unwrap());
        }
        _imageAvailableSemaphores.resize(_app._frameCount);
        for (auto& semaphore : _imageAvailableSemaphores) {
            semaphore = StaticCastUniquePtr<render::vulkan::SemaphoreVulkan>(_device->CreateSemaphoreDevice().Unwrap());
        }
        _inFlightFences.resize(_app._frameCount);
        for (auto& fence : _inFlightFences) {
            fence = StaticCastUniquePtr<render::vulkan::FenceVulkan>(_device->CreateFence().Unwrap());
        }
        _rtViews.resize(swapchainDesc.BackBufferCount);
        for (auto& rtView : _rtViews) {
            rtView = nullptr;
        }
        _rts.resize(swapchainDesc.BackBufferCount);
        for (auto& rt : _rts) {
            rt = nullptr;
        }
        _currentFrameIndex = 0;
        _currentBackBufferIndex = 0;
    }

public:
    unique_ptr<render::InstanceVulkan> _vkIns;
    shared_ptr<render::vulkan::DeviceVulkan> _device;
    render::vulkan::QueueVulkan* _cmdQueue;
    unique_ptr<render::vulkan::SwapChainVulkan> _swapchain;
    vector<unique_ptr<render::vulkan::SemaphoreVulkan>> _renderFinishSemaphores;
    vector<unique_ptr<render::vulkan::SemaphoreVulkan>> _imageAvailableSemaphores;
    vector<unique_ptr<render::vulkan::FenceVulkan>> _inFlightFences;
    vector<unique_ptr<render::vulkan::ImageViewVulkan>> _rtViews;
    vector<render::vulkan::ImageVulkan*> _rts;
    uint32_t _currentFrameIndex;
    uint32_t _currentBackBufferIndex;
};

class ImGuiApplication::SingleThreadStrategy : public ImGuiApplication::RunningStrategy {
public:
    using Base = ImGuiApplication::RunningStrategy;

    using Base::RunningStrategy;
    ~SingleThreadStrategy() noexcept = default;

    void Run() override {
        while (true) {
            _app.Update();
            if (_app._needClose) {
                break;
            }
            _app._renderContext->Render();
        }
    }
};

void ImGuiApplication::Run() {
    this->OnStart();
    _runningStrategy->Run();
}

void ImGuiApplication::Destroy() noexcept {
    _resizingConn.disconnect();
    _resizedConn.disconnect();
    _renderContext->Wait();
    this->OnDestroy();
    TerminateRendererImGui(_imguiContext->Get());
    TerminatePlatformImGui(_imguiContext->Get());
    _imguiDrawContext.reset();
    _renderContext.reset();
    _window.reset();
    _win32ImguiProc.reset();
    _imguiContext.reset();
    _runningStrategy.reset();
}

void ImGuiApplication::Init(const ImGuiApplicationDescriptor& desc_) {
    ImGuiApplicationDescriptor appDesc = desc_;
    if (!appDesc.IsRenderMultiThread) {
        if (appDesc.IsWaitFrame) {
            RADRAY_WARN_LOG("{}: ImGuiApplicationDescriptor::IsWaitFrame is only effective when IsRenderMultiThread is true.", Errors::RADRAYIMGUI);
        }
        appDesc.IsWaitFrame = false;
    }

    _backBufferCount = appDesc.BackBufferCount;
    _frameCount = appDesc.FrameCount;
    _rtFormat = appDesc.RTFormat;
    _rtSize = appDesc.WindowSize;
    _enableVSync = appDesc.EnableVSync;
    _isWaitFrame = appDesc.IsWaitFrame;
    _multithreadRender = appDesc.IsRenderMultiThread;

    _imguiContext = radray::make_unique<radray::ImGuiContextRAII>();

    PlatformId platform = GetPlatform();
    if (platform == PlatformId::Windows) {
        _win32ImguiProc = make_shared<std::function<Win32WNDPROC>>(GetImGuiWin32WNDPROCEx(_imguiContext->Get()).Release());
        weak_ptr<std::function<Win32WNDPROC>> weakImguiProc = _win32ImguiProc;
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
        auto windowOpt = CreateNativeWindow(desc);
        _window = windowOpt.Release();
        ImGuiPlatformInitDescriptor imguiDesc{};
        imguiDesc.Platform = PlatformId::Windows;
        imguiDesc.Window = _window.get();
        imguiDesc.Context = _imguiContext->Get();
        InitPlatformImGui(imguiDesc);
        InitRendererImGui(imguiDesc.Context);
    }
    if (!_window) {
        throw ImGuiApplicationException("{}: {}", Errors::RADRAYIMGUI, "fail create window");
    }
    _resizingConn = _window->EventResizing().connect(&ImGuiApplication::OnResizing, this);
    _resizedConn = _window->EventResized().connect(&ImGuiApplication::OnResized, this);

    if (platform == PlatformId::Windows && appDesc.Backend == render::RenderBackend::D3D12) {
    } else if (appDesc.Backend == render::RenderBackend::Vulkan) {
        _renderContext = make_unique<RenderContextVulkan>(*this, appDesc);
    } else {
        throw ImGuiApplicationException("{}: {}", Errors::RADRAYIMGUI, Errors::UnsupportedPlatform);
    }
    if (!_renderContext) {
        throw ImGuiApplicationException("{}: {}", Errors::RADRAYIMGUI, "fail create RenderContext");
    }
    {
        ImGuiDrawDescriptor imguiDrawDesc{};
        imguiDrawDesc.Device = _renderContext->GetDevice();
        imguiDrawDesc.RTFormat = appDesc.RTFormat;
        imguiDrawDesc.FrameCount = _frameCount;
        auto ctxOpt = CreateImGuiDrawContext(imguiDrawDesc);
        if (!ctxOpt.HasValue()) {
            throw ImGuiApplicationException("{}: {}", Errors::RADRAYIMGUI, "failed create ImGuiDrawContext");
        }
        _imguiDrawContext = ctxOpt.Release();
    }

    if (_multithreadRender) {
    } else {
        _runningStrategy = make_unique<SingleThreadStrategy>(*this);
    }
}

void ImGuiApplication::Update() {
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

    auto currFrameIndex = _renderContext->GetCurrentFrameIndex();
    ImDrawData* drawData = ImGui::GetDrawData();
    _imguiDrawContext->ExtractDrawData((int)currFrameIndex, drawData);
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

vector<render::CommandBuffer*> ImGuiApplication::OnRender() { return {}; }

void ImGuiApplication::OnDestroy() noexcept {}

}  // namespace radray

#endif
