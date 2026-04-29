#ifdef RADRAY_PLATFORM_WINDOWS

#include <radray/logger.h>
#include <radray/window/native_window.h>

#include <radray/platform/win32_headers.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandlerEx(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, ImGuiIO& io);

namespace radray {

bool InitImGuiInternal(ImGuiContext* ctx, NativeWindow* window) {
    WindowNativeHandler wnh = window->GetNativeHandler();
    if (wnh.Type != WindowHandlerTag::HWND) {
        RADRAY_ERR_LOG("Expected HWND for Win32 ImGui initialization, got {}", wnh.Type);
        return false;
    }
    ImGui::SetCurrentContext(ctx);
    float mainScale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(mainScale);
    style.FontScaleDpi = mainScale;
    return ImGui_ImplWin32_Init(wnh.Handle);
}

void ShutdownImGuiInternal(ImGuiContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    ImGui::SetCurrentContext(ctx);
    ImGui_ImplWin32_Shutdown();
}

}  // namespace radray

#endif
