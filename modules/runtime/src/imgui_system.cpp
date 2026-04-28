#include <radray/runtime/imgui_system.h>

#include <radray/window/native_window.h>

namespace radray {

extern bool InitImGuiInternal(ImGuiContext* ctx, NativeWindow* window);

ImGuiContextRAII::ImGuiContextRAII(ImFontAtlas* sharedFontAtlas)
    : _ctx(ImGui::CreateContext(sharedFontAtlas)) {}

ImGuiContextRAII::ImGuiContextRAII(ImGuiContextRAII&& other) noexcept
    : _ctx(other._ctx) {
    other._ctx = nullptr;
}

ImGuiContextRAII& ImGuiContextRAII::operator=(ImGuiContextRAII&& other) noexcept {
    ImGuiContextRAII temp{std::move(other)};
    swap(*this, temp);
    return *this;
}

ImGuiContextRAII::~ImGuiContextRAII() noexcept {
    this->Destroy();
}

bool ImGuiContextRAII::IsValid() const noexcept {
    return _ctx != nullptr;
}

void ImGuiContextRAII::Destroy() noexcept {
    if (_ctx != nullptr) {
        ImGui::DestroyContext(_ctx);
        _ctx = nullptr;
    }
}

ImGuiContext* ImGuiContextRAII::Get() const noexcept { return _ctx; }

void ImGuiContextRAII::SetCurrent() {
    ImGui::SetCurrentContext(_ctx);
}

ImGuiSystem::ImGuiSystem(
    Application* app,
    unique_ptr<ImGuiContextRAII> context)
    : _app(app),
      _context(std::move(context)) {}

Nullable<unique_ptr<ImGuiSystem>> ImGuiSystem::Create(const ImGuiSystemDescriptor& desc) {
    RADRAY_ASSERT(IMGUI_CHECKVERSION());
    auto context = make_unique<ImGuiContextRAII>();
    context->SetCurrent();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.Fonts->AddFontDefault();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    if (!InitImGuiInternal(context->Get(), desc.MainWindow)) {
        return nullptr;
    }

    return make_unique<ImGuiSystem>(desc.App, std::move(context));
}

}  // namespace radray
