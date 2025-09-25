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

bool InitImGui() {
    IMGUI_CHECKVERSION();
#ifdef RADRAY_PLATFORM_WINDOWS
    ImGui_ImplWin32_EnableDpiAwareness();
#endif
    ImGui::SetAllocatorFunctions(_ImguiAllocBridge, _ImguiFreeBridge, nullptr);
    ImGui::CreateContext();
    return true;
}

bool InitPlatformImGui() {
    return true;
}

void TerminateImGui() {
    ImGui::DestroyContext();
}

}  // namespace radray

#endif
