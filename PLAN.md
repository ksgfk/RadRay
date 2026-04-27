# RadRay ImGuiSystem 接入方案

## Summary
- 新增 `radray::ImGuiSystem`，不使用 `radray::imgui` 命名空间；Dear ImGui context 和 `ImGui::NewFrame()` / UI / `ImGui::Render()` 仍由 App 拥有。
- 支持 docking + Win32 多窗口 viewport：ImGui platform window 映射为 `Application` 管理的 secondary `AppWindow`，仍走现有 mailbox、surface recreate、acquire/submit/present 调度。
- 运行期不依赖 DXC/SPIRV-Cross 编译 shader；ImGui shader 由 `tools` 下 Python 脚本离线生成最小 DXIL + SPV 字节码并嵌入 `.cpp`，反射信息手写在 `.cpp` 内。

## Key Interfaces
- 在 `C:\Users\xiaoxs\Desktop\RadRay\modules\runtime\include\radray\runtime\imgui_system.h` 暴露：
  - `ImGuiSystemDesc { Application* App; AppWindowHandle MainWindow; GpuSurfaceDescriptor ViewportSurfaceDesc; uint32_t MailboxCount = 3; bool EnableDocking = true; bool EnableViewports = true; }`
  - `ImGuiSystem::Initialize/Shutdown`
  - `BeginFrame(float deltaSeconds)`：只更新 backend IO、monitor、platform 状态，不调用 `ImGui::NewFrame()`
  - `EndFrame()`：在 App 调用 `ImGui::Render()` 后执行 texture/font 准备和 `ImGui::UpdatePlatformWindows()`，不调用 `ImGui::RenderPlatformWindowsDefault()`
  - `PrepareWindow(AppWindowHandle, uint32_t mailboxSlot)`：深拷贝该 viewport 的 `ImDrawData`
  - `RenderWindow(AppWindowHandle, GpuFrameContext*, uint32_t mailboxSlot, render::TextureState beforeState, render::LoadAction load = Load)`：最后写回 Present
  - `IsViewportWindow(AppWindowHandle)`、`RegisterTexture/UnregisterTexture`
- `Application` 增加最小窗口管理能力：`FindWindow(AppWindowHandle)`、`DestroyWindow(AppWindowHandle)`；destroy secondary window 时进入 render-thread safe point、等待该 window GPU work、清 mailbox/flight/channel，再销毁 surface/window。
- `NativeWindow` 增加 viewport 必需能力：position、title、show/focus、alpha、DPI scale、Win32 runtime message hook、text input；Cocoa 先补空实现/基础实现但不启用多 viewport。

## Implementation
- `ImGuiSystem::Initialize` 绑定主 viewport 到 primary `AppWindow`，设置 `ImGuiBackendFlags_PlatformHasViewports | RendererHasViewports`；Win32 下开启 `ViewportsEnable`，Cocoa 下只允许 docking、关闭 multi-viewport。
- ImGui platform callbacks：
  - main viewport 不创建/销毁 OS window，只绑定已有 primary window。
  - secondary viewport 通过 `Application::CreateWindow(..., isPrimary=false, mailboxCount)` 创建 hidden Win32 window + `GpuSurface`，show 由 `Platform_ShowWindow` 控制。
  - destroy/resize/move/focus/title/alpha/DPI 全部转发到 `NativeWindow`，关闭 secondary window 时设置 ImGui viewport close request，再由 ImGui 销毁。
- 渲染路径：
  - App 在 `OnPrepareRender` 调 `PrepareWindow`，每个 mailbox slot 存一份 CPU draw snapshot，避免 render thread 读取 ImGui 活数据。
  - App 在 `OnRender` 的最后调 `RenderWindow`；它直接用 `GpuFrameContext` 录制最终 overlay pass，不修改 `RenderGraph` 资源模型。
  - 如果 App 自己的 scene/RenderGraph 已写 backbuffer，传入当前 `beforeState`；`RenderWindow` 负责 `beforeState -> RenderTarget -> Present`。
- Renderer 资源：
  - font atlas 初始化为内置 texture id；用户 texture 用 `RegisterTexture` 返回 `ImTextureID`。
  - pipeline 按 backbuffer format 缓存；vertex layout 对应 `ImDrawVert(pos, uv, col)`；开启 alpha blend，无 depth/cull，支持 `VtxOffset`、scissor、clip rect。
  - shader 参数使用一个 16-byte push-constant/ConstantBuffer 记录 scale/translate，另有 texture + sampler 绑定。

## Shader Tooling
- 新增 `C:\Users\xiaoxs\Desktop\RadRay\tools\compile_imgui_shaders.py`，从 `project_manifest.json` 读取 dxc 版本，固定使用 `C:\Users\xiaoxs\Desktop\RadRay\SDKs\dxc\v1.9.2602\extracted\bin\dxc.exe` 形态的 SDKs 内 dxc。
- shader 源放在 `C:\Users\xiaoxs\Desktop\RadRay\shaderlib\imgui\imgui.hlsl`；输出 checked-in `.cpp`，例如 `C:\Users\xiaoxs\Desktop\RadRay\modules\runtime\src\imgui\imgui_shader_data.cpp`。
- DXIL/SPV 都用 release/minimal 参数：`-O3`、不传 `-Zi`，并加 `-Qstrip_debug -Qstrip_reflect`；SPV 额外使用 `-spirv` 和固定 Vulkan target。
- 脚本只更新 `.cpp` 内 bytecode marker 区域；手写 reflection marker 区域保留，由实现者手动填 `ShaderReflectionDesc`，运行期不做反射、不加载 dxc。
- 恢复/新增 `C:\Users\xiaoxs\Desktop\RadRay\tools\CMakeLists.txt`，只提供手动 target 或 no-op，默认 runtime build 不自动跑 shader 编译。

## Test Plan
- 工具校验：运行 `python tools\compile_imgui_shaders.py --check`，确认生成文件与源一致，且 bytecode 中无 debug/reflection blob。
- 构建校验：`cmake --build build_debug --target radrayruntime --verbose`；再配置/构建一次 `RADRAY_ENABLE_DXC=OFF`，确认 ImGui runtime 不依赖 DXC。
- 自动测试：新增 runtime 测试覆盖 shader data/reflection、`ImGuiSystem` 初始化/关闭、secondary viewport 创建销毁、single-thread/multi-thread mailbox snapshot。
- 手动 smoke：Win32 下打开 docking demo，拖出 secondary OS window，测试移动、resize、关闭、multi-thread 切换、swapchain recreate；确认不调用 `ImGui::RenderPlatformWindowsDefault()` 也能由 Application 正常 present。
