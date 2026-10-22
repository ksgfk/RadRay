> - 适用: 不知道从哪开始；定位某个功能所属的模块与文档
> - 权威: 本文是仓库地图与完整文档索引，子系统契约在对应页面维护
> - 锚点: `CMakeLists.txt`, `modules/CMakeLists.txt`, `cmake/Utility.cmake`, `AGENTS.md`, `.agents/skills/`

# 全局地图

RadRay 是 C++20 实时渲染器，提供 D3D12 与 Vulkan 后端。仓库工作规则见
[AGENTS.md](../../AGENTS.md)，操作入口见[构建与测试](../guide/build-test.md)。

## 目录职责

| 目录 | 内容 |
|---|---|
| `modules/` | core、shader、window、render，以及可选 shader compiler client |
| `shaderlib/` | HLSL 数学、BSDF、光照共享库，目录本身是 include 根 |
| `tools/` | 依赖恢复与手动包准备、文档检查、编译数据库脚本和 raw shader compile CLI |
| `benchmarks/` | OBJ 解析性能测量 |
| `cmake/` | 功能与依赖清单、包发现、独立依赖适配、安装导出与构建辅助 |
| `docs/architecture/` | 当前设计、接口边界、术语及必要的设计理由 |
| `docs/guide/` | 使用、开发、验证和文档维护方法 |
| `docs/history/` | 经用户授权保留的开发历史；日期前缀命名，注明状态与基线 |
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
```

`radrayshader` 提供 shader wire contract 与 artifact decoder，不依赖 DXC 或 render。
可选 `radrayshadercompiler` 通过 RadRay DXC fork extension ABI 提供 discovery 与 typed variant
compile；包发现与运行库部署见 [Shader pipeline](shader-pipeline.md)。render 接收原始字节码与显式布局，不依赖 shader 或 compiler。

## 文档索引

| 主题 | 文档 |
|---|---|
| 配置、恢复依赖、构建、测试、编译数据库 | [构建与测试](../guide/build-test.md) |
| 选择基准、Tracy、配对测量、CPU/GPU 与内存口径 | [性能测量](../guide/performance.md) |
| 命名、接口、异常、测试和注释约定 | [C++ 约定](../guide/cpp-conventions.md) |
| IDE、clangd、调试器 | [开发环境](../guide/dev-env.md) |
| 写 HLSL pass、keyword、binding 与 target gate | [Shader authoring](../guide/shader-authoring.md) |
| 文档归属、格式、项目技能与检查 | [文档维护](../guide/documentation.md) |
| 容器、协程、日志、RTTI、数学和 IO | [Core 基础设施](core-facilities.md) |
| HLSL 共享库导航 | [Shaderlib](shaderlib.md) |
| Source contract、wire、target layout 与 compiler 发布边界 | [Shader pipeline](shader-pipeline.md) |
| RHI、后端、barrier 与同步 | [RHI 与后端](render-rhi.md) |

## 历史资料

记录与附件统一保存在 `docs/history/`。经用户明确要求或同意后新增；AI 可询问是否留档，日常改动不自动写入。[归档规则](../guide/documentation.md#开发历史)说明命名与日期选择。

以下文件保留历史设计、实验结果、原始数据和验收覆盖；内容仅适用于记录中的基线。当前契约见上面的子系统页面。
CSV/JSON/图片与报告一起保留；`build*/` 中未入库的日志和 trace 不保证随 checkout 存在。

| 历史资料 | 保留价值与当前归属 |
|---|---|
| [显式 RHI 绑定布局转向与实施计划](../history/2026-10-22-explicit-rhi-binding-layout-rationale.md) | shader artifact 布局权威转向调用方定义的原因、实施前所有权快照、当时的接口与验收要求；当前直接所有权和取消缓存决定见 RHI 文档 |
| [Runtime 完整归档](../history/2026-10-21-runtime-archive.md) | 源码恢复、设计与操作快照、草案附件及归档时验证入口 |
| [Runtime 重写原因](../history/2026-10-21-runtime-rewrite-rationale.md) | 补丁叠加的基础问题、用户决策、源码核查与下一版方向 |
| [旧渲染框架设计](../history/2026-09-14-render-framework-design.md) | a68237f 时的分层、数据流与重构范围 |
| [旧渲染框架附件](../history/2026-09-14-render-framework-legacy.md) | 原 Scene/proxy、Forward、服务和材质契约 |
| [旧 Renderer foundation 附件](../history/2026-09-14-renderer-foundation-legacy.md) | 原 RenderGraph、pool/history、draw list |
| [旧 ImGui 附件](../history/2026-09-14-runtime-imgui-legacy.md) | 原 UI 模块的集成、输入和字体设计 |
| [生命周期 v2 验收](../history/2026-09-20-lifecycle-v2-validation.md) | 2026-09-20 覆盖边界与 CSV |
| [Scene 变换实验](../history/2026-09-23-scene-transform-benchmark.md) | 世界矩阵转到 RT 的收益和代价 |
| [直接导入实验与回退](../history/2026-09-23-scene-direct-import-benchmark.md) | 未采纳的 freeze/ack、全量求值方案及配对数据 |
| [World 紧凑变换存储](../history/2026-09-24-world-transform-store-benchmark.md) | 已采纳布局的性能与接口迁移记录，含 JSON 原始结果 |
| [100k 场景诊断](../history/2026-10-06-scene-sync-100k-performance.md) | 自动实例化之前的 copy/draw 瓶颈；后续实现不能沿用旧帧率 |
| [第一阶段验收](../history/2026-10-07-phase1-rhi-command-allocator-validation.md) | allocator/pipeline 接入时的覆盖与 CSV |
| [材质和实例化验收](../history/2026-10-13-material-instancing-performance.md) | 双后端 CPU/GPU、churn 和内存对照，含回退项及图表 |
| [GPU 生命周期输入计划](../history/2026-10-13-gpu-lifecycle-plan-vulkan12-per-flight.md) | 7a807 基线的迁移目标与验收矩阵，已执行且后续 API 有变化 |
| [GPU 生命周期实施验收](../history/2026-10-13-gpu-lifecycle-implementation.md) | 2026-10-13–14 的实际覆盖、限制及性能证据；不替代当前 RHI |
| [帧循环重构输入计划](../history/2026-10-16-frame-loop-refactor-plan.md) | 93a8117 基线的旧等待关系、职责与确认后的重构契约 |
| [帧循环重构实施验收](../history/2026-10-16-frame-loop-refactor-validation.md) | 消息/进度状态机、延迟输入、GPU 排空的测试与性能证据及限制 |
| [Render Framework 重构输入](../history/2026-10-16-render-framework-refactor-plan.md) | f20c56f 与 sampler 工作区补丁的所有权快照、完整组复用目标与前后验收口径 |
| [Render Framework 重构验收](../history/2026-10-16-render-framework-refactor-validation.md) | 双后端 descriptor 前后 42 项、真实完成与退出确认、完整回归、Release 性能回退及环境限制 |
| [Render Framework 回退定位](../history/2026-10-16-render-framework-regression-diagnosis.md) | 未改生产代码的分段计时、CPU 完成清理与固定绑定值规模实验；定位重复请求展开与临时对象周转 |

## 关键代码入口

| 关注点 | 文件 |
|---|---|
| GPU 设备与 RHI | `modules/render/src/rhi.cpp` |
| Shader artifact 解码 | `modules/shader/include/radray/shader/shader_artifact.h` |
| Compiler client | `modules/shader_compiler/include/radray/shader_compiler/client.h` |

当前能力与限制在各子系统页面说明。
