#include <radray/imgui/imgui_app.h>

#include <radray/errors.h>

#ifdef RADRAY_PLATFORM_WINDOWS
#include <imgui_impl_win32.h>
#include <radray/render/backend/d3d12_impl.h>
#include <radray/render/backend/vulkan_impl.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandlerEx(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, ImGuiIO& io);
#endif

namespace radray {

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

void ImGuiApplication::Run(const ImGuiAppConfig& config_) {
    auto config = config_;
    if (config.EnableFrameDropping && !config.EnableMultiThreading) {
        config.EnableFrameDropping = false;
        RADRAY_WARN_LOG("{}: Frame dropping requires multi-threading. Disabling frame dropping.", Errors::RADRAYIMGUI);
    }
    _rtWidth = config.Width;
    _rtHeight = config.Height;
    _backBufferCount = config.BackBufferCount;
    _inFlightFrameCount = config.InFlightFrameCount;
    _rtFormat = config.RTFormat;
    _enableVSync = config.EnableVSync;
    // imgui
    _imgui = make_unique<ImGuiContextRAII>();
    // window
#ifdef RADRAY_PLATFORM_WINDOWS
    std::function<Win32MsgProc> imguiProc = [this](void* hwnd_, uint32_t msg_, uint64_t wparam_, int64_t lparam_) -> int64_t {
        if (!_imgui) {
            return 0;
        }
        ImGui::SetCurrentContext(_imgui->Get());
        ImGuiIO& io = ImGui::GetIO();
        return ImGui_ImplWin32_WndProcHandlerEx(std::bit_cast<HWND>(hwnd_), msg_, wparam_, lparam_, io);
    };
    Win32WindowCreateDescriptor desc{};
    desc.Title = config.Title;
    desc.Width = config.Width;
    desc.Height = config.Height;
    desc.X = -1;
    desc.Y = -1;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    desc.ExtraWndProcs = std::span{&imguiProc, 1};
    _window = CreateNativeWindow(desc).Unwrap();
#endif
    if (!_window) {
        throw ImGuiApplicationException("{}: {}", Errors::RADRAYIMGUI, "fail create window");
    }
    // render
    if (config.Backend == render::RenderBackend::Vulkan) {
        render::VulkanInstanceDescriptor insDesc{};
        insDesc.AppName = config.AppName;
        insDesc.AppVersion = 1;
        insDesc.EngineName = "RadRay";
        insDesc.EngineVersion = 1;
        insDesc.IsEnableDebugLayer = config.EnableValidation;
        insDesc.IsEnableGpuBasedValid = config.EnableValidation;
        _vkIns = CreateVulkanInstance(insDesc).Unwrap();
        render::VulkanCommandQueueDescriptor queueDesc = {render::QueueType::Direct, 1};
        render::VulkanDeviceDescriptor devDesc{};
        if (config.DeviceDesc.has_value()) {
            devDesc = std::get<render::VulkanDeviceDescriptor>(config.DeviceDesc.value());
        } else {
            devDesc.Queues = std::span{&queueDesc, 1};
        }
        _device = CreateDevice(devDesc).Unwrap();
        _cmdQueue = _device->GetCommandQueue(render::QueueType::Direct, 0).Unwrap();
        this->RecreateSwapChain();
        _inFlightFences.resize(_inFlightFrameCount);
        for (auto& fence : _inFlightFences) {
            fence = StaticCastUniquePtr<render::vulkan::FenceVulkan>(_device->CreateFence().Unwrap());
        }
    }
}

void ImGuiApplication::Destroy() {
    _cmdQueue->Wait();

    this->OnDestroy();
    _defaultRTVs.clear();
    _backBuffers.clear();
    _imageAvailableSemaphores.clear();
    _renderFinishSemaphores.clear();
    _inFlightFences.clear();
    _swapchain.reset();
    _cmdQueue = nullptr;
    _device.reset();
    _window.reset();
    _imgui.reset();
}

void ImGuiApplication::RecreateSwapChain() {
    if (_swapchain) {
        _cmdQueue->Wait();
        _swapchain.reset();
    }
    render::SwapChainDescriptor swapchainDesc{};
    swapchainDesc.PresentQueue = _cmdQueue;
    swapchainDesc.NativeHandler = _window->GetNativeHandler().Handle;
    swapchainDesc.Width = _rtWidth;
    swapchainDesc.Height = _rtHeight;
    swapchainDesc.BackBufferCount = _backBufferCount;
    swapchainDesc.Format = _rtFormat;
    swapchainDesc.EnableSync = _enableVSync;
    _swapchain = _device->CreateSwapChain(swapchainDesc).Unwrap();
    if (_device->GetBackend() == render::RenderBackend::Vulkan) {
        _renderFinishSemaphores.resize(_backBufferCount);
        for (auto& semaphore : _renderFinishSemaphores) {
            semaphore = StaticCastUniquePtr<render::vulkan::SemaphoreVulkan>(_device->CreateSemaphoreDevice().Unwrap());
        }
        _imageAvailableSemaphores.resize(_inFlightFrameCount);
        for (auto& semaphore : _imageAvailableSemaphores) {
            semaphore = StaticCastUniquePtr<render::vulkan::SemaphoreVulkan>(_device->CreateSemaphoreDevice().Unwrap());
        }
    }
    _defaultRTVs.resize(_backBufferCount);
    for (auto& rtView : _defaultRTVs) {
        rtView.reset();
    }
    _backBuffers.resize(_backBufferCount);
    for (auto& rt : _backBuffers) {
        rt = nullptr;
    }
}

void ImGuiApplication::MainLoop() {
}

void ImGuiApplication::RenderLoop() {
}

void ImGuiApplication::OnStart() {}

void ImGuiApplication::OnDestroy() {}

void ImGuiApplication::OnUpdate() {}

void ImGuiApplication::OnImGui() {}

}  // namespace radray
