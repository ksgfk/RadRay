> - 适用: 不知道从哪开始；要定位某个功能属于哪个模块/文件
> - 权威: 本文是全仓库唯一的地图。任何子系统的细节都不在这里，只给出去哪读
> - 锚点: `CMakeLists.txt`, `modules/CMakeLists.txt`, `cmake/Utility.cmake`

# 全局地图

RadRay 是 C++20 实时渲染器，Windows(D3D12/Vulkan) 为主，macOS 走 Vulkan-on-Metal。
非依赖代码约 6.3 万行，全部在 `modules/`、`tools/`、`shaderlib/`、`examples/`、`benchmarks/` 下。

## 目录职责

| 目录 | 内容 | 可写 |
|---|---|---|
| `modules/` | 全部核心代码，5 个静态库 | 是 |
| `shaderlib/` | HLSL 源码树，同时是 include 根 | 是 |
| `tools/` | Python 脚本 + 两个 C++ CLI（shader_gen / shader_cook） | 是 |
| `examples/` | 可运行 demo（当前全部未启用，见下） | 是 |
| `benchmarks/` | 性能基线，Release 配置默认开启 | 是 |
| `assets/` | 运行时资产（glTF 模型、贴图、obj） | 是 |
| `cmake/` | `Utility.cmake`，全部构建辅助函数 | 是 |
| `docs/` | 本知识库 | 是 |
| `third_party/`, `SDKs/` | 由 `tools/fetch_*.py` 填充的依赖树 | **否** |
| `build_debug/`, `build_release/` | 构建产物 | **否** |

## 模块依赖

```
                 radraycore
                /     |     \
      radraywindow  radrayshader
                \     |
                 \  radrayrender
                  \   /
               radrayruntime
```

依赖是 PUBLIC 的，即 `radrayruntime` 的使用者传递地看到 `core`/`render`/`window`/`shader` 的全部头文件。

| 库 | 行数（inc/src） | 职责 | 去哪读 |
|---|---|---|---|
| `radraycore` | 4.4k / 4.0k | 基础设施：容器别名、分配器、数学、JSON、日志、协程、哈希、图像/网格 IO。不涉及 GPU | `architecture/core-facilities.md` |
| `radraywindow` | 0.6k / 1.3k | 平台窗口与输入抽象（Win32 / Cocoa），sigslot 事件 | — |
| `radrayshader` | 2.4k / 9.0k | shader 工具链：DXC 编译、HLSL/SPIRV/MSL 反射、manifest 格式层、AOT 烘焙。**不触碰 GPU 设备** | `architecture/shader-pipeline.md` |
| `radrayrender` | 4.1k / 12.6k | RHI 抽象 + D3D12/Vulkan 后端实现 | `architecture/render-rhi.md`, ADR-0010/0011/0012 |
| `radrayruntime` | 4.1k / 7.0k | 资产系统、帧节奏与 GPU 上传、渲染框架、Actor/Component、Application | `architecture/asset-system.md`, `architecture/frame-and-gpu.md`, `architecture/render-framework.md` |

**渲染管线尚未接线**：`RenderSystem::_pipeline` 恒为 null，无任何 `RenderPipelinePass` 子类，
`PrimitiveComponent::CreateSceneProxy` 返回 nullptr。详见 `architecture/render-framework.md`。

`radrayshader` 与 `radrayrender` 共用命名空间 `radray::render`，且 `rhi.h` 前向包含 `shader_types.h`，
所以只 `#include <radray/render/rhi.h>` 的旧代码看不出这条模块边界。边界的判定规则见
`architecture/shader-pipeline.md`。

## 关键入口点

按"我要改的东西从哪进来"排列。符号名可直接 Grep。

| 关注点 | 入口 | 文件 |
|---|---|---|
| 进程启动、主循环、帧序 | `Application::Run` → `StartLoop` | `modules/runtime/src/application.cpp` |
| 游戏侧扩展点 | `Application::OnInit` / `OnUpdate` / `OnRenderView` / `OnShutdown` | `modules/runtime/include/radray/runtime/application.h` |
| 创建 GPU 设备（唯一后端分派点） | `Device::Create`（`std::visit` over `DeviceDescriptor`） | `modules/render/src/rhi.cpp` |
| 帧节奏、flight、提交 | `GpuSystem::BeginFrameRecord` / `EndFrameRecordAndSubmit` | `modules/runtime/src/gpu_system.cpp` |
| 资产加载、生命周期回收 | `AssetManager::Load` / `Pump` | `modules/runtime/src/asset_manager.cpp` |
| 渲染管线逐 pass 执行 | `RenderPipeline::Render` | `modules/runtime/src/render_framework/render_pipeline.cpp` |
| 场景 tick | `World::Tick` | `modules/runtime/src/game_framework/world.cpp` |
| shader 源码 → 字节码 | `ShaderResolver` | `modules/shader/src/shader_manifest.cpp` |
| shader AOT 烘焙 | `CookShaderAsset` | `modules/shader/src/shader_manifest.cpp` |
| PSO / PipelineLayout 去重 | `PipelineStateCache`, `PipelineLayoutCache` | `modules/runtime/src/gpu_resource.cpp`, `pipeline_layout_cache.cpp` |
| 服务装配（谁持有谁） | `ServiceRegistry::Add` / `Wire` / `Initialize` | `modules/runtime/include/radray/runtime/service_registry.h` |

## 超大文件与章节导航

这 4 个文件占非依赖代码的 24%，**不要整读**。每个文件顶部有章节索引，正文用
`// == 章节名 ==` 分节。用 `Grep "^// =="` 列出章节后再定向 Read：

| 文件 | 行数 | 章节数 |
|---|---|---|
| `modules/render/src/vk/vulkan_impl.cpp` | 5.6k | 18 |
| `modules/render/src/d3d12/d3d12_impl.cpp` | 4.6k | 17 |
| `modules/shader/src/shader_manifest.cpp` | 3.4k | 18 |
| `modules/render/include/radray/render/rhi.h` | 1.5k | 14 |

## 现状陷阱

不读代码看不出来、但会直接浪费时间的事实：

- **`examples/` 全部未启用**。`examples/CMakeLists.txt` 里两个 `add_subdirectory` 都被注释掉了，
  且整块受 `RADRAY_ENABLE_IMGUI` 保护。改 example 代码不会被编译，得先取消注释。
- **Metal 后端只有声明，没有实现**。`RenderBackend::Metal`、`MetalDeviceDescriptor`、
  `rhi.cpp` 里的 `RADRAY_ENABLE_METAL` 分支都存在，但仓库内没有任何 metal 实现源文件。
  macOS 上走 Vulkan，surface 由 `CAMetalLayer` 提供（`modules/render/src/vk/vulkan_macos_surface.mm`）。
- **CMake 源文件用 GLOB**。新增 `.cpp`/`.h` 后需要重新 configure（`CONFIGURE_DEPENDS` 通常会自动触发）。
  `modules/window` 与 `modules/shader`/`modules/render` 的 src glob 是**非递归**的，
  子目录源文件由平台/后端条件分支显式追加。
- **`shaderlib/` 会被拷进构建输出**。`RADRAY_ENABLE_SHADER_JIT` 下 `radrayshader` 的 POST_BUILD
  会 `rm -rf` 再整目录拷贝到 `$<TARGET_FILE_DIR:radrayshader>/shaderlib`，同时拷 DXC 运行库。
  运行时与 CLI 读的是**那一份副本**，不是仓库里的源目录。
- **`.gitignore` 忽略 `assets`**。仓库里的 assets 目录内容不受版本管理。

## 文档索引

```
docs/
  guide/          怎么做。与现状强绑定，改行为就要改它
    build-test.md         配置/构建/测试/依赖/compile_commands
    cpp-conventions.md    命名、文件组织、接口风格、测试写法、格式化
    dev-env.md            vscode / clangd / 调试器配置
    shader-authoring.md   写 HLSL / 加 keyword / 改 manifest / 跑 gen 与 cook
  architecture/   它是怎么运作的。一律现在时，不写历史
    overview.md           本文
    core-facilities.md    容器别名、Nullable、协程、日志、哈希、JSON、数学
    shader-pipeline.md    manifest 三层、变体系统、AOT 产物、解析与缓存
    shaderlib.md          HLSL 库分层与绑定 ABI
    asset-system.md       引用计数唯一权威、延迟销毁、PipelineLayout 共享
    frame-and-gpu.md      帧序、flight、帧边界等待、上传、PSO 缓存、关停顺序
    render-rhi.md         RHI 所有权、后端选择、绑定模型、barrier、同步
    render-framework.md   渲染管线框架、SceneProxy、Application、ServiceRegistry
  adr/            为什么这样，以及放弃过什么。只追加不修改
```

`guide/` 与 `architecture/` 描述现状，会随代码更新；`adr/` 是冻结的决策记录，带日期，永不修订。
想知道某个设计"为什么不是另一种写法"，先 Glob `docs/adr/*.md` 看文件名，不要去翻 git log。
