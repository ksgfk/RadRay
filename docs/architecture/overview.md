> - 适用: 不知道从哪开始；定位某个功能所属的模块与文档
> - 权威: 本文是仓库地图与完整文档索引，子系统契约在对应页面维护
> - 锚点: `CMakeLists.txt`, `modules/CMakeLists.txt`, `cmake/Utility.cmake`, `AGENTS.md`, `.agents/skills/`

# 全局地图

RadRay 是 C++20 实时渲染器，提供 D3D12 与 Vulkan 后端。仓库工作规则见
[AGENTS.md](../../AGENTS.md)，操作入口见[构建与测试](../guide/build-test.md)。

## 目录职责

| 目录 | 内容 |
|---|---|
| `modules/` | core、shader、window、render、runtime，以及可选 shader compiler client |
| `shaderlib/` | HLSL 数学、BSDF、光照、阴影共享库，目录本身是 include 根 |
| `tools/` | 依赖恢复、文档检查、编译数据库脚本和 raw shader compile CLI |
| `benchmarks/` | 性能测量程序 |
| `cmake/` | 构建辅助函数 |
| `docs/architecture/` | 当前设计、接口边界、术语及必要的设计理由 |
| `docs/guide/` | 使用、开发、验证和文档维护方法 |
| `docs/temp/` | 标明状态与基线的临时设计快照、草案和交接 |
| `.agents/skills/` | 随仓库维护的 RadRay doc 与 grill 技能 |
| `assets/` | 被忽略的本地资产根；样例和测试资产通过仓库外渠道分发 |
| `third_party/`、`SDKs/` | 恢复脚本填充的只读依赖树 |
| `build*/` | 本机构建产物 |

## 模块依赖

箭头表示依赖方向，只列自有 target：

```text
shader         → core
window         → core
shadercompiler → shader, core                 可选
render         → shader, core
runtime        → render, window, shader, core
runtime        → shadercompiler               仅 RADRAY_ENABLE_SHADER_JIT=ON
```

`radrayshader` 提供 compiler/render 共享 wire contract 与 artifact decoder，不依赖 DXC。
可选 `radrayshadercompiler` 通过 RadRay DXC fork extension ABI 提供 discovery 与 typed variant
compile；包发现与运行库部署见 [Shader pipeline](shader-pipeline.md)。render/runtime 解析
artifact 不需要 compiler，开发期 JIT 通过配置开关接入 client。

## 文档索引

| 主题 | 文档 |
|---|---|
| 配置、恢复依赖、构建、测试、编译数据库 | [构建与测试](../guide/build-test.md) |
| 命名、接口、异常、测试和注释约定 | [C++ 约定](../guide/cpp-conventions.md) |
| IDE、clangd、调试器 | [开发环境](../guide/dev-env.md) |
| 写 HLSL pass、keyword、binding 与 target gate | [Shader authoring](../guide/shader-authoring.md) |
| 文档归属、格式、项目技能与检查 | [文档维护](../guide/documentation.md) |
| 容器、协程、日志、RTTI、数学和 IO | [Core 基础设施](core-facilities.md) |
| HLSL 共享库导航 | [Shaderlib](shaderlib.md) |
| Source contract、wire、target layout、JIT 与发布边界 | [Shader pipeline](shader-pipeline.md) |
| 资产引用计数、加载与延迟销毁 | [资产系统](asset-system.md) |
| JSON 身份库、importer/settings 与加载桥接 | [AssetDatabase](asset-database.md) |
| 帧序、flight、上传与关停 | [帧与 GPU](frame-and-gpu.md) |
| ImGui 移除状态及旧设计 | [Runtime ImGui](runtime-imgui.md) |
| RHI、后端、barrier 与同步 | [RHI 与后端](render-rhi.md) |
| Application、多 World、场景同步、shader 与渲染服务 | [Runtime 宿主](render-framework.md) |
| Renderer foundation 移除状态 | [Renderer foundation](renderer-foundation.md) |
| 重构前设计、依赖边界与历史附件 | [临时设计快照](../temp/render-framework-design.md) |

## 关键代码入口

| 关注点 | 入口 | 文件 |
|---|---|---|
| 主循环 | `Application::Run` / `StartLoop` | `modules/runtime/src/application.cpp` |
| GPU 设备 | `Device::Create` | `modules/render/src/rhi.cpp` |
| flight 与提交 | `GpuSystem::BeginFrameRecord` / `EndFrameRecordAndSubmit` | `modules/runtime/src/gpu_system.cpp` |
| 资产加载与回收 | `AssetManager::Load` / `Pump` | `modules/runtime/src/asset_manager.cpp` |
| 身份登记与 path 反查 | `AssetDatabase::Open` / `Refresh` / `Save` | `modules/runtime/src/asset_database.cpp` |
| 多 World 管理 | `WorldManager::CreateWorld` / `DestroyWorld` | `modules/runtime/src/world_manager.cpp` |
| 场景交付 | `RenderSystem::SealFrameGT` / `ConsumeRenderUpdates` | `modules/runtime/src/render_system.cpp` |
| 场景 tick | `World::Tick` | `modules/runtime/src/game_framework/world.cpp` |

旧 render_framework、Forward 和 ImGui 渲染实现已移除；基础模块、runtime 宿主与游戏组件保留。
当前能力与未实现边界在各子系统页面说明，不在地图中维护另一份进度或决策清单。
