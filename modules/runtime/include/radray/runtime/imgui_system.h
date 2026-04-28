#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <imgui.h>

#include <radray/types.h>
#include <radray/nullable.h>

namespace radray {

class Application;
class NativeWindow;

class ImGuiSystemDescriptor {
public:
    Application* App{nullptr};
    NativeWindow* MainWindow{nullptr};
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
};

class ImGuiSystem {
public:
    ImGuiSystem(Application* app, unique_ptr<ImGuiContextRAII> context);
    ImGuiSystem(const ImGuiSystem&) = delete;
    ImGuiSystem(ImGuiSystem&&) = delete;
    ImGuiSystem& operator=(const ImGuiSystem&) = delete;
    ImGuiSystem& operator=(ImGuiSystem&&) = delete;
    ~ImGuiSystem() noexcept;

    static Nullable<unique_ptr<ImGuiSystem>> Create(const ImGuiSystemDescriptor& desc);

public:
    Application* _app;
    unique_ptr<ImGuiContextRAII> _context;
};

}  // namespace radray

#endif
