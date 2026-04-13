#include <radray/runtime/application.h>

#include <ranges>

#include <radray/logger.h>

namespace radray {

int32_t Application::Run(int argc, char* argv[]) {
    this->OnInitialize();
    RADRAY_ASSERT(_gpu && _gpu->IsValid());
    RADRAY_ASSERT(std::ranges::any_of(_windows.Values(), [](const auto& w) { return w._isPrimary; }));

    while (!_exitRequested) {
        this->DispatchAllWindowEvents();
        if (_exitRequested) {
            break;
        }
        this->OnUpdate();
        if (_exitRequested) {
            break;
        }
    }

    return 0;
}

void Application::OnShutdown() {
    _gpu.reset();
    _windows.Clear();
}

void Application::CreateGpuRuntime(const render::DeviceDescriptor& deviceDesc, std::optional<render::VulkanInstanceDescriptor> vkInsDesc) {
    RADRAY_ASSERT(!_gpu);
    unique_ptr<render::InstanceVulkan> vkIns;
    if (vkInsDesc.has_value()) {
        auto vkInsOpt = render::CreateVulkanInstance(*vkInsDesc);
        if (!vkInsOpt) {
            throw AppException("Failed to create Vulkan instance");
        }
        vkIns = vkInsOpt.Release();
    }
    shared_ptr<render::Device> device;
    {
        auto deviceOpt = render::CreateDevice(deviceDesc);
        if (!deviceOpt) {
            throw AppException("Failed to create Vulkan device");
        }
        device = deviceOpt.Release();
    }
    _gpu = make_unique<GpuRuntime>(std::move(device), std::move(vkIns));
}

AppWindowHandle Application::CreateWindow(const NativeWindowCreateDescriptor& windowDesc, const GpuSurfaceDescriptor& surfaceDesc_, bool isPrimary) {
    RADRAY_ASSERT(_gpu && _gpu->IsValid());
    RADRAY_ASSERT(std::ranges::any_of(_windows.Values(), [](const auto& w) { return w._isPrimary; }) == false || isPrimary == false);
    auto windowOpt = CreateNativeWindow(windowDesc);
    if (!windowOpt) {
        throw AppException("Failed to create native window");
    }
    auto window = windowOpt.Release();
    auto surfaceDesc = surfaceDesc_;
    surfaceDesc.NativeHandler = window->GetNativeHandler().Handle;
    auto surface = _gpu->CreateSurface(surfaceDesc);
    AppWindow appWindow{};
    appWindow._window = std::move(window);
    appWindow._surface = std::move(surface);
    appWindow._isPrimary = isPrimary;
    return _windows.Emplace(std::move(appWindow));
}

void Application::DispatchAllWindowEvents() {
    for (auto& i : _windows.Values()) {
        i._window->DispatchEvents();
    }
}

}  // namespace radray
