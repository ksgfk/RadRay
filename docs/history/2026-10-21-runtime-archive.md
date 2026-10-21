> - 适用: 定位 runtime 完整归档、恢复旧实现、查阅移除范围及验证
> - 权威: 2026-10-21 经用户授权的归档记录；不定义下一版 runtime
> - 锚点: `CMakeLists.txt`, `modules/CMakeLists.txt`, `project_manifest.json`

# Runtime 归档

状态：归档、主线清理及验证已完成。日期为本次归档日期。

## 基线与历史边界

- 归档根提交 A：`751aca4e6a70203e6a62f9de67893b33463b94fc`。
- 直接使用 A 的完整 commit hash 查阅和恢复，不创建归档标签。A 是当前 develop 的祖先提交。
- 压缩前 develop HEAD：`ed23aef93b779082d3efab221bbe52d9f30051f0`，共 235 个祖先提交（含自身）。
- 两者 tree 均为 `5243d82568c806176dba9d9184146a41b887cb7d`；A 无父提交，保存完全相同的已跟踪文件。
- 整理提交 B 是 A 的直接子提交，包含本文、文档迁移与代码移除；A 不再 amend。

本次只改本地 develop。不修改 main、远端分支或 upstream 配置，不新增原历史备份标签，也不清理旧对象。
旧历史报告的 hash 仍表示其原始基线，不能改写为 A：它们不属于 A 的祖先链，后续可恢复性取决于远端旧历史或本地旧对象是否仍存在。
本地 develop 与 origin/develop 已无共同祖先；普通 pull 不适用于将两者重新接续。推送新历史属于后续单独操作。

## 归档设计与资料入口

| 资料 | 保留内容 |
|---|---|
| [重写原因](2026-10-21-runtime-rewrite-rationale.md) | 用户决策、现有基础问题、可借鉴设计与草案对照 |
| [下一版草案原文](2026-10-21-runtime-next-loop-draft.md) | 未定稿输入、来源与原始 SHA-256，不是实施授权 |
| [Runtime 宿主](2026-10-21-runtime-render-framework-snapshot.md) | Application、World/Scene、Renderer、材质、绑定与绘制 |
| [帧与 GPU](2026-10-21-runtime-frame-and-gpu-snapshot.md) | 帧序、flight、上传、资源退休、维护与关停 |
| [资产系统](2026-10-21-runtime-asset-system-snapshot.md) | 引用计数、异步加载与 GPU 使用者保活 |
| [AssetDatabase](2026-10-21-runtime-asset-database-snapshot.md) | JSON 身份库、导入设置与加载桥接 |
| [实例绘制](2026-10-21-runtime-instancing-snapshot.md) | 自动合批、稳定实例集合、native frame 数据与压测 |
| [Shader pipeline](2026-10-21-runtime-shader-pipeline-snapshot.md) | 原 JIT、program cache 与共享 compiler/RHI 契约上下文 |
| [构建与测试](2026-10-21-runtime-build-test-snapshot.md) | 原构建开关、测试目标、运行与回归命令 |
| [性能测量](2026-10-21-runtime-performance-snapshot.md) | 原样例、基准、脚本、测量方法与限制 |
| [共享说明摘录](2026-10-21-runtime-shared-doc-excerpts.md) | RHI、core、shader authoring、IDE 与 C++ 约定中的原 runtime 说明 |
| [旧 ImGui 状态](2026-10-21-runtime-runtime-imgui-snapshot.md) | 归档前已移除的 UI 集成状态及更早历史入口 |
| [旧 Renderer foundation 状态](2026-10-21-runtime-renderer-foundation-snapshot.md) | 归档前已移除的旧框架及更早历史入口 |

归档版本中 runtime 依赖 render/window/shader/core，开启 JIT 时再链接 shadercompiler。
Application 装配各系统；GT 负责世界、资产与场景交付，RT 消费封存数据、准备并提交 GPU 工作。
FrameTimeline 与 GpuSystem 管理 flight 和完成发布；资源不能仅因 CPU 取消或逻辑注销就复用，必须满足真实 GPU 完成及使用者退出条件。
维护与关停同时涉及消息泵、CPU 录制交接、GPU 排空和系统拆除，完整所有权、顺序及限制见上述宿主、帧与资产快照。

## 移除与保留范围

移除 `modules/runtime/` 全部源码、公共头、测试与资源；移除 `examples/` 中 scene_sync、framework_stress、descriptor_reuse 及专属 HLSL。
移除 bench_command_allocator、bench_scene_sync、bench_scene_gpu、bench_raster_prepare、bench_material_scene、bench_lifecycle 六个基准。
移除其专属运行脚本 run_scene_sync_benchmark.py、run_gpu_lifecycle_benchmark.py、run_material_scene_benchmark.py、run_descriptor_reuse.py。
清理 runtime/examples/JIT 的 CMake 选项和入口，以及仅供 runtime 使用的 cgltf target 与 manifest 项。

保留 core、window、render、shader、shader_compiler、共享 shaderlib、raw shader compile CLI、OBJ 与 D3D12 descriptor 基准及 RHI 验证工具。
不回退基础模块，不实现新循环，不保留旧 runtime API 兼容壳。core runtime_type、协程设施与 DXC 运行库部署不属于本次移除范围。
原有历史报告和 CSV/JSON/图片保留原日期、测量结果及基线；只修复入口、链接和失效头部锚点。历史 JSON 中的文档路径使用 JSON 合法的斜杠转义保留原始路径值；解析后的数据不变，这些源清单路径按原基线查阅。

## 恢复方式与依赖

在仓库根执行，目标目录须尚不存在：

```powershell
git show 751aca4e6a70203e6a62f9de67893b33463b94fc:modules/runtime/src/application_loop.cpp
git worktree add --detach ../RadRay-runtime-2026-10-21 751aca4e6a70203e6a62f9de67893b33463b94fc
```

进入新 checkout 后按其构建指南恢复依赖和构建。使用当时的 `project_manifest.json`、CMakePresets 与源码，避免混用清理后的主线配置。
归档 manifest 固定 RadRay DXC `1.9.2609.radray.1` 及 Windows x64 校验值；shader schema 8、extension ABI 5 的具体边界见 shader 快照。
GTest discovery 要求 CMake >= 4.4.1；Windows 支持的 MSVC/Ninja 与 ClangCL/Visual Studio 预设见构建快照。
manifest 的分支依赖并非不可变锁；被忽略的 `project_manifest.lock.json` 不包含在 A 中，不承诺未来拉取完全相同的第三方提交。

`assets/`、`third_party/`、`SDKs/`、旧构建目录、exe/DLL 和 trace 均不由本次 Git 快照备份，也未被修改。
资产通过原仓库外渠道获取；没有资产或原始日志时须明确复现限制，不能以历史报告代替新的实际运行。

## 本次验证

环境：Windows x64、Ninja、CMake 4.4.4、MSVC 19.51.36260.0。全部使用本次新建的独立构建目录，按构建完成后测试的顺序串行执行。
D3D12 与 Vulkan 的实际测试均执行；没有运行已移除 runtime 的测试或基准。

| 配置 | 构建与测试结果 |
|---|---|
| Debug，compiler ON，benchmarks OFF | 全量构建成功；314 项注册，313 通过、1 跳过、0 失败；CTest 106.59 秒 |
| Debug，compiler OFF，benchmarks OFF | 全量构建成功；270 项注册，269 通过、1 跳过、0 失败；CTest 97.55 秒；构建树无 dxcompiler.dll |
| Release，benchmarks ON | bench_read_obj、bench_d3d12_descriptor 均构建成功；未运行性能测量 |

两套 Debug 的唯一跳过项均为 `Backends/NativeRenderScopeTest.VulkanNativeReusePinsBudgetAndLocalViewInvalidation`
在 D3D12 参数下的实例，原始理由为 `Vulkan native cache`。它是后端适用性跳过，不是 Vulkan 不可用，也未计入通过数。

实际命令（Windows x64 开发者环境）：

```powershell
cmake --preset win-x64-debug -B build_runtime_archive_debug -DRADRAY_BUILD_BENCHMARKS=OFF
cmake --build build_runtime_archive_debug --config Debug --parallel 8
ctest --test-dir build_runtime_archive_debug -C Debug --output-on-failure --output-junit build_runtime_archive_ops/tests-on.xml
cmake --preset win-x64-debug -B build_runtime_archive_no_compiler -DRADRAY_BUILD_SHADER_COMPILER=OFF -DRADRAY_BUILD_BENCHMARKS=OFF
cmake --build build_runtime_archive_no_compiler --config Debug --parallel 8
ctest --test-dir build_runtime_archive_no_compiler -C Debug --output-on-failure --output-junit build_runtime_archive_ops/tests-off.xml
cmake --preset win-x64-release -B build_runtime_archive_release -DRADRAY_BUILD_BENCHMARKS=ON
cmake --build build_runtime_archive_release --config Release --target bench_read_obj bench_d3d12_descriptor --parallel 8
python tools/check_docs.py
git diff --check
git diff --cached --check
```

实际 CTest 的 JUnit 输出参数使用上述仓库相对位置的绝对路径。汇总与 JUnit 校验值见
[验证摘要](2026-10-21-runtime-archive-validation.json)。完整配置、构建、CTest 日志及 JUnit XML 位于本机
`build_runtime_archive_ops/`，不随 checkout 分发。

文档检查与暂存区 diff 空白检查通过；当前代码/构建无旧 runtime 依赖。
归档 tree 一致性、无父提交、当前分支祖先关系已验证；基础模块源码和文档检查器未修改。
草案附件原文在暂存 blob 中与输入字节完全一致；该文件单独保留 CRLF，不改变其他文档的换行规则。
原有历史 JSON 逐份与 A 比较，解析数据一致。
