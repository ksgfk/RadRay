#include <atomic>
#include <charconv>
#include <radray/runtime/application.h>
#include <radray/runtime/imgui/imgui_graph.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/window_manager.h>
#include <misc/cpp/imgui_stdlib.h>
#include <imgui_internal.h>
#ifdef RADRAY_PLATFORM_WINDOWS
#include <radray/platform/win32_headers.h>
#endif

namespace radray {
namespace {
struct Options {
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    uint32_t Frames{0}, Flights{2};
    bool Multithread{false}, Viewports{true}, Stress{false}, Srgb{false};
    std::filesystem::path Font, Settings;
};
class Gallery final : public Application {
public:
    explicit Gallery(Options options) : Opt(std::move(options)) {}
    bool Failed{false};

protected:
    void OnInit() override {
        GetRenderSystem()->SetPipeline(make_unique<ImGuiOnlyPipeline>(*GetImGuiSystem().Get()));
        Image.Create(ImTextureFormat_RGBA32, 128, 128);
        for (uint32_t y = 0; y < 128; ++y)
            for (uint32_t x = 0; x < 128; ++x) {
                const auto offset = (y * 128 + x) * 4;
                Image.Pixels[offset] = static_cast<unsigned char>(x * 2);
                Image.Pixels[offset + 1] = static_cast<unsigned char>(y * 2);
                Image.Pixels[offset + 2] = 100;
                Image.Pixels[offset + 3] = 255;
            }
        ImGui::RegisterUserTexture(&Image);
    }
    void OnUpdate(const AppUpdateContext&) override {
        ++Frame;
        if (Opt.Frames && Frame > Opt.Frames) {
#ifdef RADRAY_PLATFORM_WINDOWS
            ::PostMessageW(static_cast<HWND>(GetWindowManager()->GetMainWindow()->GetNativeWindow()->GetNativeHandler()), WM_CLOSE, 0, 0);
#endif
        }
    }
    void OnImGui() override {
        ImGui::DockSpaceOverViewport();
        ImGui::SetNextWindowSize({430, 530}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("RadRay runtime UI")) {
            ImGui::TextUnformatted("Docking / native viewports / RenderGraph / dynamic fonts");
            ImGui::Text("Frame %u, %u GPU flights", Frame, Opt.Flights);
            ImGui::InputText("UTF-8 / IME", &Text);
            ImGui::TextUnformatted("\xe4\xb8\xad\xe6\x96\x87 / Unicode: \xf0\x9f\x8e\xa8");
            {
                ImGui::Image(Image.GetTexRef(), {192, 192});
                ImGui::TextUnformatted("The runtime uploads and draws this CPU-authored image.");
            }
            ImGui::Checkbox("Official demo", &Demo);
            ImGui::Checkbox("Detached tool", &Tool);
            if (ImGui::SliderFloat("Style scale", &Scale, .75f, 2.0f)) GetImGuiSystem()->SetStyleScale(Scale);
            if (Opt.Stress) {
                auto* list = ImGui::GetWindowDrawList();
                const auto p = ImGui::GetCursorScreenPos();
                for (int i = 0; i < 18000; ++i) list->AddRectFilled({p.x + float(i % 160), p.y + float((i / 160) % 60)}, {p.x + float(i % 160) + 1, p.y + float((i / 160) % 60) + 1}, IM_COL32(200, 100, 40, 80));
                list->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
                ImGui::Image(Image.GetTexRef(), {80, 80});
                list->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
            }
        }
        ImGui::End();
        if (Tool && Opt.Viewports) {
            const auto* main = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos({main->Pos.x + main->Size.x + 20, main->Pos.y + 30}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize({260, 230}, ImGuiCond_FirstUseEver);
            ImGui::Begin("Detached runtime tool", &Tool);
            ImGui::TextUnformatted("Drag back to dock.");
            ImGui::Image(Image.GetTexRef(), {128, 128});
            ImGui::End();
        }
#ifndef IMGUI_DISABLE_DEMO_WINDOWS
        if (Demo) ImGui::ShowDemoWindow(&Demo);
#endif
    }
    void OnShutdown() override {
        Failed = GetImGuiSystem()->HasError();
        ImGui::UnregisterUserTexture(&Image);
        GetRenderSystem()->SetPipeline(nullptr);
        RADRAY_INFO_LOG("ImGui gallery completed {} frames: {}", Frame, Failed ? "FAILED" : "clean");
    }

private:
    Options Opt;
    uint32_t Frame{0};
    ImTextureData Image;
    bool Demo{false}, Tool{true};
    float Scale{1};
    string Text{"RadRay"};
};
}  // namespace
}  // namespace radray

int main(int argc, char** argv) {
    using namespace radray;
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--vulkan")
            options.Backend = render::RenderBackend::Vulkan;
        else if (arg == "--d3d12")
            options.Backend = render::RenderBackend::D3D12;
        else if (arg == "--multithread")
            options.Multithread = true;
        else if (arg == "--no-viewports")
            options.Viewports = false;
        else if (arg == "--stress")
            options.Stress = true;
        else if (arg == "--srgb")
            options.Srgb = true;
        else if ((arg == "--font" || arg == "--settings") && i + 1 < argc) {
            if (arg == "--font")
                options.Font = argv[++i];
            else
                options.Settings = argv[++i];
        } else if ((arg == "--frames" || arg == "--flights") && i + 1 < argc) {
            std::string_view value{argv[++i]};
            uint32_t number = 0;
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return 2;
            if (arg == "--frames")
                options.Frames = number;
            else
                options.Flights = number;
        } else {
            RADRAY_ERR_LOG("Unknown ImGui gallery argument: {}", arg);
            return 2;
        }
    }
    if (options.Flights < 2 || options.Flights > 3) return 2;
    ApplicationRuntimeDescriptor descriptor{.Backend = options.Backend, .EnableValidation = true, .Multithreaded = options.Multithread, .WindowTitle = "RadRay ImGui", .WindowWidth = 960, .WindowHeight = 720, .FlightDataCount = options.Flights, .BackBufferFormat = options.Srgb ? render::TextureFormat::BGRA8_UNORM_SRGB : render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO, .EnableSynchronizationValidation = true};
    descriptor.ImGui.Enabled = true;
    descriptor.ImGui.Viewports = options.Viewports;
    descriptor.ImGui.SettingsPath = options.Settings;
    if (!options.Font.empty()) descriptor.ImGui.Fonts.push_back({options.Font});
    std::atomic_bool errors{false};
    SetLogCallback(+[](LogLevel level, std::string_view, void* data) { if (level == LogLevel::Err || level == LogLevel::Critical) static_cast<std::atomic_bool*>(data)->store(true); }, &errors);
    Gallery app(options);
    const int result = app.Run(descriptor);
    ClearLogCallback();
    return result || app.Failed || errors ? 1 : 0;
}
