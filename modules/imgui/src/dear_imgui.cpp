#include <radray/imgui/dear_imgui.h>

#include <cstdlib>
#include <bit>

#include <radray/utility.h>
#include <radray/basic_math.h>

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

bool InitImGui() {
    IMGUI_CHECKVERSION();
#ifdef RADRAY_PLATFORM_WINDOWS
    ImGui_ImplWin32_EnableDpiAwareness();
#endif
    ImGui::SetAllocatorFunctions(_ImguiAllocBridge, _ImguiFreeBridge, nullptr);
    ImGui::CreateContext();
    return true;
}

bool InitPlatformImGui(const ImGuiPlatformInitDescriptor& desc) {
#ifdef RADRAY_PLATFORM_WINDOWS
    if (desc.Platform == PlatformId::Windows) {
        WindowNativeHandler wnh = desc.Window->GetNativeHandler();
        if (wnh.Type != WindowHandlerTag::HWND) {
            RADRAY_ERR_LOG("radrayimgui only supports HWND on Windows platform.");
            return false;
        }
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        float mainScale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
        style.ScaleAllSizes(mainScale);
        style.FontScaleDpi = mainScale;
        ImGui_ImplWin32_Init(wnh.Handle);
        io.Fonts->AddFontDefault();
    }
#endif
    return true;
}

bool InitRendererImGui() {
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

void TerminateRendererImGui() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiRendererData* bd = (ImGuiRendererData*)io.BackendRendererUserData;
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    IM_DELETE(bd);
}

void TerminatePlatformImGui() {
#ifdef RADRAY_PLATFORM_WINDOWS
    ImGui_ImplWin32_Shutdown();
#endif
}

void TerminateImGui() {
    ImGui::DestroyContext();
}

Nullable<Win32WNDPROC> GetImGuiWin32WNDPROC() noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    return ImGui_ImplWin32_WndProcHandler;
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
        RADRAY_ERR_LOG("radrayimgui unsupported backend {}", backendType);
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
        auto& frame = result->_frames.emplace_back(ImGuiDrawContext::Frame{});
        frame._descSet = device->CreateDescriptorSet(result->_rsLayout.get()).Unwrap();
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

void ImGuiDrawContext::NewFrame() {
    auto& frame = _frames[_currentFrameIndex];
    frame._uploads.clear();
}

void ImGuiDrawContext::EndFrame() {
    _currentFrameIndex = (_currentFrameIndex + 1) % _frames.size();
}

void ImGuiDrawContext::UpdateDrawData(ImDrawData* drawData, render::CommandBuffer* cmdBuffer) {
    using namespace ::radray::render;

    if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        return;
    }
    auto& frame = _frames[_currentFrameIndex];
    if (drawData->Textures != nullptr) {
        for (ImTextureData* tex : *drawData->Textures) {
            if (tex->Status != ImTextureStatus_OK) {
                this->UpdateTexture(tex, cmdBuffer);
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
        string vbName = radray::format("imgui_vb_{}", _currentFrameIndex);
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
        string ibName = radray::format("imgui_ib_{}", _currentFrameIndex);
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
}

void ImGuiDrawContext::EndUpdateDrawData(ImDrawData* drawData) {
    using namespace ::radray::render;

    if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        return;
    }
    if (drawData->Textures != nullptr) {
        for (ImTextureData* tex : *drawData->Textures) {
            if (tex->Status != ImTextureStatus_OK) {
                if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
                    tex->SetStatus(ImTextureStatus_OK);
                }
                if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames >= _desc.FrameCount) {
                    auto texObjPtr = std::bit_cast<Texture*>(tex->BackendUserData);
                    _texs.erase(texObjPtr);
                    tex->SetTexID(ImTextureID_Invalid);
                    tex->SetStatus(ImTextureStatus_Destroyed);
                    tex->BackendUserData = nullptr;
                }
            }
        }
    }
}

void ImGuiDrawContext::UpdateTexture(ImTextureData* tex, render::CommandBuffer* cmdBuffer) {
    using namespace ::radray::render;

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
        ImGuiDrawTexture* drawTex = nullptr;
        {
            auto it = _texs.find(texObjPtr);
            if (it == _texs.end()) {
                RADRAY_ABORT("radrayimgui texture not found");
            }
            drawTex = it->second.get();
        }
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
        {
            BarrierTextureDescriptor barrierBefore{};
            barrierBefore.Target = drawTex->_tex.get();
            barrierBefore.Before = TextureUse::Uninitialized;
            barrierBefore.After = TextureUse::CopyDestination;
            barrierBefore.IsFromOrToOtherQueue = false;
            barrierBefore.IsSubresourceBarrier = false;
            cmdBuffer->ResourceBarrier({}, std::span{&barrierBefore, 1});
        }
        SubresourceRange range{};
        range.BaseArrayLayer = 0;
        range.ArrayLayerCount = 1;
        range.BaseMipLevel = 0;
        range.MipLevelCount = 1;
        cmdBuffer->CopyBufferToTexture(drawTex->_tex.get(), range, uploadBuffer.get(), 0);
        {
            BarrierTextureDescriptor barrierBefore{};
            barrierBefore.Target = drawTex->_tex.get();
            barrierBefore.Before = TextureUse::CopyDestination;
            barrierBefore.After = TextureUse::Resource;
            barrierBefore.IsFromOrToOtherQueue = false;
            barrierBefore.IsSubresourceBarrier = false;
            cmdBuffer->ResourceBarrier({}, std::span{&barrierBefore, 1});
        }

        auto& frame = _frames[_currentFrameIndex];
        frame._uploads.emplace_back(std::move(uploadBuffer));
    }
}

void ImGuiDrawContext::Draw(ImDrawData* drawData, render::CommandEncoder* encoder) {
    using namespace ::radray::render;

    if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        return;
    }

    auto& frame = _frames[_currentFrameIndex];

    int fbWidth = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    int fbHeight = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    this->SetupRenderState(drawData, encoder, fbWidth, fbHeight);

    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    ImGuiRenderState rs{};
    rs._encoder = encoder;
    platform_io.Renderer_RenderState = &rs;

    ImVec2 clip_off = drawData->DisplayPos;
    ImVec2 clip_scale = drawData->FramebufferScale;
    int global_vtx_offset = 0;
    int global_idx_offset = 0;
    for (const ImDrawList* draw_list : drawData->CmdLists) {
        for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++) {
            const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback != nullptr) {
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState) {
                    this->SetupRenderState(drawData, encoder, fbWidth, fbHeight);
                } else {
                    pcmd->UserCallback(draw_list, pcmd);
                }
            } else {
                ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x, (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
                ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x, (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);
                if (clip_min.x < 0.0f) {
                    clip_min.x = 0.0f;
                }
                if (clip_min.y < 0.0f) {
                    clip_min.y = 0.0f;
                }
                if (clip_max.x > (float)fbWidth) {
                    clip_max.x = (float)fbWidth;
                }
                if (clip_max.y > (float)fbHeight) {
                    clip_max.y = (float)fbHeight;
                }
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y) {
                    continue;
                }
                Rect scissor{};
                scissor.X = (int)clip_min.x;
                scissor.Y = (int)clip_min.y;
                scissor.Width = (int)(clip_max.x - clip_min.x);
                scissor.Height = (int)(clip_max.y - clip_min.y);
                encoder->SetScissor(scissor);
                auto texView = std::bit_cast<TextureView*>(pcmd->GetTexID());
                frame._descSet->SetResource(0, texView);
                encoder->BindDescriptorSet(0, frame._descSet.get());
                encoder->DrawIndexed(pcmd->ElemCount, 1, pcmd->IdxOffset + global_idx_offset, pcmd->VtxOffset + global_vtx_offset, 0);
            }
        }
        global_idx_offset += draw_list->IdxBuffer.Size;
        global_vtx_offset += draw_list->VtxBuffer.Size;
    }
    platform_io.Renderer_RenderState = nullptr;
    {
        Rect scissor{};
        scissor.X = 0;
        scissor.Y = 0;
        scissor.Width = fbWidth;
        scissor.Height = fbHeight;
        encoder->SetScissor(scissor);
    }
}

void ImGuiDrawContext::SetupRenderState(ImDrawData* drawData, render::CommandEncoder* encoder, int fbWidth, int fbHeight) {
    using namespace ::radray::render;

    auto& frame = _frames[_currentFrameIndex];

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

}  // namespace radray

#endif
