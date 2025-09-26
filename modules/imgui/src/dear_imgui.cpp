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

Nullable<Win32WNDPROC> GetWin32WNDPROCImGui() noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    return ImGui_ImplWin32_WndProcHandler;
#else
    return nullptr;
#endif
}

Nullable<unique_ptr<ImGuiDrawContext>> CreateImGuiDrawContext(render::Device* device) noexcept {
    using namespace ::radray::render;

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
    samplerDesc[0].AddressS = AddressMode::ClampToEdge;
    samplerDesc[0].AddressT = AddressMode::ClampToEdge;
    samplerDesc[0].AddressR = AddressMode::ClampToEdge;
    samplerDesc[0].MigFilter = FilterMode::Linear;
    samplerDesc[0].MagFilter = FilterMode::Linear;
    samplerDesc[0].MipmapFilter = FilterMode::Linear;
    samplerDesc[0].LodMin = 0.0f;
    samplerDesc[0].LodMax = std::numeric_limits<float>::max();
    samplerDesc[0].Compare = CompareFunction::Always;
    samplerDesc[0].AnisotropyClamp = 0.0f;
    RootSignatureSetElement rsElems[1];
    rsElems[0].Slot = 0;
    rsElems[0].Space = 0;
    rsElems[0].Type = ResourceBindType::Sampler;
    rsElems[0].Count = 1;
    rsElems[0].Stages = ShaderStage::Pixel;
    rsElems[0].StaticSamplers = samplerDesc;
    RootSignatureBindingSet rsSet;
    rsSet.Elements = rsElems;
    auto layout = device->CreateDescriptorSetLayout(rsSet).Unwrap();

    RootSignatureConstant rsConst{};
    rsConst.Slot = 0;
    rsConst.Space = 0;
    rsConst.Size = 64;
    rsConst.Stages = ShaderStage::Vertex;
    RootSignatureBinding rsBind[1];
    rsBind[0].Slot = 0;
    rsBind[0].Space = 0;
    rsBind[0].Type = ResourceBindType::Texture;
    rsBind[0].Stages = ShaderStage::Pixel;
    DescriptorSetLayout* layouts[1];
    layouts[0] = layout.get();
    RootSignatureDescriptor rsDesc{};
    rsDesc.Constant = rsConst;
    rsDesc.RootBindings = rsBind;
    rsDesc.BindingSets = layouts;
    auto rs = device->CreateRootSignature(rsDesc).Unwrap();

    auto result = make_unique<ImGuiDrawContext>();
    result->_rsLayout = layout;
    result->_rs = rs;
    return result;
}

}  // namespace radray

#endif
