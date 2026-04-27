#pragma once

#include <imgui.h>

namespace radray {

class ImGuiSystem {
public:
    static bool InitializeEnvironment() noexcept;
    static void ShutdownEnvironment() noexcept;
};

// class ImGuiSystem {
// public:
//     ImGuiSystem() noexcept = default;
//     ImGuiSystem(const ImGuiSystem&) = delete;
//     ImGuiSystem& operator=(const ImGuiSystem&) = delete;
//     ImGuiSystem(ImGuiSystem&&) = delete;
//     ImGuiSystem& operator=(ImGuiSystem&&) = delete;
//     ~ImGuiSystem() noexcept;

//     bool Initialize(const ImGuiSystemDesc& desc) noexcept;
//     void Shutdown() noexcept;

//     void BeginFrame(float deltaSeconds) noexcept;
//     void EndFrame() noexcept;

//     bool IsInitialized() const noexcept;
//     bool IsViewportWindow(AppWindowHandle handle) const noexcept;

// private:
//     void* _backend{nullptr};
// };

}  // namespace radray
