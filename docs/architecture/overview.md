> - 适用: 不知道从哪开始；定位某个功能所属的模块与文档
> - 权威: 本文是仓库地图与当前文档入口，子系统契约在对应页面维护
> - 锚点: `CMakeLists.txt`, `modules/CMakeLists.txt`, `cmake/Utility.cmake`, `AGENTS.md`, `.agents/skills/`

# 全局地图

RadRay 是 C++20 实时渲染器，提供 D3D12 与 Vulkan 后端。仓库工作规则见
[AGENTS.md](../../AGENTS.md)，操作入口见[构建与测试](../guide/build-test.md)。

## 目录职责

| 目录 | 内容 |
|---|---|
| `modules/` | core、shader、window、render、runtime，以及可选 shader compiler client |
| `shaderlib/` | HLSL 数学、BSDF、光照共享库，目录本身是 include 根 |
| `tools/` | 依赖恢复与手动包准备、文档检查、编译数据库脚本和 raw shader compile CLI |
| `benchmarks/` | OBJ 解析、SparseSet 压力与 RDG 上传及帧回收基准 |
| `examples/` | 独立教学程序；[Mesh Drawing Pipeline 样例](../guide/build-test.md#mesh-drawing-pipeline-教学样例) 展示 Scene 输入到离屏 GPU 绘制与 PNG 读回 |
| `cmake/` | 功能与依赖清单、包发现、独立依赖适配、安装导出与构建辅助 |
| `docs/architecture/` | 当前设计、接口边界、术语及必要的设计理由 |
| `docs/guide/` | 使用、开发、验证和文档维护方法 |
| `docs/history/` | 经用户授权保留的计划与开发历史；独立[历史目录](../history/README.md)按需读取 |
| `.agents/skills/` | 随仓库维护的 RadRay doc 与 grill 技能 |
| `assets/` | 被忽略的本地资产根；样例和测试资产通过仓库外渠道分发 |
| `third_party/` | 恢复脚本填充的第三方源码，不手动编辑 |
| `SDKs/` | 依赖准备脚本安装的包与恢复脚本解包的 SDK，不手动编辑 |
| `build*/` | 本机构建产物 |

## 模块依赖

箭头表示依赖方向，只列自有 target：

```text
shader         → core
window         → core
shadercompiler → shader, core                 可选
render         → core
runtime        → render, core
```

`radrayshader` 提供 shader wire contract 与 artifact decoder，不依赖 DXC 或 render。
可选 `radrayshadercompiler` 通过 RadRay DXC fork extension ABI 提供 discovery 与 typed variant
compile；包发现与运行库部署见 [Shader pipeline](shader-pipeline.md)。render 接收原始字节码与显式布局，不依赖 shader 或 compiler。

## 文档索引

| 主题 | 文档 |
|---|---|
| 配置、恢复依赖、构建、测试、编译数据库 | [构建与测试](../guide/build-test.md) |
| 测试取舍、用例形态、GPU 测试约定与 TDD 流程 | [编写测试](../guide/testing.md) |
| 选择基准、Tracy、配对测量、CPU/GPU 与内存口径 | [性能测量](../guide/performance.md) |
| 命名、接口、异常、测试和注释约定 | [C++ 约定](../guide/cpp-conventions.md) |
| IDE、clangd、调试器 | [开发环境](../guide/dev-env.md) |
| 写 HLSL pass、keyword、binding 与 target gate | [Shader authoring](../guide/shader-authoring.md) |
| 文档归属、格式、项目技能与检查 | [文档维护](../guide/documentation.md) |
| 容器、协程、日志、RTTI、数学和 IO | [Core 基础设施](core-facilities.md) |
| HLSL 共享库导航 | [Shaderlib](shaderlib.md) |
| Source contract、wire、target layout 与 compiler 发布边界 | [Shader pipeline](shader-pipeline.md) |
| RHI、后端、barrier 与同步 | [RHI 与后端](render-rhi.md) |
| Runtime 环境、设备、材质核心、Scene 输入、网格命令与多槽帧循环 | [Rendering runtime](runtime.md) |

## 关键代码入口

| 关注点 | 文件 |
|---|---|
| Runtime 后端环境 | `modules/runtime/include/radray/runtime/gpu_env.h` |
| Runtime 设备与队列 | `modules/runtime/include/radray/runtime/gpu_device.h` |
| Runtime 多槽帧循环与 RDG | `modules/runtime/include/radray/runtime/frame_scheduler.h`、`frame_pipeline.h`、`frame_slot.h`、`render_graph_pipeline.h`、`rdg.h` |
| Runtime 场景存储 | `modules/runtime/include/radray/runtime/scene.h` |
| Runtime 材质资源、定义、实例与渲染快照 | `modules/runtime/include/radray/runtime/material.h` |
| Runtime 材质标签表 / 集合 | `modules/runtime/include/radray/runtime/material_tag.h` |
| Runtime Primitive、MeshBatch 与顶点输入 | `modules/runtime/include/radray/runtime/mesh_batch.h`、`vertex_input.h` |
| Runtime 绘制命令数据 | `modules/runtime/include/radray/runtime/mesh_draw_command.h` |
| Runtime Pass Processor 与命令输出 Context | `modules/runtime/include/radray/runtime/mesh_pass_processor.h` |
| Mesh Drawing Pipeline 教学主流程 | `examples/mesh_drawing_pipeline/sample.cpp` |
| GPU 设备与 RHI | `modules/render/src/rhi.cpp` |
| Shader artifact 解码 | `modules/shader/include/radray/shader/shader_artifact.h` |
| Compiler client | `modules/shader_compiler/include/radray/shader_compiler/client.h` |

当前能力与限制在各子系统页面说明。
