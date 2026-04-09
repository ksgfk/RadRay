#include <radray/runtime/application.h>

#include <radray/logger.h>
#include <radray/window/native_window.h>

namespace radray {

Application::Application(AppConfig config, IAppCallbacks* callbacks)
    : _config(config),
      _callbacks(callbacks) {}

Application::~Application() noexcept {
    StopRenderThread();
}

int Application::Run() {
    // ─── 窗口 ───
    _window = _config.Window;
    if (_window == nullptr) {
        RADRAY_ERR_LOG("Application: Window is null in AppConfig");
        return 1;
    }

    // ─── 创建 GpuRuntime ───
    {
        bool created = false;
        switch (_config.Backend) {
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
            case render::RenderBackend::D3D12: {
                render::D3D12DeviceDescriptor desc{};
                desc.AdapterIndex = std::nullopt;
#ifdef RADRAY_IS_DEBUG
                desc.IsEnableDebugLayer = true;
                desc.IsEnableGpuBasedValid = true;
#endif
                auto opt = GpuRuntime::Create(desc);
                if (opt.HasValue()) {
                    _gpu = opt.Release();
                    created = true;
                }
                break;
            }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
            case render::RenderBackend::Vulkan: {
                render::VulkanInstanceDescriptor instanceDesc{};
                instanceDesc.AppName = "RadRay";
                instanceDesc.AppVersion = 1;
                instanceDesc.EngineName = "RadRay";
                instanceDesc.EngineVersion = 1;
#ifdef RADRAY_IS_DEBUG
                instanceDesc.IsEnableDebugLayer = true;
#endif
                render::VulkanCommandQueueDescriptor queueDesc{};
                queueDesc.Type = render::QueueType::Direct;
                queueDesc.Count = 1;
                render::VulkanDeviceDescriptor deviceDesc{};
                deviceDesc.PhysicalDeviceIndex = std::nullopt;
                deviceDesc.Queues = std::span{&queueDesc, 1};
                auto opt = GpuRuntime::Create(deviceDesc, instanceDesc);
                if (opt.HasValue()) {
                    _gpu = opt.Release();
                    created = true;
                }
                break;
            }
#endif
            default:
                break;
        }
        if (!created) {
            RADRAY_ERR_LOG("Application: failed to create GpuRuntime for backend {}", static_cast<int>(_config.Backend));
            return 1;
        }
    }

    // ─── 创建 Surface ───
    {
        auto nativeHandler = _window->GetNativeHandler();
        auto size = _window->GetSize();
        uint32_t w = size.X > 0 ? static_cast<uint32_t>(size.X) : 1;
        uint32_t h = size.Y > 0 ? static_cast<uint32_t>(size.Y) : 1;
        _surface = _gpu->CreateSurface(
            nativeHandler.Handle,
            w, h,
            _config.BackBufferCount,
            _config.FlightFrameCount,
            _config.SurfaceFormat,
            _config.PresentMode);
        if (_surface == nullptr || !_surface->IsValid()) {
            RADRAY_ERR_LOG("Application: created invalid surface");
            _gpu->Destroy();
            _gpu.reset();
            return 1;
        }
    }

    // ─── 初始化 ───
    _multiThreaded = _config.MultiThreadedRender;
    _allowFrameDrop = _config.AllowFrameDrop;
    _pendingPresentMode = _config.PresentMode;

    if (_multiThreaded) {
        SwitchThreadMode(true);
    }

    // 连接 resize 信号
    _window->EventResized().connect([this](int w, int h) {
        if (w > 0 && h > 0) {
            _pendingResize = true;
            _pendingWidth = static_cast<uint32_t>(w);
            _pendingHeight = static_cast<uint32_t>(h);
        }
    });

    _callbacks->OnStartup(this);

    _timer.Restart();

    // ═══════════════ 主循环 ═══════════════
    while (!_exitRequested && !_window->ShouldClose()) {
        _window->DispatchEvents();

        // 计时
        auto elapsed = _timer.Elapsed();
        _timer.Restart();
        float dt = std::chrono::duration<float>(elapsed).count();
        _frameInfo.DeltaTime = dt;
        _frameInfo.TotalTime += dt;
        _frameInfo.LogicFrameIndex++;

        // Phase 0: 处理 pending 状态变更
        HandlePendingChanges();

        // Phase 1: 逻辑帧
        _callbacks->OnUpdate(this, dt);

        // Phase 2: 同步上一帧渲染（多线程时）
        if (_multiThreaded) {
            WaitRenderComplete();
            HandlePresentResult(_lastPresentResult);
        }

        _gpu->ProcessTasks();

        // Phase 3: 窗口最小化 → 跳过渲染
        if (_window->IsMinimized()) {
            continue;
        }

        // 如果 surface 无效（被标记重建但还没完成），跳过
        if (_surface == nullptr || !_surface->IsValid()) {
            continue;
        }

        // Phase 4: 尝试获取帧
        GpuRuntime::BeginFrameResult result{};
        if (_allowFrameDrop) {
            result = _gpu->TryBeginFrame(_surface.get());
        } else {
            result = _gpu->BeginFrame(_surface.get());
        }

        switch (result.Status) {
            case render::SwapChainStatus::Success: {
                auto ctx = result.Context.Release();
                AppFrameInfo snapshot = _frameInfo;
                snapshot.RenderFrameIndex = ++_frameInfo.RenderFrameIndex;

                auto packet = _callbacks->OnExtractRenderData(this, snapshot);

                if (_multiThreaded) {
                    KickRenderThread(std::move(ctx), snapshot, std::move(packet));
                } else {
                    _callbacks->OnRender(this, ctx.get(), snapshot, packet.get());
                    render::SwapChainPresentResult presentResult{};
                    if (ctx->IsEmpty()) {
                        auto submitResult = _gpu->AbandonFrame(std::move(ctx));
                        presentResult = submitResult.Present;
                    } else {
                        auto submitResult = _gpu->SubmitFrame(std::move(ctx));
                        presentResult = submitResult.Present;
                    }
                    HandlePresentResult(presentResult);
                }
                break;
            }
            case render::SwapChainStatus::RetryLater: {
                _frameInfo.DroppedFrameCount++;
                break;
            }
            case render::SwapChainStatus::RequireRecreate: {
                _pendingSurfaceRecreate = true;
                break;
            }
            case render::SwapChainStatus::Error:
            default: {
                RADRAY_ERR_LOG("Application: BeginFrame returned error status {}", static_cast<int>(result.Status));
                _exitRequested = true;
                break;
            }
        }
    }

    // ═══════════════ 关闭 ═══════════════
    if (_multiThreaded) {
        WaitRenderComplete();
    }
    StopRenderThread();

    if (_surface != nullptr && _surface->IsValid()) {
        _gpu->Wait(render::QueueType::Direct, _surface->_queueSlot);
    }
    _gpu->ProcessTasks();

    _callbacks->OnShutdown(this);

    _surface.reset();
    _gpu->Destroy();
    _gpu.reset();
    _window = nullptr;

    return 0;
}

// ─── 运行时控制 ───

void Application::SetMultiThreadedRender(bool enable) {
    _pendingThreadModeSwitch = enable;
}

void Application::SetAllowFrameDrop(bool enable) {
    _allowFrameDrop = enable;
}

void Application::RequestPresentModeChange(render::PresentMode mode) {
    if (mode != GetCurrentPresentMode()) {
        _pendingPresentMode = mode;
        _pendingSurfaceRecreate = true;
    }
}

void Application::RequestExit() {
    _exitRequested = true;
}

// ─── 只读查询 ───

GpuRuntime* Application::GetGpuRuntime() const {
    return _gpu.get();
}

NativeWindow* Application::GetWindow() const {
    return _window;
}

GpuSurface* Application::GetSurface() const {
    return _surface.get();
}

const AppFrameInfo& Application::GetFrameInfo() const {
    return _frameInfo;
}

bool Application::IsMultiThreadedRender() const {
    return _multiThreaded;
}

bool Application::IsFrameDropEnabled() const {
    return _allowFrameDrop;
}

render::PresentMode Application::GetCurrentPresentMode() const {
    if (_surface != nullptr && _surface->IsValid()) {
        return _surface->GetPresentMode();
    }
    return _config.PresentMode;
}

// ─── pending 变更处理 ───

void Application::HandlePendingChanges() {
    bool needSync = _pendingSurfaceRecreate || _pendingResize || _pendingThreadModeSwitch.has_value();
    if (!needSync) {
        return;
    }

    // 确保渲染线程空闲
    if (_multiThreaded) {
        WaitRenderComplete();
    }

    // Surface 重建（resize / present mode 切换）
    if (_pendingSurfaceRecreate || _pendingResize) {
        uint32_t w = _pendingResize ? _pendingWidth : _surface->GetWidth();
        uint32_t h = _pendingResize ? _pendingHeight : _surface->GetHeight();
        auto mode = _pendingSurfaceRecreate ? _pendingPresentMode : _surface->GetPresentMode();
        RecreateSurface(w, h, mode);
        _pendingSurfaceRecreate = false;
        _pendingResize = false;
    }

    // 线程模式切换
    if (_pendingThreadModeSwitch.has_value()) {
        SwitchThreadMode(*_pendingThreadModeSwitch);
        _pendingThreadModeSwitch.reset();
    }
}

void Application::RecreateSurface(uint32_t width, uint32_t height, render::PresentMode mode) {
    // drain GPU 队列
    if (_surface != nullptr && _surface->IsValid()) {
        _gpu->Wait(render::QueueType::Direct, _surface->_queueSlot);
    }
    _gpu->ProcessTasks();

    // 销毁旧 surface
    _surface.reset();

    // 创建新 surface
    auto nativeHandler = _window->GetNativeHandler();
    _surface = _gpu->CreateSurface(
        nativeHandler.Handle,
        width, height,
        _config.BackBufferCount,
        _config.FlightFrameCount,
        _config.SurfaceFormat,
        mode);

    if (_surface != nullptr && _surface->IsValid()) {
        _callbacks->OnSurfaceRecreated(this, _surface.get());
    }
}

void Application::SwitchThreadMode(bool enableMultiThread) {
    if (enableMultiThread && !_multiThreaded) {
        _renderThreadStop = false;
        _renderHasWork = false;
        _renderDone = true;
        _renderThread = std::thread(&Application::RenderThreadMain, this);
        _multiThreaded = true;
    } else if (!enableMultiThread && _multiThreaded) {
        StopRenderThread();
        _multiThreaded = false;
    }
}

// ─── 渲染线程 ───

void Application::RenderThreadMain() {
    while (true) {
        unique_ptr<GpuFrameContext> ctx;
        AppFrameInfo info{};
        unique_ptr<FramePacket> packet;

        // 等待工作
        {
            std::unique_lock lock(_renderMutex);
            _renderKickCV.wait(lock, [this] { return _renderHasWork || _renderThreadStop; });
            if (_renderThreadStop && !_renderHasWork) {
                break;
            }
            ctx = std::move(_renderWork.Ctx);
            info = _renderWork.Info;
            packet = std::move(_renderWork.Packet);
            _renderHasWork = false;
        }

        // 执行渲染
        _callbacks->OnRender(this, ctx.get(), info, packet.get());

        // 提交
        render::SwapChainPresentResult presentResult{};
        if (ctx->IsEmpty()) {
            auto submitResult = _gpu->AbandonFrame(std::move(ctx));
            presentResult = submitResult.Present;
        } else {
            auto submitResult = _gpu->SubmitFrame(std::move(ctx));
            presentResult = submitResult.Present;
        }

        // 通知完成
        {
            std::lock_guard lock(_renderMutex);
            _lastPresentResult = presentResult;
            _renderDone = true;
        }
        _renderDoneCV.notify_one();
    }
}

void Application::KickRenderThread(unique_ptr<GpuFrameContext> ctx, AppFrameInfo info, unique_ptr<FramePacket> packet) {
    {
        std::lock_guard lock(_renderMutex);
        _renderWork.Ctx = std::move(ctx);
        _renderWork.Info = info;
        _renderWork.Packet = std::move(packet);
        _renderHasWork = true;
        _renderDone = false;
    }
    _renderKickCV.notify_one();
}

void Application::WaitRenderComplete() {
    std::unique_lock lock(_renderMutex);
    _renderDoneCV.wait(lock, [this] { return _renderDone; });
}

void Application::StopRenderThread() {
    if (!_renderThread.joinable()) {
        return;
    }
    {
        std::lock_guard lock(_renderMutex);
        _renderThreadStop = true;
    }
    _renderKickCV.notify_one();
    _renderThread.join();
    _renderThreadStop = false;
}

void Application::HandlePresentResult(render::SwapChainPresentResult result) {
    switch (result.Status) {
        case render::SwapChainStatus::RequireRecreate:
            _pendingSurfaceRecreate = true;
            break;
        case render::SwapChainStatus::Error:
            RADRAY_ERR_LOG("Application: present returned error");
            _exitRequested = true;
            break;
        default:
            break;
    }
}

}  // namespace radray
