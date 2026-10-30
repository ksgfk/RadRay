> - 适用: 查阅 2026-10-31 精简前的 runtime 设计、对象关系、执行流程和源码恢复入口
> - 权威: 删除前实现的历史快照，不作为当前契约或下一版设计要求；当前范围见 runtime 长期文档
> - 锚点: `modules/runtime/CMakeLists.txt`, `modules/runtime/include/radray/runtime/environment.h`, `modules/runtime/include/radray/runtime/device.h`

# Runtime 设计归档：保留环境与设备之前

## 状态、基线与范围

- 记录日期：2026-10-31。
- 源码基线：`07d41dcf80d63316a512be848eeae9781dddb1e6`；归档开始时工作树干净。
- 状态：本文记录基线中已经实现的 runtime，在删除相关源码之前落盘。
- 用户决定：先将当前设计保存到 history，然后 runtime 仅保留 `Environment`、`DeviceContext` 及模块构建入口，其他部分删除，后续重新设计。
- 本文不制定下一版 Scene、材质、Mesh Pass 或 RDG 的接口；讨论过 UE 的对象关系不代表决定迁入 UE 的设计。
- 当前接口边界见 [Rendering runtime](../architecture/runtime.md)。2026-10-21 的 [Runtime 完整归档](2026-10-21-runtime-archive.md) 属于更早的实现，不能与本次基线混用。

## 源码与文档恢复入口

以下路径均指基线提交中的文件。每组 `.h` 和 `.cpp` 同名对应，头文件在
`modules/runtime/include/radray/runtime/`，实现文件在 `modules/runtime/src/`。

| 文件名 | 基线职责 | 本次处理范围 |
|---|---|---|
| `environment.h` / `environment.cpp` | 后端环境、DXGI factory / Vulkan instance | 保留 |
| `device.h` / `device.cpp` | Device、Direct 队列、外部设备身份匹配与能力查询 | 保留 |
| `scene.h` / `scene.cpp` | 场景更新、primitive 关联、命令缓存、排序与合批 | 删除 |
| `resources.h` / `resources.cpp` | 几何页、索引分配、staging 与 SceneUpload | 删除 |
| `renderer.h` / `renderer.cpp` | Pipeline 接口、PreparedMeshPass、帧槽与提交事务 | 删除 |
| `graph.h` / `graph.cpp` | 图声明、依赖校验、裁剪、状态与屏障计划 | 删除 |
| `graph_execution.h` / `graph_execution.cpp` | 临时资源池、图录制、PassContext 与 extraction | 删除 |
| `pipeline_cache.h` / `pipeline_cache.cpp` | graphics PSO 描述符缓存 | 删除 |

配套文件为 `modules/runtime/tests/CMakeLists.txt`、`test_mesh_pipeline.cpp`、`test_graph.cpp`、
`test_runtime_gpu.cpp`、`tests/data/graph_sample.hlsl`、`tests/data/graph_compute.hlsl` 和
`tools/compile_runtime_test_shaders.py`。三个测试源文件位于 `modules/runtime/tests/`。
它们随被移除的功能一起删除，不保留空测试 target 或旧 shader 生成入口。

删除前的完整长期说明和操作说明也保存在基线中：

```powershell
git show 07d41dcf80d63316a512be848eeae9781dddb1e6:docs/architecture/runtime.md
git show 07d41dcf80d63316a512be848eeae9781dddb1e6:docs/guide/build-test.md
git show 07d41dcf80d63316a512be848eeae9781dddb1e6:modules/runtime/include/radray/runtime/scene.h
git show 07d41dcf80d63316a512be848eeae9781dddb1e6:modules/runtime/src/renderer.cpp
git ls-tree -r --name-only 07d41dcf80d63316a512be848eeae9781dddb1e6 -- modules/runtime tools/compile_runtime_test_shaders.py
```

其他旧源码同样使用 `git show <基线>:<路径>` 查阅，不依赖本机 build 目录或另存源码副本。

## 分层与依赖

`RadRay::runtime` 是 C++20 静态库，实际 target 为 `radrayruntime`，公共依赖 `RadRay::render`，
并通过它依赖 core。公共头使用 `<radray/runtime/...>`，类型位于 `namespace radray`；RHI 类型位于
`radray::render`。模块不依赖 shader compiler、DXC SDK、应用 bridge 或应用顶点语义。

基线文档记录该分层来自 Raynium 中参考 UE 5.8.4 的最小框架，并非完整 UE Renderer 的移植：

```text
应用：SceneUpdate + ViewSubmission + 自定义 Pipeline / shader / 参数
    ↓
Scene → MeshPassProcessor → 缓存的 MeshDrawCommand
    ↓
GeometryStore + BuildDrawList → PreparedMeshPass
    ↓
IRenderPipeline::BuildGraph → GraphBuilder → CompiledGraph
    ↓
GraphExecution → RHI command buffer → Direct queue
```

应用负责可见物体、实例字节布局、shader 字节码、布局、固定状态、资源绑定和具体渲染算法。
runtime 负责更新校验、绘制准备、图资源依赖、录制、提交及在途资源寿命。
失败通过日志和 `bool` / `Nullable` 表达，不沿接口传递错误字符串。

## Mesh、Material、Primitive 与 Pass 的关系

`SceneUpdate` 提交普通 ID 的索引、mesh、material、primitive 增量与删除项。
`MeshUpdate` 包含顶点字节、stride、layout、索引 buffer ID 和多个 `DrawRange`。
`MaterialDescriptor` 包含 ID、program key、逻辑资源索引和不透明参数字节。

`PrimitiveUpdate { id, mesh, material }` 将一个 primitive 关联到一个 mesh 和一个 material：

```text
多个 Primitive → 一个 Mesh → 多个 submesh / DrawRange
多个 Primitive → 一个 Material
VisibleItem → 一个 Primitive + 一个 submesh + 一个实例区间
```

mesh 本身不保存材质。多个 primitive 可以共用同一 mesh，也可以使用不同材质；多个 mesh 对应的
primitive 也可共用材质。一个 primitive 选出的所有 submesh 使用该 primitive 的同一材质。
没有 mesh 材质槽或每个 submesh 单独关联 material 的结构。

`ViewSubmission` 提供本帧可见的 `VisibleItem` 列表、实例字节流与 stride。实例变换、相机、
包围盒和可见性由应用解释与计算，runtime 不自行剔除物体。

材质没有内置 pass mask，也不自动筛选深度、阴影、主颜色或透明 pass。
每次 `Render` 从 Pipeline 取得一个 `MeshPassProcessor`，准备一份 `PreparedMeshPass`。
`BuildGraph` 可以组织多个 pass，但每个 pass 的分类规则和绑定由应用决定；
`AddMeshPass` 收集传入对象的资源访问，按值捕获 prepared 数据，回调时调用它的 `Draw`。
它不额外按 material 过滤绘制列表。命令缓存按 primitive 保存，不是按 primitive × pass 保存。

基线的 `MeshBatch` 是 mesh 的布局、索引和范围记录；`MeshDrawCommand` 是逻辑绘制描述，
保存 mesh/material ID、版本、program、layout、element、range 和材质描述，不含图句柄或帧指针。
实际 GPU PSO、参数集与 buffer 在本帧 `PreparedMeshPass` / `MeshBinding` 中落实。

## 场景更新、缓存与绘制准备

`Scene::Prepare` 校验数组、引用、顶点/索引界限、epoch 和实例范围，在 `Scene::Update` 中暂存变化。
epoch 非零且不能倒退；切换 epoch 清空旧场景但保留设备。`Commit` 使用预备的 map 节点发布。
准备、构图、图编译或录制失败时不提交场景增量。

可见 primitive 的命令按需生成。缓存 key 包括 mesh/material/index 版本、绑定版本、processor
身份 token 和 Version。带资源引用的材质使用调用方传入的统一 bindingVersion；无资源引用时该版本为零。
这不是按每个纹理对象分别维护的细粒度版本系统。纹理内容变化本身不等于绑定身份变化。
可见 submesh、depth key 和实例内容改变只影响本帧数据。

`MeshPassProcessor::Validate` 校验 mesh/material 兼容性；`Build` 默认按 mesh 的各个范围生成命令。
processor 提供稳定描述，不录制 GPU 命令。shader/布局/状态及绑定设施由应用 owner 管理。

`BuildDrawList` 按 depth key、program/material、页、mesh、实例源与范围稳定排序。
几何、版本、布局、绑定和状态一致且范围连续时合并范围；几何和范围一致且实例流连续时合并实例。
direct 与 indirect 消费同一组 DrawPacket，indirect 参数由 CPU 生成并上传；能力不足或 forceDirect 时使用 direct。

`GraphicsPipelineCache` 复制入口名、顶点语义和描述值，比较 shader 对象、pipeline layout、
顶点布局、固定状态和颜色/深度/采样 attachment signature。尺寸不是 key；shader/layout owner 必须比缓存活得久。

## 一帧调用顺序与所有权

`Renderer::Create` 接收 `DeviceContext&` 和一个 `unique_ptr<IRenderPipeline>`。
Scene 由应用持有；Renderer 拥有 pipeline、GeometryStore、ResourceStore、完成 fence 和两个 FrameContext。
设备由 Renderer 借用，Environment 必须晚于设备和所有依赖它的资源销毁。

`Renderer::Render` 的基线顺序：

1. 校验提交的 timeline waits/signals；`Scene::Prepare` 暂存更新并取得可见命令。
2. 选择 `_submitted % 2` 帧槽，等待该槽此前的 completion；默认超时 5 秒，超时保留 owner。
3. 重置 allocator，释放该槽已完成的图和保活引用；`GeometryStore::Prepare` 准备分配与 staging。
4. 解析顶点页和索引位置，调用 `BuildDrawList`，上传实例以及需要的 CPU indirect 数据。
5. 加入 `SceneUpload`，调用应用 `IRenderPipeline::BuildGraph`；需要采样时加入 `TimestampResolve`。
6. `GraphBuilder::Compile` 产生计划；`GraphExecution::Create` 取得临时资源。
7. 在提交前准备好所有可能分配的账本，`GraphExecution::Record` 录制命令。
8. 向 Direct queue 提交 command buffer、waits 和 signals；更新 completion，保留图执行对象，再 Commit 几何和 Scene。

`Render` 成功表示已提交，不表示 GPU 已完成。frameData 仅在本次 BuildGraph 调用期间借用，
延迟 pass 回调需要保存自己使用的数据。图持有回调捕获，帧槽和资源 owner 保活 GPU 对象。
`Finish` 等待完成再释放引用；超时返回失败并保留资源。Renderer 析构时若仍有未完成提交会等待队列。
录制失败会结束并 reset 未提交命令；RHI 不可恢复的终止不转成可恢复失败。

顶点驻留在约 4 MiB 的 device-local 页中，超大 mesh 可以扩大单页；索引按 ID 分配。
staging 复制作为图内 pass 记录，屏障由图生成。几何分配属于对应 Renderer，Scene 不能在不同 Renderer
之间直接迁移并继续使用旧驻留数据。场景替换移除当前引用，在途帧仍持有旧分配；完成后归还预备的空闲节点，
几何分配析构不分配内存。无引用空页在后续准备时释放。

FrameContext 持有 allocator、command buffer、命名上传 buffer、staging、计时资源、图执行对象与 owner 引用。
生命周期和 RHI CPU 调用由调用方串行化，没有内置 Render Thread、并行录制或多队列调度。

## Render Graph 与执行器

GraphBuilder 支持创建/导入 texture、buffer，添加 raster/compute/copy/mesh pass，标记 output、Extract 和 Compile。
句柄包含共享图身份 token 与资源索引，不使用全局计数器。解析检查所属图、类型和 pass 声明。
重复导入同一底层对象会去重并保留各次 owner；初始/最终状态或 acquire/release 冲突会使编译失败。
空导入、冲突导入和无效输出立即记录错误并使 builder 保持失败状态。

访问按整资源追踪：读依赖此前最近写入，写建立 WAW/WAR，读建立 RAW。
写声明表示整资源初始化；部分写且保留旧内容要声明读写或 attachment Load。
编译检查初始化、跨图句柄、usage/stage/pass 兼容性、冲突访问、attachment 尺寸/格式/采样数和 load/store。
Discard 使内容失效，之后不能读取或作为有效输出。attachment 只支持完整单层 2D，没有子资源 attachment、
resolve 或 stencil 操作配置。

output、extraction、导入资源写入和显式 sideEffect 是保留根，反向保留依赖。
存活 pass 按声明顺序执行，不做覆盖写消除、激进重排或 pass 合并；无关 pass 和临时资源不执行、不分配。
CompiledGraph 保存 pass、依赖、使用区间、前置屏障、epilogue 和最终状态。
Dump、Renderer.LastGraph 和 `RADRAY_DUMP_GRAPH` 环境变量用于诊断。

资源访问映射到 RHI buffer/texture 状态及 shader stage。连续 UAV 访问存在写依赖时，即使状态相同也插入 UAV 屏障。
导入的 acquire/release 在首用和图末处理，无存活 pass 时仍保留必要边界交接。
timeline waits/signals 在 Renderer 提交层处理。

GraphExecution 开闭 pass、设置屏障并调用回调。PassContext 只能解析已声明资源、创建兼容 TextureView，
以及获取对应 raster/compute encoder 或 copy 接口。错误解析会令 Record 失败，即使回调返回 true 也不能掩盖。
回调不能自行改变资源状态或 pass 边界；导入裸指针的外部 owner 必须活到 GPU 完成，显式传入 owner 则由图保活。

ResourceStore 按完整描述符匹配资源，只有池外 owner 全部释放且 completion 已完成才复用。
不做同图内存别名。ExtractedResource 保存资源/导入 owner、最终状态和 completion fence/value；
CPU 使用前等待完成，跨帧导入使用返回状态，并由调用方满足执行同步。设备和环境必须活到所有 extracted owner 释放。

## 环境、设备与能力边界

Environment 创建 DXGI factory 或 Vulkan instance，记录 backend、validation 和创建线程。
DeviceContext::Create 借用环境创建 Device 与一条 Direct 队列，启用外部资源扩展，按可选宿主身份尝试匹配适配器；
找不到匹配项时记录警告并使用优选设备。基线还公开接受 shared Device 和非空队列指针的构造函数。
DeviceContext 析构等待 Direct 队列；SupportsIndirect 读取 RHI 的 IndirectDraw 与 IndirectFirstInstance 能力。

基线没有 GPUScene、GPU 剔除、GPU 生成 indirect 参数、完整材质系统、自动材质/pass 分类、
内置相机/光照/阴影流程、并行录制、异步计算、多队列调度、图内内存别名或运行时 shader 编译。
profiling 支持整图 timestamp 读回，每 120 个样本打印 p50/p95/p99、几何页字节和命令构建累计数；
它不构成固定场景性能基准。

## 删除前测试入口与既有验证记录

CPU 与 GPU 测试链接到同一个 `test_radray_runtime`，通过 gtest_discover_tests 注册：

- RuntimeScene：缓存与更新事务、严格合批、procedural mesh 缓存。
- RuntimeGraph：依赖与裁剪、访问校验、构建错误即时日志、attachment 与 extraction。
- RuntimeGpu：导入及 owner、资源池 completion、compute 与失败事务、raster/resize、可替换中间 pass，按 D3D12/Vulkan 参数化。

旧操作入口如下；恢复对应基线源码及依赖后才适用：

```powershell
python tools/compile_runtime_test_shaders.py
python tools/compile_runtime_test_shaders.py --check
cmake --build --preset win-x64-debug --target test_radray_runtime --parallel 8
$env:RADRAY_TEST_REQUIRED_BACKENDS = "d3d12,vulkan"
ctest --preset win-x64-debug -R 'Runtime(Scene|Graph|Gpu)' --output-on-failure
```

脚本显式使用本地 DXC，默认产物为 `build/runtime-test-shaders/`，可通过 `--dxc`、`--output-dir`
和 CMake 的 `RADRAY_RUNTIME_TEST_SHADER_DIR` 调整。CMake 仅执行 `--check`，不自动下载或编译 shader。
校验包含生成脚本、HLSL、core shaderlib 的内容指纹以及 DXIL/SPIR-V 文件哈希。

下面保存删除前 build-test 文档中的既有验证声明；本次归档没有复跑这些旧测试，
这些计数也不作为本基线或精简后工作树的测试数量保证：

> 2026-10-30 在 Windows x64、MSVC 19.51、RTX 3060 / NVIDIA 591.44、Vulkan SDK 1.4.357.0 验证迁入的 runtime：
> standalone 动态 CRT 的 Debug/Release 各 16 项通过，`RADRAY_TEST_REQUIRED_BACKENDS=d3d12,vulkan`，没有跳过。
> 同一源码在 Raynium 的 `/MT` Debug/Release 消费中各通过 57 个 native/GPU 回归；没有进行 GUI 或游戏交互验证。
> 另以 Release `/MT`、仅 core/render/runtime 的安装配置生成公共头与静态库，外部项目通过
> `find_package(RadRay CONFIG REQUIRED COMPONENTS runtime)` 链接 `RadRay::runtime`，运行图编译与计划检查成功。
> 这些检查不覆盖本机以外的 GPU/驱动或其他平台。
