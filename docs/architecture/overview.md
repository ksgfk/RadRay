> - 适用: 不知道从哪开始；要定位某个功能属于哪个模块/文件
> - 权威: 本文是全仓库唯一的地图。任何子系统的细节不在这里，只给出去哪读
> - 锚点: `CMakeLists.txt`, `modules/CMakeLists.txt`, `cmake/Utility.cmake`

# 全局地图

RadRay 是 C++20 实时渲染器，当前支持 Windows 下的 D3D12 与 Vulkan 后端。
构建产物、依赖恢复和测试命令以 `docs/guide/build-test.md` 为准。

## 目录职责

| 目录 | 内容 | 可写 |
|---|---|---|
| `modules/` | core、shader、window、render、runtime，以及可选 shader compiler client | 是 |
| `shaderlib/` | HLSL 数学、光照共享库与最小产品 pass，目录本身是 include 根 | 是 |
| `tools/` | 依赖恢复、文档检查、编译数据库脚本，以及可选的 raw shader compile CLI | 是 |
| `assets/` | 本地运行时资产 | 是 |
| `cmake/` | 构建辅助函数 | 是 |
| `docs/` | 当前架构、指南、ADR 和研究记录 | 是 |
| `third_party/`, `SDKs/` | 由恢复脚本填充的依赖树 | **否** |
| `build_*/` | 构建产物 | **否** |

## 模块依赖

```text
                 radraycore
                /     |     \
      radraywindow  radrayshader  radrayshadercompiler
                       |
                  radrayrender
                       |
                   radrayruntime
```

| 库 | 职责 | 去哪读 |
|---|---|---|
| `radraycore` | 容器别名、分配器、数学、JSON、日志、协程、哈希和 IO | `architecture/core-facilities.md` |
| `radrayshader` | compiler/render 共享的 shader wire contract、artifact decoder 与双 target view；只依赖 core，不依赖 DXC | `architecture/shader-pipeline.md` |
| `radraywindow` | 窗口、输入和平台事件 | `guide/dev-env.md` |
| `radrayshadercompiler` | 可选 source-contract discovery 与 DXC boundary client；依赖 shader 与 core，不拥有 render/runtime | `docs/todo/hlsl-radray-dxc-shader-pipeline.md`, `docs/todo/filesystem-backed-shader-include-correction.md`, `docs/todo/radray-dxc-frontend-semantic-migration.md` |
| `radrayrender` | RHI、D3D12/Vulkan 后端、资源、命令和 PSO | `architecture/render-rhi.md` |
| `radrayruntime` | 资产生命周期、帧节奏、渲染框架和 Application | `architecture/asset-system.md`, `architecture/frame-and-gpu.md`, `architecture/render-framework.md` |

`radrayshadercompiler` 只在 `RADRAY_BUILD_SHADER_COMPILER=ON` 时进入构建图；它依赖
`radraycore` 与 `radrayshader`，不反向依赖 render/runtime。当前 target 已提供 source-contract
discovery、typed variant compile、RadRay DXC fork extension client 和 extension probe；client
只用 fork extension ABI，无 stock adapter。`tools/fetch_sdks.py` 按 `project_manifest.json` 准备
本地包，正式配置按 Manifest `Name` 使用 `SDKs/radray_dxc/extracted` 并发现
`RadRayDXC::Headers/Compiler`，不把 DXC 源码加入 RadRay build graph。shader wire
contract 与 artifact decoder 由 `radrayshader` 提供，因此 render 与 runtime 解析
compiler-produced metadata 不依赖 `radrayshadercompiler`；`radrayruntime` 仅在
`RADRAY_ENABLE_SHADER_JIT=ON`（默认）时 PUBLIC 链接它以驱动开发期 JIT。

## 关键入口

| 关注点 | 入口 | 文件 |
|---|---|---|
| 进程启动与主循环 | `Application::Run` → `StartLoop` | `modules/runtime/src/application.cpp` |
| 创建 GPU 设备 | `Device::Create` | `modules/render/src/rhi.cpp` |
| 帧节奏、flight、提交 | `GpuSystem::BeginFrameRecord` / `EndFrameRecordAndSubmit` | `modules/runtime/src/gpu_system.cpp` |
| 资产加载与回收 | `AssetManager::Load` / `Pump` | `modules/runtime/src/asset_manager.cpp` |
| render pass / framebuffer 去重 | `RenderPassRegistry` | `modules/render/include/radray/render/render_pass_registry.h` |
| 场景 tick | `World::Tick` | `modules/runtime/src/game_framework/world.cpp` |

## 当前边界

M-1 已移除旧的 shader 资产、手写 metadata、旧的命令行 shader 工具和未接线的 UI 路线。
当前 `shaderlib/` 由共享数学层、target gate 和三条最小产品 pass 组成；compiler-owned
metadata、target-native artifact decoder/layout 与 runtime JIT 已接通。RHI 的 layout 构造入口
按 DXIL/SPIR-V view 分开，公共绑定提交使用当前 artifact 颁发的不透明 `BindingHandle`。
`radray_shader_compile`（`RADRAY_BUILD_SHADER_TOOLS` 默认 ON）只输出 raw DXIL/SPIR-V
metadata blob，不生成 artifact index、cook 或 publisher；client 只用 fork extension ABI，
无 stock adapter（测试内的 stock DXC 仅作为 Pso smoke 的 bytecode 来源）。正式 cook/artifact
发布链和 RadRay 自身安装导出层仍不在当前工作树。

## 文档索引

```text
docs/
  guide/
    build-test.md       配置、构建、测试、依赖和 compile_commands
    shader-authoring.md 新 HLSL pass、keyword 与 target gate 约定
    cpp-conventions.md  命名、接口风格和测试约定
    dev-env.md          IDE、clangd 和调试器设置
  architecture/
    overview.md         本文
    core-facilities.md  core 提供的容器、协程、日志和基础设施
    shaderlib.md        当前 HLSL 共享库边界
    shader-pipeline.md  source contract、双 target wire、decoder 与 JIT 边界
    asset-system.md     资产引用计数与延迟销毁
    asset-database.md   asset 元数据 LMDB 存储与 AssetDatabase 身份登记
    frame-and-gpu.md    帧序、flight、上传和关停
    render-rhi.md       RHI、后端、barrier 和同步
    render-framework.md 渲染框架、SceneProxy、Application、ServiceRegistry
  adr/                  设计决策记录，只追加不修改
  todo/                 有范围和检查站的实施计划
```

`guide/` 与 `architecture/` 描述当前状态；`adr/` 记录为什么采用某个设计；`research/`
只保存带版本和范围的外部证据，不替代当前契约。
