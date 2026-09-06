> - 适用: 接入可选 runtime UI、字体、平台窗口、资产图片或本帧 Graph 图片
> - 权威: 本文描述 RadRay 的 ImGui 适配契约；通用帧同步见 [帧与 GPU](frame-and-gpu.md)，Graph 规则见 [Renderer foundation](renderer-foundation.md)
> - 锚点: `modules/runtime/cmake/imgui.cmake`, `modules/runtime/include/radray/runtime/imgui/imgui_config.h`, `modules/runtime/include/radray/runtime/imgui/imgui_system.h`, `modules/runtime/include/radray/runtime/imgui/imgui_graph.h`, `modules/runtime/src/imgui/imgui_system.cpp`, `modules/runtime/src/imgui/imgui_graph.cpp`, `modules/runtime/tests/test_imgui_rendering.cpp`, `shaderlib/ui/`, `tools/generate_imgui_shaders.py`

# 可选 runtime ImGui

依赖固定为 ImGui `v1.92.9b-docking` 和 FreeType `VER-2-14-3`，由依赖恢复脚本填充。
只支持 Win32 runtime 的 D3D12 / Vulkan；不编译官方 Win32、DX12 或 Vulkan backend。
上游配置与 API 以该 tag 的 [imconfig.h](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/imconfig.h)
和 [imgui.h](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/imgui.h) 为准。

## 装配与剥离

`RADRAY_ENABLE_IMGUI` 默认 OFF，开启要求 Win32 runtime。ImGui 适配仍属于 runtime：公开头在
`modules/runtime/include/radray/runtime/imgui/`，实现、私有头与内置 artifact 在
`modules/runtime/src/imgui/`，依赖配置由 runtime 按开关包含 `cmake/imgui.cmake`。
头文件与 `.cpp` 由 runtime 常规规则收集，内容均以 `RADRAY_ENABLE_IMGUI` 包裹。
OFF 时头文件可直接包含且不引入依赖，源码编译为空翻译单元；不检查 ImGui/FreeType 源码，
不创建其 target，也不传播其 include、链接或编译定义。通用窗口、输入路由、输出请求及区域拷贝
仍属于原模块。

FreeType 子目录既关闭可选依赖发现，也在局部变量作用域屏蔽父项目的 PNG、zlib 等发现结果，
避免其 CMake 在禁用发现后继续消费已有的 `*_FOUND`。项目其他模块的依赖配置保持独立。

ON 时 `radray_imgui` 静态库的 PUBLIC 使用要求向 runtime 和消费者传播 `RADRAY_ENABLE_IMGUI`
与唯一的 `IMGUI_USER_CONFIG`，保证上游源码、适配层及消费者的 WCHAR、断言、字体后端和
诊断裁剪宏一致。消费者链接
`radrayruntime`，显式包含可选头；不要独立编译另一份 ImGui 或覆盖其 ABI 宏。

编译启用不等于实例启用。`ApplicationRuntimeDescriptor::ImGui.Enabled` 默认 false；
关闭实例不创建 context、UI 上传页或平台窗口。`ConfigureImGui` 在 context 创建前调整描述符；
`OnImGui` 在主线程的有效帧内调用。应用在 `OnInit` 显式选择管线：

```cpp
#ifdef RADRAY_ENABLE_IMGUI
void ConfigureImGui(ImGuiSystemDescriptor& descriptor) override { descriptor.Enabled = true; }
void OnInit() override {
    GetRenderSystem()->SetPipeline(make_unique<ImGuiOnlyPipeline>(*GetImGuiSystem().Get()));
}
void OnImGui() override {
    ImGui::Begin("Tools");
    ImGui::TextUnformatted("RadRay runtime");
    ImGui::End();
}
#endif
```

一个 Application 拥有一个 context，全部 ImGui API 与平台回调只在创建线程执行。
UI 由 [Application 固定帧序](frame-and-gpu.md#帧序) 驱动，runner 拒绝模态消息造成的嵌套 Tick。
窗口创建、输出注册在主线程且 render idle 时发生。平台子窗口经 WindowManager 创建，
AppWindow 管理 swapchain，输出用途为 Auxiliary。窗口标题、透明度、owner、装饰、焦点、
位置和大小由平台回调转发；不使用 `RenderPlatformWindowsDefault()`，提交与 Present 只有原有路径。

正常关停先等待 GPU，调用应用 `OnShutdown` 释放 UI 注册与应用引用，再释放 UI flight 引用、
GPU 纹理、平台子窗口与 context。部分初始化失败也销毁已经创建的 context 和窗口。

## 显式组合 Graph

ForwardPipeline（含 Tidal Atrium 与 Pipeline Probe）在同一张图中按以下顺序装配：

1. `ImGuiGraph::PrepareSceneOutputs` 将有相机的场景输出路由到可采样中间纹理。
2. 原场景 `BuildGraph` 完成光照、后处理、tone mapping 和输出分辨率重建。
3. `ImGuiGraph::BuildGraph` 添加动态纹理上传、UI 绘制、线性合成和最终输出 Pass。
4. 原 `ExecuteGraph` 执行一次；随后 `ImGuiGraph::CompleteGraph` 记录这次执行结果。

纯工具程序显式装配 `ImGuiOnlyPipeline`。自定义管线可以传入 `ImGuiSceneOutput`，明确场景纹理
**采样后数值**的编码；没有场景输入的 viewport 使用深色背景。无相机 UI 通过
`RenderWorkloadBuilder::RequestOutput` 请求呈现，场景和 UI 共享 output 时只 acquire 一次。

UI 在输出尺寸的 RGBA16_FLOAT 显示线性目标上混合，顶点颜色从 sRGB 解码；纹理按其描述符
解码。FreeType 覆盖率只影响 alpha，UI 不参与场景曝光、TAA、Bloom 或 RenderScale。
UNORM 最终目标显式编码 sRGB，sRGB attachment 由硬件编码；sRGB 纹理视图已经硬件解码，
因此注册为 `ImGuiColorEncoding::Linear`，不能再次标成 Srgb。

绘制保留标准 `ImDrawVert`、16 位 `ImDrawIdx`、`IdxOffset` 与 `VtxOffset`，处理 DisplayPos、
FramebufferScale、桌面负坐标及裁剪。Vulkan 继续使用统一的 `MakeViewport` Y 方向契约。
关闭深度和剔除，颜色用 SrcAlpha / OneMinusSrcAlpha，alpha 用 One / OneMinusSrcAlpha。
顶点、索引经 `MappedUploadPage` / `HostWriteBatch`，常量和 descriptor 经 Graph parameter set；
全部保留到对应 flight 安全复用。

支持 `ImGuiPlatformIO` 的 ResetRenderState、SetSamplerLinear、SetSamplerNearest 回调标记，
自定义 sampler 通过 `ImGuiTextureDescriptor::Sampler` 提供。任意原生绘制回调使快照无效并诊断；
额外 GPU 工作必须显式声明 Graph Pass。

## 图片与动态字体纹理

`ImTextureID` 保持默认 64 位，0 无效。高 32 位为 generation，低 32 位为 slot + 1，
不保存 descriptor 或 RHI 指针。generation 耗尽的 slot 不再复用。

| 入口 | 所有权与使用方式 |
|---|---|
| `RegisterTexture(StreamingAssetRef<TextureAsset>)` | 注册资产引用；实际绘制要求资产已经就绪，发布快照保留引用 |
| `RegisterTexture(shared_ptr<ImGuiTextureLease>)` | lease 独占 RHI texture 和状态追踪；调用者交付已正确初始化的资源与初始状态，随后不能从图外改变其状态 |
| `RegisterOutput(RenderOutputId)` | 显示本帧相机输出的场景纹理；编码由场景输出提供，UI 在合成前取样；不替调用者创建相机或拥有输出 |
| `CreateGraphImage()` | 主线程取得稳定逻辑 ID；渲染线程每帧通过 `ImGuiGraphImageBinding` 绑定本图的 `RgTextureViewHandle` |
| `UnregisterTexture(id)` | 阻止未来快照使用该 ID；已发布 flight 继续拥有纹理，重复注销或旧 generation 返回 false |

RegisterOutput 的 source 必须在本帧 scene outputs 中有且只有一个生产者；缺失或重复时诊断并拒绝图。输出所有者负责
保持注册与资源到 flight fence。主场景也可在 ImGui 中预览：读取合成前的独立场景纹理，避免 UI 反馈。

Graph image 的 ID 可以跨帧保留，Graph view handle 只能在所属图使用。实际 Image 必须有唯一
有效绑定、正确初始化、Resource usage，且是单采样 2D 纹理。缺失、重复、跨图、MSAA、非法
view range 或 attachment 反馈均失败关闭；MSAA 先用 typed resolve Pass 转成单采样图像。
Graph 从参数绑定自动声明 sampled read 依赖，不需要外部手写 barrier。

1.92 动态纹理遵循 WantCreate / WantUpdates / WantDestroy。主线程复制完整 RGBA32 像素、
局部区域与请求版本；Alpha8 展开为白色 RGB 加原 alpha。`TexRef._TexData` 在快照时转换成
逻辑 ID，渲染线程不会读取活的 ImTextureData、ImDrawData、viewport 或 context。

带 `UseColors` 的动态图集按 sRGB RGB、线性 alpha 解释，通过 sRGB 纹理在过滤前解码，符合
[FreeType 彩色位图契约](https://freetype.org/freetype2/docs/reference/ft2-basic_types.html#ft_pixel_mode)。
首次加入彩色字形导致格式变化时完整上传到新资源，旧 flight 继续保留原资源；覆盖率图集保持 UNORM。

上传结果必须同时满足 Graph 执行成功、所有上传 Pass 实际执行、对应 flight 真正完成，
主线程才对同版本请求调用 SetTexID / SetStatus。未执行、丢弃、编译失败的快照不会确认；
请求继续保留并重试。QueueUserData 在待处理期间阻止上游提前退休纹理，atlas 扩容的旧资源、
局部更新上传页及注销前的引用由真实 flight 保活，UnusedFrames 不作为 GPU 安全依据。

## 编译配置

只有依赖和裁剪项提供 CMake 开关，其余采用初始化配置与标准 ImGui API。

| 配置 | 当前设置 |
|---|---|
| `RADRAY_IMGUI_USE_FREETYPE` | ON；OFF 使用内置 STB。FreeType 禁用 ZLIB/BZIP2/PNG/HARFBUZZ/BROTLI 自动发现，SVG 扩展关闭 |
| `RADRAY_IMGUI_DEMO_WINDOWS` / `RADRAY_IMGUI_DEBUG_TOOLS` | 均 ON；OFF 分别定义 IMGUI_DISABLE_DEMO_WINDOWS / IMGUI_DISABLE_DEBUG_TOOLS |
| IMGUI_API / IMGUI_DISABLE | 静态默认；不定义 IMGUI_DISABLE，上游依赖由 CMake 剥离，适配层由 RADRAY_ENABLE_IMGUI 条件编译 |
| IM_ASSERT | 接入 RadRay 日志与终止，Release 保留 |
| 旧 API / 默认分配器 | 禁用；context 创建前绑定 radray::Malloc / Free |
| 默认 Win32 clipboard、IME、shell | 禁用，使用窗口能力；shell callback 为空 |
| 默认 OSX clipboard | 不启用；ImGui runtime 仅 Win32 |
| 默认时间、文件、格式化、数学函数 | 保留上游；工程自有代码使用 fmt；自动 ini/log 路径单独禁用 |
| WCHAR / 顶点色 / 索引 / TextureID | WCHAR32；RGBA；16 位；默认 64 位与 invalid 0 |
| 默认 bitmap/vector 字体 | 均保留，无外部字体也能启动 |
| FreeType/STB 宏 | FreeType 模式只定义 IMGUI_ENABLE_FREETYPE，不额外强制 STB；无 STB 文件或实现替换 |
| 自定义顶点、回调签名、Eigen 转换、数学运算符 | 均不覆盖或全局注入 |
| 旧 CRC、STB sprintf、SSE 禁用、Test Engine、自动用户头、paranoid、全量 ID 扫描 | 均不启用；IM_DEBUG_BREAK 保留平台默认 |

使用官方 `imgui_stdlib` 文本编辑适配。初始化执行 `IMGUI_CHECKVERSION()` 并静态检查顶点、索引、
TextureID 布局。开启 Demo/Debug 裁剪不改变模块的快照验证和失败诊断。

## IO、样式与字体默认值

| 配置 | 默认值 |
|---|---|
| Keyboard / Gamepad navigation | true / false，后端不声明 HasGamepad |
| Docking / Viewports | true / true，描述符可关闭 |
| NoMouse / NoKeyboard / NoMouseCursorChange | false / false / false，后续应用修改由 ImGui 和适配层遵守 |
| IsSRGB / IsTouchScreen / MacOSXBehaviors | true / false / false |
| NavSwapGamepadButtons / NavMoveSetMousePos / NavCaptureKeyboard | false / false / true |
| Escape 清 item / window focus；nav cursor 自动 / 常显 | true / false；true / false |
| Docking NoSplit / NoDockingOver / WithShift / AlwaysTabBar / TransparentPayload | 全 false |
| Viewports NoAutoMerge / NoTaskBarIcon / NoDecoration / NoDefaultParent | false / true / true / false |
| 平台焦点同步 ImGui focus；DPI 字体 / viewport 自动缩放 | true；true / true |
| DisplayFramebufferScale | Win32 为 (1,1)，DPI 不再次乘进 framebuffer 像素尺寸 |
| 输入 trickle / 文本光标闪烁 / Enter 保留编辑 / Drag 单击转文本 | true / true / false / false |
| ColorEdit | DefaultOptions_ |
| 边缘 resize / 仅标题栏移动 / Ctrl+C 复制整窗 / 滚动条按页 | true / true / false / true |
| MouseDrawCursor / FontAllowUserScaling | false / false |
| 内存收缩 / 双击时间 / 双击距离 / drag 阈值 | 60 s / .30 s / 6 / 6 |
| 单击延迟 / key repeat 延迟 / 间隔 | .50 s / .275 s / .050 s，单击延迟显式覆盖该版本构造值 |
| 错误恢复及 assert/log/tooltip；ID 冲突提示和 Item Picker | 全开启，Debug tools 裁剪遵循上游禁用宏 |
| Begin 返回值故障注入 / 忽略失焦 | 全关闭 |

Dark 样式，16 像素基础字号，FontScaleMain=1，WindowRounding=0，窗口背景 alpha=1。
`SetStyleScale` 从保存的未缩放基线重建间距和主字号缩放，保留当前 FontScaleDpi，避免累计误差。
自动跨屏缩放遵循上游字体和 viewport 契约，不承诺间距自动跨屏等比缩放。

字体由 `ImGuiFontDescriptor::Path` 显式提供 TTF/OTF/TTC，字体 bytes 复制到 ImGui 分配器，
由 atlas 持有至销毁。`Config` 支持 MergeMode、FontNo、hinting、偏移、advance 与 rasterizer
参数；排除范围复制进模块自有存储。默认不合并、FontNo=0、FreeType flags=0，不合成粗体、
斜体、单色、彩色或 bitmap 模式。PixelSnap=false，oversampling=auto，atlas flags=None，
格式 RGBA32，最大纹理尺寸来自设备。中文字体由应用或样例 `--font` 指定，不分发系统字体。

IniFilename 与 LogFilename 均为空。非空 SettingsPath 经项目 IO 显式加载，5 秒保存间隔和正常
关停时保存；空路径不持久化。ini 日期=true、自动淘汰月份=0、调试注释=false。
可调用标准 ImGui API 添加配置与工具，变更样式后调用 SetStyleScale 会回到模块保存的基线。

## Shader 产物与验证

UI shader 源位于 `shaderlib/ui/`，内置 DXIL / SPIR-V 使用当前 schema、decoder、layout recipe、
ShaderProgram 与 PSO cache。生成文件携带源及 include SHA256 和 artifact 身份；普通构建不运行 DXC。
重生成、一致性检查和 compiler-off 命令见[构建与测试](../guide/build-test.md#可选-imgui)。

`ImGuiRenderingTest` 使用真实 GPU readback 验证线性混合、UNORM/sRGB、offset、大顶点数、
图内图像、Alpha8、彩色图集格式切换和线性过滤、atlas 扩容、局部更新、丢帧和错误图不确认上传。平台窗口回归覆盖 Auxiliary、
resize、最小化恢复、关闭重建及模态重入。实机中文 IME、跨窗口拖动、混合 DPI/多显示器和
显示器热插拔仍需要交互环境验证；自动化调用平台 API 不能代替这些人工观察。
