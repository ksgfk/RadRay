#include <radray/imgui/dear_imgui.h>

#include <cstdlib>

#include <radray/utility.h>

#ifdef RADRAY_ENABLE_IMGUI

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

bool GlobalInitDearImGui() {
    IMGUI_CHECKVERSION();
    ImGui::SetAllocatorFunctions(_ImguiAllocBridge, _ImguiFreeBridge, nullptr);
    ImGui::CreateContext();
    return true;
}

void GlobalTerminateDearImGui() {
    ImGui::DestroyContext();
}

}  // namespace radray::render

#endif
