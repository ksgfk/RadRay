#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <cstddef>
#include <span>

#include <imgui.h>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/render/common.h>

namespace radray {

class GpuRuntime;
class GpuSurface;
class Application;
class AppWindow;
class ImGuiSystem;
struct ImGuiViewportRendererData;

class ImGuiSystemDescriptor {
public:
    Application* App{nullptr};
    AppWindow* MainWnd{nullptr};
    bool EnableViewports{false};
};

class ImGuiContextRAII {
public:
    explicit ImGuiContextRAII(ImFontAtlas* sharedFontAtlas = nullptr);
    ImGuiContextRAII(const ImGuiContextRAII&) = delete;
    ImGuiContextRAII(ImGuiContextRAII&&) noexcept;
    ImGuiContextRAII& operator=(const ImGuiContextRAII&) = delete;
    ImGuiContextRAII& operator=(ImGuiContextRAII&&) noexcept;
    ~ImGuiContextRAII() noexcept;

    bool IsValid() const noexcept;
    void Destroy() noexcept;
    ImGuiContext* Get() const noexcept;

    void SetCurrent();

    friend constexpr void swap(ImGuiContextRAII& a, ImGuiContextRAII& b) noexcept {
        using std::swap;
        swap(a._ctx, b._ctx);
    }

private:
    ImGuiContext* _ctx{nullptr};
};

class ImGuiRenderer {
public:
    explicit ImGuiRenderer(Application* app, AppWindow* mainWnd);
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer(ImGuiRenderer&&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(ImGuiRenderer&&) = delete;
    ~ImGuiRenderer() noexcept;

    static Nullable<unique_ptr<ImGuiRenderer>> Create(Application* app, AppWindow* mainWnd);

    static void CreateWindow(ImGuiViewport* viewport);
    static void DestroyWindow(ImGuiViewport* viewport);
    static void SetWindowSize(ImGuiViewport* viewport, ImVec2 size);
    static void RenderWindow(ImGuiViewport* viewport, void* renderArg);
    static void SwapBuffers(ImGuiViewport* viewport, void* renderArg);

public:
    Application* _app{nullptr};
    AppWindow* _mainWnd{nullptr};
    ImGuiSystem* _system{nullptr};
    unique_ptr<render::Shader> _vs{};
    unique_ptr<render::Shader> _ps{};
    unique_ptr<render::RootSignature> _rs{};
    unique_ptr<render::GraphicsPipelineState> _pso{};
};

class ImGuiSystem {
public:
    ImGuiSystem(Application* app, AppWindow* mainWnd, unique_ptr<ImGuiContextRAII> context);
    ImGuiSystem(const ImGuiSystem&) = delete;
    ImGuiSystem(ImGuiSystem&&) = delete;
    ImGuiSystem& operator=(const ImGuiSystem&) = delete;
    ImGuiSystem& operator=(ImGuiSystem&&) = delete;
    ~ImGuiSystem() noexcept;

    static Nullable<unique_ptr<ImGuiSystem>> Create(const ImGuiSystemDescriptor& desc);

public:
    Application* _app;
    AppWindow* _mainWnd;
    unique_ptr<ImGuiContextRAII> _context;
    unique_ptr<ImGuiRenderer> _renderer;
    vector<unique_ptr<ImGuiViewportRendererData>> _viewportRendererData;
};

std::span<const std::byte> GetImGuiHLSL() noexcept;
std::span<const std::byte> GetImGuiVertexShaderDXIL() noexcept;
std::span<const std::byte> GetImGuiPixelShaderDXIL() noexcept;
std::span<const std::byte> GetImGuiVertexShaderSPIRV() noexcept;
std::span<const std::byte> GetImGuiPixelShaderSPIRV() noexcept;

}  // namespace radray

#endif
