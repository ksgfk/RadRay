// #include <radray/runtime/imgui_system.h>

// #include <radray/logger.h>

// #if defined(RADRAY_ENABLE_IMGUI)
// #include <algorithm>
// #include <cmath>
// #include <exception>
// #include <functional>

// #include <imgui.h>

// #if defined(RADRAY_PLATFORM_WINDOWS)
// #include <radray/platform/win32_headers.h>

// #include <backends/imgui_impl_win32.h>

// extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// #ifdef CreateWindow
// #undef CreateWindow
// #endif
// #ifdef FindWindow
// #undef FindWindow
// #endif
// #ifdef IsMinimized
// #undef IsMinimized
// #endif
// #endif
// #endif

// namespace radray {

// #if defined(RADRAY_ENABLE_IMGUI)
// namespace {

// struct ImGuiSystemBackend {
//     ImGuiSystemDesc Desc{};
//     Win32MsgProcHandle MainWin32MsgProc{};
//     bool ViewportsEnabled{false};
// #if defined(RADRAY_PLATFORM_WINDOWS)
//     bool Win32BackendInitialized{false};
// #endif
// };

// struct ImGuiViewportData {
//     AppWindowHandle Window{AppWindowHandle::Invalid()};
//     bool Owned{false};
// };

// ImGuiSystemBackend* g_imguiSystemBackend{nullptr};

// ImGuiSystemBackend* GetBackend() noexcept {
//     if (ImGui::GetCurrentContext() == nullptr) {
//         return nullptr;
//     }
//     return g_imguiSystemBackend;
// }

// ImGuiViewportData* GetViewportData(ImGuiViewport* viewport) noexcept {
//     return viewport != nullptr ? static_cast<ImGuiViewportData*>(viewport->PlatformUserData) : nullptr;
// }

// NativeWindow* GetViewportNativeWindow(ImGuiViewport* viewport) noexcept {
//     ImGuiViewportData* viewportData = GetViewportData(viewport);
//     ImGuiSystemBackend* backend = GetBackend();
//     if (viewportData == nullptr || backend == nullptr || backend->Desc.App == nullptr) {
//         return nullptr;
//     }
//     Nullable<AppWindow*> window = backend->Desc.App->FindWindow(viewportData->Window);
//     if (!window.HasValue() || window.Get()->_window == nullptr) {
//         return nullptr;
//     }
//     return window.Get()->_window.get();
// }

// void UpdateViewportPlatformHandle(ImGuiViewport* viewport, NativeWindow* window) noexcept {
//     if (viewport == nullptr || window == nullptr) {
//         return;
//     }
//     WindowNativeHandler native = window->GetNativeHandler();
//     viewport->PlatformHandle = native.Handle;
//     viewport->PlatformHandleRaw = native.Handle;
// }

// void BindMainViewport(AppWindowHandle windowHandle, NativeWindow* window) {
//     ImGuiViewport* mainViewport = ImGui::GetMainViewport();
//     auto* mainViewportData = new ImGuiViewportData{};
//     mainViewportData->Window = windowHandle;
//     mainViewportData->Owned = false;
//     mainViewport->PlatformUserData = mainViewportData;
//     mainViewport->PlatformWindowCreated = true;
//     UpdateViewportPlatformHandle(mainViewport, window);
// }

// #if defined(RADRAY_PLATFORM_WINDOWS)
// int64_t ImGuiWin32MsgProc(void* hwnd, uint32_t msg, uint64_t wParam, int64_t lParam) noexcept {
//     HWND nativeHwnd = static_cast<HWND>(hwnd);
//     const LRESULT imguiResult = ImGui_ImplWin32_WndProcHandler(nativeHwnd, msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
//     if (imguiResult != 0) {
//         return imguiResult;
//     }

//     if (ImGui::GetCurrentContext() == nullptr) {
//         return 0;
//     }

//     switch (msg) {
//         case WM_CLOSE: {
//             if (ImGuiViewport* viewport = ImGui::FindViewportByPlatformHandle(nativeHwnd)) {
//                 ImGuiViewportData* viewportData = GetViewportData(viewport);
//                 if (viewportData != nullptr && viewportData->Owned) {
//                     viewport->PlatformRequestClose = true;
//                     return 1;
//                 }
//             }
//             return 0;
//         }
//         case WM_MOVE:
//             if (ImGuiViewport* viewport = ImGui::FindViewportByPlatformHandle(nativeHwnd)) {
//                 viewport->PlatformRequestMove = true;
//             }
//             return 0;
//         case WM_SIZE:
//             if (ImGuiViewport* viewport = ImGui::FindViewportByPlatformHandle(nativeHwnd)) {
//                 viewport->PlatformRequestResize = true;
//             }
//             return 0;
//         case WM_MOUSEACTIVATE:
//             if (ImGuiViewport* viewport = ImGui::FindViewportByPlatformHandle(nativeHwnd)) {
//                 if (viewport->Flags & ImGuiViewportFlags_NoFocusOnClick) {
//                     return MA_NOACTIVATE;
//                 }
//             }
//             return 0;
//         case WM_NCHITTEST:
//             if (ImGuiViewport* viewport = ImGui::FindViewportByPlatformHandle(nativeHwnd)) {
//                 if (viewport->Flags & ImGuiViewportFlags_NoInputs) {
//                     return HTTRANSPARENT;
//                 }
//             }
//             return 0;
//         default:
//             return 0;
//     }
// }
// #endif

// void PlatformCreateWindow(ImGuiViewport* viewport) {
//     ImGuiSystemBackend* backend = GetBackend();
//     if (backend == nullptr || backend->Desc.App == nullptr || viewport == nullptr) {
//         return;
//     }

// #if defined(RADRAY_PLATFORM_WINDOWS)
//     AppWindowHandle handle = AppWindowHandle::Invalid();
//     try {
//         vector<std::function<Win32MsgProc>> wndProcs;
//         wndProcs.emplace_back(ImGuiWin32MsgProc);

//         Win32WindowCreateDescriptor windowDesc{};
//         windowDesc.Title = "RadRay ImGui Viewport";
//         windowDesc.Width = std::max(1, static_cast<int32_t>(std::ceil(viewport->Size.x)));
//         windowDesc.Height = std::max(1, static_cast<int32_t>(std::ceil(viewport->Size.y)));
//         windowDesc.X = static_cast<int32_t>(std::floor(viewport->Pos.x));
//         windowDesc.Y = static_cast<int32_t>(std::floor(viewport->Pos.y));
//         windowDesc.Resizable = true;
//         windowDesc.StartVisible = false;
//         windowDesc.ExtraWndProcs = wndProcs;

//         handle = backend->Desc.App->CreateWindow(windowDesc, backend->Desc.ViewportSurfaceDesc, false, backend->Desc.MailboxCount);
//         Nullable<AppWindow*> appWindow = backend->Desc.App->FindWindow(handle);
//         if (!appWindow.HasValue() || appWindow.Get()->_window == nullptr) {
//             if (handle.IsValid()) {
//                 backend->Desc.App->DestroyWindow(handle);
//             }
//             return;
//         }

//         auto* viewportData = new ImGuiViewportData{};
//         viewportData->Window = handle;
//         viewportData->Owned = true;
//         viewport->PlatformUserData = viewportData;
//         appWindow.Get()->_window->SetPosition(windowDesc.X, windowDesc.Y);
//         appWindow.Get()->_window->SetSize(windowDesc.Width, windowDesc.Height);
//         UpdateViewportPlatformHandle(viewport, appWindow.Get()->_window.get());
//         viewport->PlatformRequestResize = false;
//     } catch (...) {
//         if (handle.IsValid()) {
//             try {
//                 backend->Desc.App->DestroyWindow(handle);
//             } catch (...) {
//             }
//         }
//         backend->Desc.App->RequestFatalExit(std::current_exception());
//     }
// #else
//     (void)viewport;
// #endif
// }

// void PlatformDestroyWindow(ImGuiViewport* viewport) {
//     if (viewport == nullptr) {
//         return;
//     }

//     ImGuiSystemBackend* backend = GetBackend();
//     ImGuiViewportData* viewportData = GetViewportData(viewport);
//     if (viewportData != nullptr && viewportData->Owned && backend != nullptr && backend->Desc.App != nullptr) {
//         try {
// #if defined(RADRAY_PLATFORM_WINDOWS)
//             if (::GetCapture() == static_cast<HWND>(viewport->PlatformHandle)) {
//                 ::ReleaseCapture();
//             }
// #endif
//             backend->Desc.App->DestroyWindow(viewportData->Window);
//         } catch (...) {
//             backend->Desc.App->RequestFatalExit(std::current_exception());
//         }
//     }

//     delete viewportData;
//     viewport->PlatformUserData = nullptr;
//     viewport->PlatformHandle = nullptr;
//     viewport->PlatformHandleRaw = nullptr;
// }

// void PlatformShowWindow(ImGuiViewport* viewport) {
//     NativeWindow* window = GetViewportNativeWindow(viewport);
//     if (window == nullptr) {
//         return;
//     }
//     window->Show();
//     if ((viewport->Flags & ImGuiViewportFlags_NoFocusOnAppearing) == 0) {
//         window->Focus();
//     }
// }

// void PlatformSetWindowPos(ImGuiViewport* viewport, ImVec2 pos) {
//     if (NativeWindow* window = GetViewportNativeWindow(viewport)) {
//         window->SetPosition(static_cast<int32_t>(std::floor(pos.x)), static_cast<int32_t>(std::floor(pos.y)));
//     }
// }

// ImVec2 PlatformGetWindowPos(ImGuiViewport* viewport) {
//     if (NativeWindow* window = GetViewportNativeWindow(viewport)) {
//         WindowVec2i pos = window->GetPosition();
//         return ImVec2(static_cast<float>(pos.X), static_cast<float>(pos.Y));
//     }
//     return ImVec2(0.0f, 0.0f);
// }

// void PlatformSetWindowSize(ImGuiViewport* viewport, ImVec2 size) {
//     if (NativeWindow* window = GetViewportNativeWindow(viewport)) {
//         window->SetSize(std::max(1, static_cast<int32_t>(std::ceil(size.x))), std::max(1, static_cast<int32_t>(std::ceil(size.y))));
//     }
// }

// ImVec2 PlatformGetWindowSize(ImGuiViewport* viewport) {
//     if (NativeWindow* window = GetViewportNativeWindow(viewport)) {
//         WindowVec2i size = window->GetSize();
//         return ImVec2(static_cast<float>(size.X), static_cast<float>(size.Y));
//     }
//     return ImVec2(0.0f, 0.0f);
// }

// void PlatformSetWindowFocus(ImGuiViewport* viewport) {
//     if (NativeWindow* window = GetViewportNativeWindow(viewport)) {
//         window->Focus();
//     }
// }

// bool PlatformGetWindowFocus(ImGuiViewport* viewport) {
//     if (NativeWindow* window = GetViewportNativeWindow(viewport)) {
//         return window->IsFocused();
//     }
//     return false;
// }

// bool PlatformGetWindowMinimized(ImGuiViewport* viewport) {
//     if (NativeWindow* window = GetViewportNativeWindow(viewport)) {
//         return window->IsMinimized();
//     }
//     return false;
// }

// void PlatformSetWindowTitle(ImGuiViewport* viewport, const char* title) {
//     if (NativeWindow* window = GetViewportNativeWindow(viewport)) {
//         window->SetTitle(title != nullptr ? std::string_view{title} : std::string_view{});
//     }
// }

// void PlatformSetWindowAlpha(ImGuiViewport* viewport, float alpha) {
//     if (NativeWindow* window = GetViewportNativeWindow(viewport)) {
//         window->SetAlpha(alpha);
//     }
// }

// void PlatformUpdateWindow(ImGuiViewport* viewport) {
//     NativeWindow* window = GetViewportNativeWindow(viewport);
//     if (window == nullptr) {
//         return;
//     }
//     if (window->ShouldClose()) {
//         viewport->PlatformRequestClose = true;
//     }
//     UpdateViewportPlatformHandle(viewport, window);
// }

// float PlatformGetWindowDpiScale(ImGuiViewport* viewport) {
//     if (NativeWindow* window = GetViewportNativeWindow(viewport)) {
//         return window->GetDpiScale();
//     }
//     return 1.0f;
// }

// ImVec2 PlatformGetWindowFramebufferScale(ImGuiViewport*) {
//     return ImVec2(1.0f, 1.0f);
// }

// void InstallPlatformHandlers() noexcept {
//     ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
//     platformIO.Platform_CreateWindow = PlatformCreateWindow;
//     platformIO.Platform_DestroyWindow = PlatformDestroyWindow;
//     platformIO.Platform_ShowWindow = PlatformShowWindow;
//     platformIO.Platform_SetWindowPos = PlatformSetWindowPos;
//     platformIO.Platform_GetWindowPos = PlatformGetWindowPos;
//     platformIO.Platform_SetWindowSize = PlatformSetWindowSize;
//     platformIO.Platform_GetWindowSize = PlatformGetWindowSize;
//     platformIO.Platform_GetWindowFramebufferScale = PlatformGetWindowFramebufferScale;
//     platformIO.Platform_SetWindowFocus = PlatformSetWindowFocus;
//     platformIO.Platform_GetWindowFocus = PlatformGetWindowFocus;
//     platformIO.Platform_GetWindowMinimized = PlatformGetWindowMinimized;
//     platformIO.Platform_SetWindowTitle = PlatformSetWindowTitle;
//     platformIO.Platform_SetWindowAlpha = PlatformSetWindowAlpha;
//     platformIO.Platform_UpdateWindow = PlatformUpdateWindow;
//     platformIO.Platform_GetWindowDpiScale = PlatformGetWindowDpiScale;
// }

// }  // namespace
// #endif

// ImGuiSystem::~ImGuiSystem() noexcept {
//     this->Shutdown();
// }

// bool ImGuiSystem::Initialize(const ImGuiSystemDesc& desc) noexcept {
// #if !defined(RADRAY_ENABLE_IMGUI)
//     (void)desc;
//     RADRAY_ERR_LOG("ImGuiSystem requires RADRAY_ENABLE_IMGUI");
//     return false;
// #else
//     if (_backend != nullptr) {
//         return true;
//     }
//     if (g_imguiSystemBackend != nullptr) {
//         RADRAY_ERR_LOG("only one ImGuiSystem instance can be initialized at a time");
//         return false;
//     }
//     if (ImGui::GetCurrentContext() == nullptr) {
//         RADRAY_ERR_LOG("ImGuiSystem requires an active Dear ImGui context");
//         return false;
//     }
//     if (desc.App == nullptr || !desc.MainWindow.IsValid()) {
//         RADRAY_ERR_LOG("ImGuiSystem requires a valid Application and main window");
//         return false;
//     }
//     if (desc.MailboxCount == 0) {
//         RADRAY_ERR_LOG("ImGuiSystem mailbox count must be greater than zero");
//         return false;
//     }

//     Nullable<AppWindow*> mainWindow = desc.App->FindWindow(desc.MainWindow);
//     if (!mainWindow.HasValue() || mainWindow.Get()->_window == nullptr) {
//         RADRAY_ERR_LOG("ImGuiSystem main window was not found");
//         return false;
//     }

//     ImGuiIO& io = ImGui::GetIO();
//     if (io.BackendPlatformUserData != nullptr) {
//         RADRAY_ERR_LOG("Dear ImGui platform backend is already initialized");
//         return false;
//     }

//     auto backend = make_unique<ImGuiSystemBackend>();
//     backend->Desc = desc;

//     NativeWindow* nativeWindow = mainWindow.Get()->_window.get();
//     WindowNativeHandler native = nativeWindow->GetNativeHandler();

// #if defined(RADRAY_PLATFORM_WINDOWS)
//     if (native.Type != WindowHandlerTag::HWND || native.Handle == nullptr) {
//         RADRAY_ERR_LOG("ImGuiSystem Win32 backend requires a HWND main window");
//         return false;
//     }

//     if (!ImGui_ImplWin32_Init(native.Handle)) {
//         RADRAY_ERR_LOG("ImGui_ImplWin32_Init failed");
//         return false;
//     }
//     backend->Win32BackendInitialized = true;
//     backend->ViewportsEnabled = desc.EnableViewports;

//     ImGui::DestroyPlatformWindows();
// #else
//     backend->ViewportsEnabled = false;
// #endif

//     g_imguiSystemBackend = backend.get();
//     InstallPlatformHandlers();

//     if (desc.EnableDocking) {
//         io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
//     }
//     if (backend->ViewportsEnabled) {
//         io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
//         io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
//     } else {
//         io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
//         io.BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;
//     }

// #if defined(RADRAY_PLATFORM_WINDOWS)
//     backend->MainWin32MsgProc = nativeWindow->AddWin32MsgProc(ImGuiWin32MsgProc);
//     if (!backend->MainWin32MsgProc.IsValid()) {
//         g_imguiSystemBackend = nullptr;
//         io.BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;
//         io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
//         ImGui_ImplWin32_Shutdown();
//         RADRAY_ERR_LOG("failed to add ImGui Win32 message proc");
//         return false;
//     }
// #else
//     ImGui::GetPlatformIO().Monitors.resize(0);
//     ImGuiPlatformMonitor monitor{};
//     monitor.MainSize = ImVec2(static_cast<float>(nativeWindow->GetSize().X), static_cast<float>(nativeWindow->GetSize().Y));
//     monitor.WorkSize = monitor.MainSize;
//     monitor.DpiScale = nativeWindow->GetDpiScale();
//     ImGui::GetPlatformIO().Monitors.push_back(monitor);
// #endif

//     try {
//         BindMainViewport(desc.MainWindow, nativeWindow);
//     } catch (...) {
//         g_imguiSystemBackend = nullptr;
// #if defined(RADRAY_PLATFORM_WINDOWS)
//         nativeWindow->RemoveWin32MsgProc(backend->MainWin32MsgProc);
//         if (backend->Win32BackendInitialized) {
//             io.BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;
//             io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
//             ImGui_ImplWin32_Shutdown();
//         }
// #endif
//         RADRAY_ERR_LOG("failed to bind ImGui main viewport");
//         return false;
//     }

//     _backend = backend.release();
//     return true;
// #endif
// }

// void ImGuiSystem::Shutdown() noexcept {
// #if defined(RADRAY_ENABLE_IMGUI)
//     auto* backend = static_cast<ImGuiSystemBackend*>(_backend);
//     if (backend == nullptr) {
//         return;
//     }

//     if (ImGui::GetCurrentContext() != nullptr) {
//         ImGui::DestroyPlatformWindows();

//         if (backend->Desc.App != nullptr) {
//             Nullable<AppWindow*> mainWindow = backend->Desc.App->FindWindow(backend->Desc.MainWindow);
//             if (mainWindow.HasValue() && mainWindow.Get()->_window != nullptr) {
//                 mainWindow.Get()->_window->RemoveWin32MsgProc(backend->MainWin32MsgProc);
//             }
//         }

//         ImGuiIO& io = ImGui::GetIO();
//         io.BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;
//         io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;

// #if defined(RADRAY_PLATFORM_WINDOWS)
//         if (backend->Win32BackendInitialized) {
//             ImGui_ImplWin32_Shutdown();
//         } else
// #endif
//         {
//             ImGui::GetPlatformIO().ClearPlatformHandlers();
//         }
//     }

//     g_imguiSystemBackend = nullptr;
//     delete backend;
//     _backend = nullptr;
// #endif
// }

// void ImGuiSystem::BeginFrame(float deltaSeconds) noexcept {
// #if defined(RADRAY_ENABLE_IMGUI)
//     auto* backend = static_cast<ImGuiSystemBackend*>(_backend);
//     if (backend == nullptr || ImGui::GetCurrentContext() == nullptr || backend->Desc.App == nullptr) {
//         return;
//     }

// #if defined(RADRAY_PLATFORM_WINDOWS)
//     if (backend->Win32BackendInitialized) {
//         ImGui_ImplWin32_NewFrame();
//         if (deltaSeconds > 0.0f) {
//             ImGui::GetIO().DeltaTime = deltaSeconds;
//         }
//         return;
//     }
// #endif

//     Nullable<AppWindow*> mainWindow = backend->Desc.App->FindWindow(backend->Desc.MainWindow);
//     if (!mainWindow.HasValue() || mainWindow.Get()->_window == nullptr) {
//         return;
//     }

//     NativeWindow* nativeWindow = mainWindow.Get()->_window.get();
//     WindowVec2i size = nativeWindow->GetSize();
//     ImGuiIO& io = ImGui::GetIO();
//     io.DisplaySize = ImVec2(static_cast<float>(size.X), static_cast<float>(size.Y));
//     io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
//     io.DeltaTime = deltaSeconds > 0.0f ? deltaSeconds : (1.0f / 60.0f);
// #endif
// }

// void ImGuiSystem::EndFrame() noexcept {
// #if defined(RADRAY_ENABLE_IMGUI)
//     if (_backend == nullptr || ImGui::GetCurrentContext() == nullptr) {
//         return;
//     }

//     ImGui::UpdatePlatformWindows();
// #endif
// }

// bool ImGuiSystem::IsInitialized() const noexcept {
//     return _backend != nullptr;
// }

// bool ImGuiSystem::IsViewportWindow(AppWindowHandle handle) const noexcept {
// #if !defined(RADRAY_ENABLE_IMGUI)
//     (void)handle;
//     return false;
// #else
//     if (_backend == nullptr || !handle.IsValid() || ImGui::GetCurrentContext() == nullptr) {
//         return false;
//     }

//     ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
//     for (ImGuiViewport* viewport : platformIO.Viewports) {
//         ImGuiViewportData* viewportData = GetViewportData(viewport);
//         if (viewportData != nullptr && viewportData->Window.Id == handle.Id) {
//             return true;
//         }
//     }
//     return false;
// #endif
// }

// }  // namespace radray
