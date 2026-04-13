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
        {
            std::unique_lock<std::mutex> l{_gpuMutex, std::defer_lock};
            if (_multiThreaded) {
                l.lock();
            }
            _gpu->ProcessTasks();
        }
        this->HandleSurfaceChanges();
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
    if (appWindow._surface != nullptr && appWindow._surface->IsValid()) {
        appWindow._flightTasks.resize(appWindow._surface->GetFlightFrameCount());
    }
    appWindow._isPrimary = isPrimary;
    auto handle = _windows.Emplace(std::move(appWindow));
    _windows.Get(handle)._selfHandle = handle;
    return handle;
}

void Application::DispatchAllWindowEvents() {
    for (auto& i : _windows.Values()) {
        i._window->DispatchEvents();
    }
}

void Application::HandleSurfaceChanges() {
    RADRAY_ASSERT(_gpu && _gpu->IsValid());
    std::unique_lock<std::mutex> gpuLock{_gpuMutex, std::defer_lock};
    if (_multiThreaded) {
        gpuLock.lock();
    }
    for (auto& window : _windows.Values()) {
        if (!window._pendingResize) {
            continue;
        }
        if (window._window == nullptr || !window._window->IsValid()) {
            throw AppException(fmt::format("Application::HandleSurfaceChanges window {} is invalid", window._selfHandle));
        }
        if (window._surface == nullptr || !window._surface->IsValid()) {
            throw AppException(fmt::format("Application::HandleSurfaceChanges surface for window {} is invalid", window._selfHandle));
        }
        if (window._window->IsMinimized()) {
            continue;
        }
        const auto size = window._window->GetSize();
        if (size.X <= 0 || size.Y <= 0) {
            continue;
        }
        const auto nativeHandler = window._window->GetNativeHandler();
        if (nativeHandler.Handle == nullptr) {
            throw AppException(fmt::format("Application::HandleSurfaceChanges window {} native handle is null", window._selfHandle));
        }
        auto& oldSurface = window._surface;
        const auto swapchainDesc = oldSurface->_swapchain->GetDesc();
        if (swapchainDesc.PresentQueue == nullptr) {
            throw AppException(fmt::format("Application::HandleSurfaceChanges surface for window {} has no present queue", window._selfHandle));
        }
        _gpu->Wait(swapchainDesc.PresentQueue->GetQueueType(), oldSurface->_queueSlot);
        GpuSurfaceDescriptor recreateDesc{};
        recreateDesc.NativeHandler = nativeHandler.Handle;
        recreateDesc.Width = static_cast<uint32_t>(size.X);
        recreateDesc.Height = static_cast<uint32_t>(size.Y);
        recreateDesc.BackBufferCount = swapchainDesc.BackBufferCount;
        recreateDesc.FlightFrameCount = swapchainDesc.FlightFrameCount;
        recreateDesc.Format = swapchainDesc.Format;
        recreateDesc.PresentMode = swapchainDesc.PresentMode;
        recreateDesc.QueueSlot = oldSurface->_queueSlot;
        oldSurface.reset();
        auto recreatedSurface = _gpu->CreateSurface(recreateDesc);
        if (recreatedSurface == nullptr || !recreatedSurface->IsValid()) {
            throw AppException(fmt::format("Application::HandleSurfaceChanges failed to recreate surface for window {}", window._selfHandle));
        }
        window._surface = std::move(recreatedSurface);
        window._flightTasks.clear();
        window._flightTasks.resize(window._surface->GetFlightFrameCount());
        window._nextFreeTaskSlot = 0;
        window._pendingResize = false;
    }
}

}  // namespace radray
