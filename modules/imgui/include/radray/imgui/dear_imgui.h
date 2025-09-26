#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <imgui.h>
#ifdef RADRAY_PLATFORM_WINDOWS
#include <imgui_impl_win32.h>
#endif

#include <radray/render/common.h>
#include <radray/window/native_window.h>

namespace radray {

class ImGuiPlatformInitDescriptor {
public:
    PlatformId Platform;

    NativeWindow* Window;
};

class ImGuiRendererData {
public:
};

class ImGuiDrawContext {
public:
    shared_ptr<render::DescriptorSetLayout> _rsLayout;
    shared_ptr<render::RootSignature> _rs;
};

bool InitImGui();
bool InitPlatformImGui(const ImGuiPlatformInitDescriptor& desc);
bool InitRendererImGui();
void TerminateRendererImGui();
void TerminatePlatformImGui();
void TerminateImGui();

Nullable<Win32WNDPROC> GetWin32WNDPROCImGui() noexcept;
std::span<const byte> GetImGuiShaderDXIL_VS() noexcept;
std::span<const byte> GetImGuiShaderDXIL_PS() noexcept;
std::span<const byte> GetImGuiShaderSPIRV_VS() noexcept;
std::span<const byte> GetImGuiShaderSPIRV_PS() noexcept;
Nullable<unique_ptr<ImGuiDrawContext>> CreateImGuiDrawContext(render::Device* device) noexcept;

}  // namespace radray

#endif
