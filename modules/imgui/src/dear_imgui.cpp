#include <radray/imgui/dear_imgui.h>

#include <cstdlib>
#include <bit>

#include <radray/utility.h>

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

    SamplerDescriptor samplerDesc[1];
    SamplerDescriptor& sampler = samplerDesc[0];
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
    rsSampler.Slot = 0;
    rsSampler.Space = 0;
    rsSampler.Type = ResourceBindType::Sampler;
    rsSampler.Count = 1;
    rsSampler.Stages = ShaderStage::Pixel;
    rsSampler.StaticSamplers = samplerDesc;
    RootSignatureBindingSet rsSet;
    rsSet.Elements = rsElems;
    auto layout = device->CreateDescriptorSetLayout(rsSet).Unwrap();

    RootSignatureConstant rsConst{};
    rsConst.Slot = 0;
    rsConst.Space = 0;
    rsConst.Size = 64;
    rsConst.Stages = ShaderStage::Vertex;
    DescriptorSetLayout* layouts[1];
    layouts[0] = layout.get();
    RootSignatureDescriptor rsDesc{};
    rsDesc.Constant = rsConst;
    rsDesc.BindingSets = layouts;
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
    uvElem.Location = 0;
    VertexElement& colorElem = vertexElems[2];
    colorElem.Offset = offsetof(ImDrawVert, col);
    colorElem.Semantic = "COLOR";
    colorElem.SemanticIndex = 0;
    colorElem.Format = VertexFormat::UNORM8X4;
    colorElem.Location = 0;
    VertexBufferLayout vbLayout[1];
    vbLayout[0].ArrayStride = sizeof(ImDrawVert);
    vbLayout[0].StepMode = VertexStepMode::Vertex;
    vbLayout[0].Elements = vertexElems;
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
    ColorTargetState rtStates[1];
    BlendState rtBlendState{};
    rtBlendState.Color.Src = BlendFactor::SrcAlpha;
    rtBlendState.Color.Dst = BlendFactor::OneMinusSrcAlpha;
    rtBlendState.Color.Op = BlendOperation::Add;
    rtBlendState.Alpha.Src = BlendFactor::One;
    rtBlendState.Alpha.Dst = BlendFactor::OneMinusSrcAlpha;
    rtBlendState.Alpha.Op = BlendOperation::Add;
    ColorTargetState& rtState = rtStates[0];
    rtState.Format = desc.RTFormat;
    rtState.Blend = rtBlendState;
    rtState.WriteMask = ColorWrite::All;
    GraphicsPipelineStateDescriptor psoDesc{};
    psoDesc.RootSig = rs.get();
    psoDesc.VS = ShaderEntry{shaderVS.get(), "VSMain"};
    psoDesc.PS = ShaderEntry{shaderPS.get(), "PSMain"};
    psoDesc.VertexLayouts = vbLayout;
    psoDesc.Primitive = primState;
    psoDesc.DepthStencil = std::nullopt;
    psoDesc.MultiSample = msState;
    psoDesc.ColorTargets = rtStates;
    auto pso = device->CreateGraphicsPipelineState(psoDesc).Unwrap();

    auto result = make_unique<ImGuiDrawContext>();
    result->_device = device;
    result->_rsLayout = layout;
    result->_rs = rs;
    result->_pso = pso;
    result->_desc = desc;
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

void ImGuiDrawContext::Draw(ImDrawData* draw_data) {
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f) {
        return;
    }

    if (draw_data->Textures != nullptr) {
        for (ImTextureData* tex : *draw_data->Textures) {
            if (tex->Status != ImTextureStatus_OK) {
                this->UpdateTexture(tex);
            }
        }
    }

    // TODO: draw
}

void ImGuiDrawContext::UpdateTexture(ImTextureData* tex) {
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
        texDesc.Usage = TextureUse::Resource;
        texDesc.Name = texName;
        shared_ptr<Texture> texObj = _device->CreateTexture(texDesc).Unwrap();

        TextureViewDescriptor viewDesc{};
        viewDesc.Target = texObj.get();
        viewDesc.Dim = TextureViewDimension::Dim2D;
        viewDesc.Format = TextureFormat::RGBA8_UNORM;
        viewDesc.Range.BaseArrayLayer = 0;
        viewDesc.Range.BaseMipLevel = 0;
        viewDesc.Usage = TextureUse::Resource;
        shared_ptr<TextureView> srv = _device->CreateTextureView(viewDesc).Unwrap();

        auto texId = std::bit_cast<ImU64>(srv.get());
        tex->SetTexID(texId);
        tex->BackendUserData = texObj.get();

        _texs.emplace(texObj.get(), make_unique<ImGuiDrawTexture>(std::move(texObj), std::move(srv)));
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
        // TODO: upload data

        // const int upload_x = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.x;
        // const int upload_y = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.y;
        // const int upload_w = (tex->Status == ImTextureStatus_WantCreate) ? tex->Width : tex->UpdateRect.w;
        // const int upload_h = (tex->Status == ImTextureStatus_WantCreate) ? tex->Height : tex->UpdateRect.h;
        // int upload_pitch_src = upload_w * tex->BytesPerPixel;
        // int upload_size = upload_pitch_src * upload_h;

        // string uploadName = format("imgui_tex_upload_{}", tex->UniqueID);
        // BufferDescriptor uploadDesc{};
        // uploadDesc.Size = upload_size;
        // uploadDesc.Memory = MemoryType::Upload;
        // uploadDesc.Usage = BufferUse::CopySource;
        // uploadDesc.Name = uploadName;
        // shared_ptr<Buffer> uploadBuffer = _device->CreateBuffer(uploadDesc).Unwrap();

        
    }

    if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames >= _desc.FrameCount) {
        // TODO: destroy texture
    }
}

}  // namespace radray

#endif
