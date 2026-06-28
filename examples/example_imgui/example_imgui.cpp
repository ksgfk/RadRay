#include <radray/runtime/application.h>
#include <radray/runtime/imgui_system.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/render/common.h>
#include <radray/basic_math.h>
#include <radray/triangle_mesh.h>
#include <radray/logger.h>

#include <string_view>

#ifndef RADRAY_EXAMPLE_ASSET_DIR
#define RADRAY_EXAMPLE_ASSET_DIR "."
#endif

using namespace radray;

class ExampleApp : public Application {
public:
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;
    static constexpr render::TextureFormat DepthFormat = render::TextureFormat::D32_FLOAT;

    void OnInit() override {
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        if (ImGuiSystem* imgui = GetSubsystem<ImGuiSystem>()) {
            if (imgui->Begin(ctx)) {
                DrawMonitorUi(ctx);
                imgui->End();
            }
        }
    }

    void DrawMonitorUi(const AppUpdateContext& ctx) {
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration |
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoMove;
        constexpr float PAD = 10.0f;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 workPos = viewport->WorkPos;
        ImVec2 windowPos{workPos.x + PAD, workPos.y + PAD};
        ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, ImVec2{0.0f, 0.0f});
        ImGui::SetNextWindowBgAlpha(0.35f);
        if (ImGui::Begin("RadrayMonitor", &_showMonitor, windowFlags)) {
            ImGui::Text("Delta time: %06.3f ms", ctx.DeltaTime.count() * 1000.0f);
            ImGui::Text("Frame latency: %06.3f ms", ctx.LastFrameLatency.count() * 1000.0f);
            ImGui::Text("GPU time: %06.3f ms", GetGpuSystem()->GetLastGpuTimeMs());
            static constexpr render::PresentMode kModes[] = {
                render::PresentMode::FIFO,
                render::PresentMode::Mailbox,
                render::PresentMode::Immediate};
            render::PresentMode currentMode = render::PresentMode::FIFO;
            if (GetWindowManager() != nullptr) {
                currentMode = GetWindowManager()->GetMainPresentMode();
            }
            string preview{render::format_as(currentMode)};
            if (ImGui::BeginCombo("Present Mode", preview.c_str())) {
                for (render::PresentMode mode : kModes) {
                    string item{render::format_as(mode)};
                    const bool selected = mode == currentMode;
                    if (ImGui::Selectable(item.c_str(), selected) && mode != currentMode) {
                        GetWindowManager()->SetPresentMode(mode);
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::End();

        ImGui::ShowDemoWindow();
    }

private:
    bool _showMonitor{true};
};

int main(int argc, char* argv[]) {
    ApplicationRuntimeDescriptor desc{};
    desc.Backend = render::RenderBackend::Vulkan;
    desc.AppName = "Example ImGui App";
    desc.EngineName = "RadRay";
    desc.WindowTitle = "Example ImGui App";
    desc.WindowWidth = 1280;
    desc.WindowHeight = 720;
    desc.BackBufferFormat = ExampleApp::BackBufferFormat;
    desc.PresentMode = render::PresentMode::FIFO;

    for (int i = 0; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--backend" && i + 1 < argc) {
            std::string_view backendStr{argv[i + 1]};
            if (backendStr == "vulkan") {
                desc.Backend = render::RenderBackend::Vulkan;
            } else if (backendStr == "d3d12") {
                desc.Backend = render::RenderBackend::D3D12;
            }
        }
        if (arg == "--valid-layer") {
            desc.EnableValidation = true;
        }
        if (arg == "--multithread") {
            desc.Multithreaded = true;
        }
    }

    ExampleApp app{};
    app.RegisterSubsystem<ImGuiSystem>();
    return app.Run(desc);
}
