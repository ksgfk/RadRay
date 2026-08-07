> - 适用: ADR-0016 第一阶段实施；正式 cook、publisher、install-tree 验收、完整 AOT coverage 不在本阶段
> - 权威: 本文是第一阶段实施清单；设计约束以 ADR-0016、`CONTEXT.md` 和 shader pipeline 架构文档为准
> - 状态: 已完成（2026-08-08；M-1 至 M8 检查站全部通过）。后续生产 cook/publisher 与 install-tree 验收仍按文末 TODO 保留
> - 锚点: `docs/adr/0016-hlsl-and-radray-dxc-are-shader-authority.md`, `docs/research/dxc-embedded-metadata-vs-cpp-trace.md`, `CMakeLists.txt`, `CMakePresets.json`, `.gitattributes`, `AGENTS.md`, `modules/render`, `modules/runtime`, `shaderlib`, `tools`

> 后续修正：本文记录的是 2026-08-08 的第一阶段历史检查站。其旧版 ABI probe、D3D12 smoke、错误
> ABI fixture 与源码扫描路径已由 ADR-0034 和 `docs/todo/radray-dxc-frontend-semantic-migration.md`
> 取代；当前测试入口以 Clang/LLVM harness 与 `RadRayShaderCompilerClient` consumer suite 为准。

# HLSL + forked RadRay DXC shader pipeline：第一阶段

## 完成定义

第一阶段完成必须同时满足：

1. 老 shader 系统已整体删除：`radrayshader`、手写 shader JSON、`shader_gen`/`shader_cook`、
   反射校验、SPIRV-Cross、Metal 残留、imgui 全套、旧 manifest/resolver 路径和 C++ trace 路线
   在源码、CMake 与 active docs 中都不存在。这不是重构，是清空后从头重写。
2. pinned RadRay DXC SDK 能通过 `dxcapi_radrayext.h` 完成 source contract discovery 和 concrete Variant 的 DXIL/SPIR-V 编译，并输出独立、版本化、target-specific metadata。
3. `radrayrender` 能直接解码 compiler metadata，构造 D3D12 与 Vulkan 各自的 binding/layout/vertex state；运行时不调用反射校验、不重建 metadata、不依赖 SPIRV-Cross。
4. `radrayruntime` 通过 compiler-produced artifact 完成 D3D12/Vulkan JIT 垂直切片；Variant 选择由 caller/asset orchestration 提供，编译器内部完成 stage merge。
5. `RADRAY_BUILD_SHADER_COMPILER=OFF` 的 compiler-free 构建通过 **build-tree** 检查，且只用测试 fixture
   验证 compiler-free artifact consumption boundary；这不宣称正式 cook、artifact loader、install-tree
   验收或生产 AOT 发布已完成。

## 执行规则

- 里程碑必须按顺序推进；前一里程碑的检查站未全部通过，不得开始后一里程碑的改动。
- 检查站必须留下可重复的测试目标、命令或文件清单；人工"看起来正确"不算通过。
- 检查站失败时修复当前里程碑，不通过添加运行时 fallback 或临时 JSON 绕过。
- 每个里程碑完成后，同步更新 `CONTEXT.md`、`AGENTS.md`、受影响的 architecture/guide 文档和本文件状态。
- 所有构建/测试遵守 `docs/guide/build-test.md` 的预设；不得并行运行 build 与 test。

### 检查站记录格式

每个检查站使用稳定的 `M<里程碑>-C<编号>` ID（清空里程碑为 `M-1-C<编号>`），并且必须同时写明
验证命令/静态检查、通过条件、fixture 或测试输入、证据输出位置。推荐的 CTest suite 名称为：
`RadRayRenderPsoSmoke`、`RadRayShaderContract`、`RadRayDxcAbiProbe`、`RadRayDxcMetadata`、
`RadRayDxcAtomicBatch`、`RadRayRenderShaderArtifact`、`RadRayRuntimeShaderJit`、`RadRayShaderCMake`。
只有对应 ID 的命令退出码、断言和证据都满足，才能把该检查站从 `[ ]` 改为 `[x]`。

### 当前实施记录

本轮工作保留了多个可回溯中文提交点：`b42b874`（M-1 清空）、`ded392a`（M0 wire）、
`9bb65c2`（M1 ABI probe）、`edba732`（M2 discovery）、`b8e69b0`（M3 typed batch）、
`1eda14b`/`807c0e9`（M5 decoder 与 handle）、`a825fe1`/`a6f39ec`（M6 graphics/compute JIT）、
`186425d`/`736e57a`（M7 pass）、`0c442b0`（M5 负向 decoder）、`f279d5d`/`88bcf43`（M3
assignment 与稳定性）、`c3ac0cb`（M4 stock adapter 隔离）、`ff8de12`/`faefab9`（文档与隔离指南）、
`29ba901`（compiler-free 分支修复）、`09c5537`（DXIL register namespace 与 target-native handle）、
`502f817`（active binding/type-tree fixture）、`94c21e3`（compile input hash/root-push matrix）、
`a9d604f`（逐 lane native layout input）、`f9eabc2`/`f47f385`（runtime handle 与七类 JIT case report）、
`38c3952`/`e6b9db3`（vertex input fail-closed）、`293ed5e`/`b69c136`（static sampler 与
RootSignature policy）、`51b2618`（shaderlib target-native binding 断言）、`7a43b15`（BindingHandle
跨 layout Debug guard）、`dad04ba`（M4 `RadRayShaderCompilerClient` loader/link-boundary probe）、
`bf37509`（M4 raw shader compile tool 与 tools-only preset）、`e1d8ebf`（raw tool 边界文档）、
`36eb401`（wire type kind ABI 命名）、`0dfbbd0`（type-tree wire safety 负向矩阵）、`cd37337`
（旧 shader target 检查精确匹配）。

本轮新增可回溯提交：`da8ad9a`（RadRay DXC fork package/client 接入）、`1069458`（fork ABI
正向 probe）、`f9f4ba2`（fork 真实 metadata golden）、`ad8cdfa`（移除 core 内过渡扩展头）、
`d8a603c`（RootSignature/static sampler 真实 fork 证据）、`d36aa45`（正式 preset 与 package
runtime deployment）、`60768d9`（冻结 vertex interface wire v2）、`c61e7fe`（render 解码
vertex interface 与 fork golden）、`886c82545`（fork metadata schema v2 vertex interface），
`b89a112d3`（fork metadata schema v3 type-tree underlying index）和 `0ff762d41`（fork 自有
D3D12 DXIL GPU gate）。对应的独立 DXC fork 位于 branch `codex/radray-dxc-1.9.2607`，当前
package identity 为 `1.9.2607.radray.1`；`fetch_sdks.py` 按 `project_manifest.json` 准备
`radray_dxc` 本地包，RadRay 正式配置按 Manifest `Name` 使用
`SDKs/radray_dxc/extracted` 并发现 `RadRayDXC::Headers/Compiler`，
不设 `RADRAY_DXC_FORK_PACKAGE_ROOT`/`RADRAY_DXC_SDK_ROOT` override。fork 打包脚本位于 fork 仓库
`utils/package_radray_sdk.py`，把完整 dxc 发行集（`dxc`、`dxcompiler`、`dxc-headers` 组件与
`dxil.dll`/`dxil.lib`）打包为 relocatable zip 并回写 manifest hash；`fetch_sdks.py` 对本地包
直接复制并校验 hash。stock DXC 只保留在 manifest 的 `dxc` 条目供隔离验证，不是正式 fork
能力证据。

M1-C05 已由 fork 仓库自有独立 executable 完成；vertex interface 与 CPU type-tree wire 已由
fork schema v3、两个 target golden 和 render fail-closed decoder 覆盖。
2026-08-08 曾在主工程 configure 中验证 manifest/state/archive 完整性；该职责现已移交
`fetch_sdks.py`，主工程 configure 只使用 Name-derived package prefix。fork ABI/schema/toolchain
identity 仍由 package config、client 和 ABI probe 校验。
M6-C04 的当前实现已在 D3D12/Vulkan 资源写入与 push-constant 路径加入 Debug 跨 layout
断言，并由 `RadRayRenderPsoSmoke` 的 debug death test 与 unknown/inactive lookup 覆盖。
2026-08-08 回归：`ctest --test-dir build_runtime_only -C Debug -R RadRayRenderShaderArtifact`
通过；`ctest --test-dir build_shader_compiler -C Debug --output-on-failure` 为 182/182；tools-only CLI
重新输出 `depth.dxil.bin` 3114 bytes 与 `depth.spirv.bin` 718 bytes。`llvm-readobj` 当前不在
环境中，tools import 的本轮证据使用 map、Visual Studio target graph 与 runtime-only 文件/cache
扫描；既有 compiler-client COFF import 检查仍通过。

## M-1：清空老 shader 系统

**目标**：把老 shader 系统整体删除，让后续里程碑在干净地基上从头重写。删除期间不保留兼容层、
不保留半死的占位类型。删完后仓库处于"能编译、GPU 冒烟通过、没有任何 shader 管线"的状态。

**为什么排在 M0 之前**：`radrayrender` 对 `radrayshader` 是 PUBLIC 链接且 `rhi.h` 直接 include
`shader_types.h`，这条边必须先断开；老 `ShaderPassDesc`/`ShaderArtifactEntry`/`BindingGroups`
留在树里会把 M0 的新 wire 形状带偏（旧 `ComputeShaderArtifactKey` 把 `PassName` 入 hash，
正是新契约明令禁止的）。

**删除项**：

- `modules/shader/` 整个模块（含 `dxc`、`hlsl`、`spirv`、`spvc`、`msl`、`shader_manifest`、
  `shader_asset_template`、`shader_reflection_map`、`shader_types`、vendored `d3d12shader.h` /
  `d3dcommon_adapter.h`，以及 `tests/` 下 296 个用例）。
- `radrayruntime` 的 shader 耦合：`shader_asset.*`、`shader_program.*`、`pipeline_layout_cache.*`、
  `gpu_resource.{h,cpp}` 中的 `PipelineStateCache` 与 `GraphicsPipelineStateKey`。
  `RenderSystem` 去掉 `_pipelineStateCache`/`_pipelineLayoutCache`/`_shaderResolveContext`/`_dxc`
  四个成员，保留瘦壳（window/swapchain/frame target 遍历与 `_renderPassRegistry`）。
- `radrayruntime` 的 shader 测试：`test_shader_program`、`test_vertical_slice`、
  `test_pipeline_layout_cache`、`test_pipeline_state_cache`、`test_shader_layout_binding`
  （共 53 个用例）。`test_asset_slot` 保留：它用假资产、不需要 device 也不需要 shaderlib，
  守的是 ADR-0007/0009 的引用计数与延迟销毁语义。
- `tools/shader_gen/`、`tools/shader_cook/`、`tools/generate_imgui_shader.py`；
  `tools/CMakeLists.txt` 与根 `CMakeLists.txt` 的 `add_subdirectory(tools)` 一并删除，
  M4 需要 shader tools 时重建。
- imgui 全套：`RADRAY_ENABLE_IMGUI`、`RADRAY_DISABLE_IMGUI_DEMO`、`imgui_config.h`、
  `radray_imgui_shader.cpp`、`shaderlib/imgui/`、`runtime/CMakeLists.txt` 的 imgui 源与 include。
  理由：RadRay 侧没有任何 imgui 集成代码，`GetImGui*` 五个函数无调用方，
  `imgui_pass.hlsl` 的手写 `[[vk::push_constant]]` 还违反 `VK_*` 宏规则。
  将来重新引入时必须按新契约重写。
- `examples/` 整个目录：`sphere_demo/`、`gltf_viewer/` 与 `examples/CMakeLists.txt`，
  以及根 `CMakeLists.txt` 的 `add_subdirectory(examples)` 与 `RADRAY_BUILD_EXAMPLES` 选项、
  `cmake/Utility.cmake` 的 `radray_add_example` / `radray_example_files`。
  **两个 demo 早已编译不过**：`sphere_demo.cpp` include 了 8 个当前不存在的头
  （`material_asset.h`、`components/static_mesh_component.h`、`components/camera_control_component.h`、
  `render_framework/{static_mesh_scene_proxy,forward_pipeline_shader,standard_material_factory}.h` 等），
  靠 `examples/CMakeLists.txt` 把两个 `add_subdirectory` 全注释掉才没暴露。它们引用的渲染框架
  API 或从未存在、或已被删除，留着是纯误导。**注意**：`RADRAY_ENABLE_IMGUI` 消失后那层
  `if (RADRAY_ENABLE_IMGUI)` 包裹也没了，若只删包裹不删目录，任何人解开注释都会立刻构建失败。
  **`assets/` 目录必须保留，`RADRAY_ASSETS_DIR` 注入必须保留**：虽然
  `radray_example_files` 把 example 资产拷进 `assets/<target>/`，但该目录同时被
  `modules/core/tests/CMakeLists.txt` 的 `test_img_rw`（读 `assets/*.png`）与
  `benchmarks/bench_read_obj`（硬编码读 `assets/buddha1.obj`）消费，`cmake/Utility.cmake`
  的 `radray_add_radray_gtest_case` 也注入 `RADRAY_ASSETS_DIR`。
  只删 `radray_add_example`/`radray_example_files` 两个函数，不动 `assets/` 与
  `RADRAY_ASSETS_DIR`；连带删会打断 `test_img_rw`，M-1-C01 立即失败。
  （`assets/` 下 4 个子目录是 example 资产产物、`rg` 证明无代码引用，且整个 `assets/`
  在 `.gitignore` 里不受版本控制，所以无需清理。）
- freetype：`RADRAY_ENABLE_FREETYPE` 与 `third_party/freetype` 接入。删 imgui 后它零消费者。
  `RADRAY_ENABLE_ZLIB`/`LIBPNG`/`LIBJPEG` 保留 —— `radraycore` 的图片读写在用。
- Metal 残留：`RADRAY_ENABLE_METAL`、`ShaderBlobCategory::MSL`、`::METALLIB`。新契约的
  target lane 只有 DXIL 与 SPIR-V，留着永不合法的枚举值会让每个 `switch` 带不可达分支。
  删除窗口正是现在 —— M-1 之后无任何序列化数据引用它们，M0 冻结 wire 后再删就是 schema 变更。
- SPIRV-Cross：`RADRAY_ENABLE_SPIRV_CROSS`、`third_party/SPIRV-Cross` 的 `add_subdirectory`
  与四个 target 的 flag 循环、`project_manifest.json` 条目。

**保留与搬迁项**：

- `shaderlib/` 只保留着色数学层：`core/{math,color,frame}.hlsli`、`bsdf/*`、`lighting/*`、
  `shadow/*`。它们不含任何 `register(...)`、`vk::binding` 或 pragma，且承载对齐成本
  （`bsdf/principled.hlsli` 逐项对齐 Mitsuba3，`shadow/filtering.hlsli` 移植自 Unity URP 并
  保持数值行为）。include 已是 root-relative 尖括号形式，无需改动。
- 删除绑定/Pass 层：`core/platform.hlsli`、`forward_pipeline/` 全部（`bindings.hlsli`、
  `view.hlsli`、`standard_material.hlsli`、三个 `.hlsl`、两个 `.shader.json`）。
- `shader_types.h` 中 RHI 需要的枚举/结构**搬回 `rhi.h`**：`ShaderStage`（含
  `is_flags`/`is_compound_enum_flags`/`EnumFlags` 特化）、`ShaderBlobCategory`（去掉
  `MSL`/`METALLIB`）、`AddressMode`、`FilterMode`、`CompareFunction`、`VertexStepMode`、
  `VertexFormat`、`ShaderParameterBindingType`、`SamplerDescriptor`、`ShaderBindingLocation`。
  命名空间保持 `radray::render`，所以 `rhi.h` 内 15 处引用零改动，只删 include。
  `JsonSerializer`/`JsonDeserializer<render::ShaderBindingLocation>` 与 `<render::SamplerDescriptor>`
  四个特化随 JSON 路线删除，`rhi.h` 因此不再需要 `radray/json.h`。
  `ShaderBindingLocation` 只搬不改，等 M5 重做 binding lookup 时再决定它是进 DXIL payload 还是消失。
- ADR-0006 那套"是不是 manifest 数据"的收录判据随 manifest 作废；`rhi.h` 里解释该判据的
  注释与 `shader_types.h` 头部注释一并删除。
- **删文件时连带删它的 doc banner**，不要留悬空引用。`check_docs.py` 会校验 banner 指向的 doc
  存在（当前 17 个 banner）。指向即将删除的 `docs/architecture/shader-pipeline.md` 的 banner
  共 5 处（`shader_manifest.{h,cpp}`、`shader_asset_template.h`、`shader_program.h`、
  `shader_asset.h`），它们所在文件恰好都在删除清单里，故不会产生悬空引用 —— 但删除时必须
  确认这一点，而不是假设。
- **删 `radrayruntime` 的传递依赖 include**：`gpu_resource.h`、`render_system.h`、
  `shader_program.h` 三个头 include `radray/shader/shader_manifest.h`。其中 `gpu_resource.h`
  被 `static_mesh.h`、`gpu_system.h`、`primitive_scene_proxy.h` 三个**保留**文件间接引入；
  `rg` 证明这三者不使用任何 shader 类型（`ShaderVariantKey`/`ShaderHash`/`ShaderStages`/
  `ShaderBlobCategory` 零命中），属纯传递依赖，直接删 include 即可。

**CMake 选项改动**：

- 删 `RADRAY_BUILD_SHADER`；`RADRAY_BUILD_RENDER` 解除对它的依赖。
- 删 `RADRAY_ENABLE_SHADER_JIT`（M-1 后零消费者，M4 随 client 重新引入）与
  `RADRAY_ENABLE_SPIRV_CROSS`，以及 `RADRAY_ENABLE_SHADER_JIT`/`SPIRV_CROSS` 两条 `FATAL_ERROR` 校验。
- `RADRAY_ENABLE_DXC` **改名为 `RADRAY_BUILD_SHADER_COMPILER`**。M-1 到 M3 期间它的语义是
  "有 stock DXC headers，GPU 冒烟测试可用"；M4 升级为"发现并导入 RadRay DXC SDK + 构建 client"。
  提前改名让 M-1-C04 能一次清干净所有旧开关名。
- `RADRAY_BUILD_RUNTIME` 对 `RADRAY_BUILD_RENDER AND RADRAY_BUILD_WINDOW` 的依赖不变。
- M-1 不动 preset 结构；两个隔离 preset 在 M4 建立。

**新增项**：

- **`RadRayRenderPsoSmoke`**：`modules/render/tests/` 下的最小 GPU 冒烟测试，**永久保留**。
  它建立 device/queue、编译 inline HLSL、创建 PSO、draw、readback 并断言像素；D3D12 与
  Vulkan 各一遍。字节码来源是测试内直接调用 stock `IDxcCompiler3`，只用 SDK 的 headers，
  不经过任何 RadRay wrapper。
  这条路径被 `CONTEXT.md` 明确许可："任意 DXC flag 实验只能走标准 `IDxcCompiler3`，
  不进入 RadRay artifact pipeline"；它约束的是 client，不是测试。
  HLSL 源写成 `.cpp` 里的 inline string literal，不读 `shaderlib/`、不需要 `RADRAY_PROJECT_DIR`。
  门控在 compiler capability（M-1 起是 `RADRAY_BUILD_SHADER_COMPILER`）；runtime-only preset 下不注册。
  它是 M-1 到 M6 之间唯一的 GPU 活体证明，也是长期独立于 RadRay compiler 管线的 RHI 冒烟。
- **加载 DXC 用 `radraycore` 已有的 `radray/dynamic_library.h`**（`DynamicLibrary` 提供 RAII 与
  `GetFunction<T>`），不手写 `LoadLibrary`/`dlopen`。
  **只加载 `dxcompiler`，不要求 `dxil` 可加载** —— 老 `CreateDxc()` 把 `dxil` 当硬前置，
  而 research 报告已实测证明 pinned SDK 的默认 internal validator 输出可被 D3D12 加载、
  与 external validator 逐字节相同；沿用旧要求会让没部署 `dxil` 的环境无谓 skip。
- **必须新增 compiler 运行库拷贝规则**：把 `dxcompiler` 运行库拷到
  `${RADRAY_BUILD_PATH}/$<CONFIG>`（挂在 `radrayrender` 或冒烟测试 target 的 POST_BUILD）。
  这是 M-1-C02 的**前置条件**：老实现按裸名 `DynamicLibrary{"dxcompiler"}` 加载，靠
  `radrayshader` 的 POST_BUILD 把 DLL 拷到输出目录；删掉那个 POST_BUILD 后裸名加载会失败，
  测试将全程 `GTEST_SKIP` —— 看起来"通过"，实际什么都没验。
  这不违反"第一期不做 shaderlib 部署拷贝"：那条说的是 HLSL 源，不是 compiler 运行库；
  且 `CONTEXT.md` 本来就要求 compiler capability 开启时 CMake 自动把 compiler binary
  放进公共 build output，本条是它的 M-1 版本。
  因此根 `CMakeLists.txt` 的 `RADRAY_DXC_RUNTIME_DLLS` 与 `RADRAY_DXC_INCLUDE_DIR` 两个
  INTERNAL cache 变量**保留**（原唯一消费者是 `modules/shader/CMakeLists.txt` 的 POST_BUILD），
  但 `RADRAY_DXC_RUNTIME_DLLS` 只保留 `dxcompiler`，去掉 `dxil` 条目。
- **`RadRayRenderPsoSmoke` 应从 `test_vertical_slice.cpp` 移植 GPU 脚手架，不从零写**：
  device/queue/Vulkan env 建立与关停顺序、render target 创建、readback map/unmap、像素断言
  约 400 行与 shader 系统无关。`test_render_pass_registry.cpp` 已经有一份近似的设备建立代码
  （含 Vulkan instance 全局环境的关停顺序处理），**已经重复了一次**，不要再重复第三次 ——
  移植时把公共部分提成 `modules/render/tests/` 下的共享 fixture header，两个 suite 共用。
- 第一期不做 shaderlib **源码**部署拷贝（承载它的 POST_BUILD 随 `radrayshader` 删除）。JIT 读仓库源，
  通过 `RADRAY_PROJECT_DIR` 定位。部署副本的意义是发布包，第一期不做发布包。

**删除顺序（必须遵守）**：

25,000 行跨 5 个目录，顺序错了会长时间处于编译不过的状态。按下列顺序做，每一步之后仓库都
尽量接近可编译：

1. 叶子先删：`tools/`（两个 CLI + `generate_imgui_shader.py` + `CMakeLists.txt`）、`examples/`、
   imgui 全套。这三组无人依赖。
2. 断开 render→shader 的边：`shader_types.h` 的 RHI 类型搬进 `rhi.h`，删 include 与
   `radrayrender` 对 `radrayshader` 的 PUBLIC 链接。
3. 删 `radrayruntime` 的 shader 耦合与 5 个测试 target。
4. 删 `modules/shader/` 整模块与 `modules/CMakeLists.txt` 的对应 `add_subdirectory`。
5. 删 CMake 选项、`FATAL_ERROR` 校验、SPIRV-Cross/freetype 的 third_party 接入与
   `project_manifest.json` 条目；改名 `RADRAY_ENABLE_DXC`；新增 compiler 运行库拷贝规则。
6. 删 `shaderlib/` 绑定层，保留数学层。
7. 建 `RadRayRenderPsoSmoke`（含共享 fixture header）。
8. 改文档与 `AGENTS.md`、`.gitattributes`。

**文档改动**：

- 删除 `docs/architecture/shader-pipeline.md` 与 `docs/guide/shader-authoring.md`，M7 用新内容重建同名文件。
  不让它们在 M-1 到 M7 之间描述一个不存在的系统。
- 删除三份已作废的 TODO：`docs/todo/cpp-trace-shader-frontend.md`（已废弃）、
  `docs/todo/backend-specialized-shader-lanes.md`（正文自称整体作废）、
  `docs/todo/vertex-interface-projection.md`（已实施，但实施对象随 M-1 删除，失去指代）。
  历史保留在 git log 中；`docs/todo/` 不是 ADR，不适用"只追加不修订"。
- 同步改写：`AGENTS.md`（下述四条）、`docs/architecture/overview.md`（模块表、依赖图、
  行数表、职责表、shaderlib 部署段、文档索引）、`docs/architecture/shaderlib.md`、
  `docs/architecture/render-framework.md`、`docs/architecture/render-rhi.md`、
  `docs/architecture/asset-system.md`、`docs/guide/build-test.md`（选项表、target→suite 对照表）。
- `AGENTS.md` 四处按事实改：模块数 5 → 4；依赖链去掉 shader 段（M4 加 client 后再补）；
  `.hlsl` 的 entry 由 `[shader("...")]` 决定、不再有 `VSMain`/`PSMain`/`CSMain` 命名约定；
  layering 那条对两个已删 CLI 的约束改为约束未来的 `RADRAY_BUILD_SHADER_TOOLS`。
- `AGENTS.md` 的 shader 宏规则改为：`[[vk::*]]` 属性必须通过 `core/platform.hlsli` 的 `VK_*`
  宏书写，因为 DXIL lane 下裸写非法；binding 编号直接写在 `register(...)` 与 `VK_BINDING(...)`
  里，**不再有编号封装宏**。理由见 M2。
- `.gitattributes` 清掉 `modules/render/src/d3d12shader.h` 与 `d3dcommon_adapter.h` 两行
  （路径本就写错，实际在 `modules/shader/src/`，且随 M-1 删除）。

**检查站（全部通过才完成）**：

- [x] **M-1-C01**：`cmake --preset win-x64-debug` 与 `cmake --build build_debug --parallel 24` 全量成功；`ctest --preset win-x64` 全部通过（无 skip 以外的失败）。这条守住删除没有留下悬空引用；证据为 build/test 日志。
- [x] **M-1-C02**：`RadRayRenderPsoSmoke` 在 D3D12 与 Vulkan 上各完成一次 PSO 创建 + draw + 像素回读；字节码由测试内 stock `IDxcCompiler3` 现场编译。**必须先确认 `${RADRAY_BUILD_PATH}/<Config>/` 存在 `dxcompiler` 运行库**：缺它会让裸名加载失败而全程 `GTEST_SKIP`，把"什么都没验"伪装成通过。因此缺 `dxcompiler` 不允许 skip，且 GPU CI gate 的通过条件是"两个 backend 各有一次真实的像素断言执行"，不是"suite 退出码为 0"。无 GPU 的开发机可以 `GTEST_SKIP`，但该结果只能标记为本地环境未覆盖，不能作为里程碑/CI gate 通过证据；证据为 suite 输出与输出目录文件清单。
- [x] **M-1-C03**：`python tools/check_docs.py` 通过。M-1 当时记录的 baseline 是零错误；当前复核为 234 files / 34 docs / 104 anchors 全通过，所以删模块必须同步改锚点与文档引用；证据为命令输出。
- [x] **M-1-C04**：`rg -n "shader\.json|shader_gen|shader_cook|SPIRV-Cross|spirv_cross|reflection validation|C\+\+ trace|RADRAY_BUILD_SHADER(\b|[^_])|RADRAY_ENABLE_DXC|RADRAY_ENABLE_SPIRV_CROSS|RADRAY_ENABLE_METAL|RADRAY_ENABLE_IMGUI|RADRAY_ENABLE_FREETYPE|RADRAY_BUILD_EXAMPLES|\bradrayshader\b|METALLIB|imgui" CMakeLists.txt cmake modules shaderlib tools docs/architecture docs/guide` 零命中（历史 ADR/research 允许命中）。这里的 `\bradrayshader\b` 只匹配已删除的旧 target，不误伤当前 `radrayshadercompiler` client。`tools/` 仍保留 `check_docs.py`、依赖恢复和编译辅助脚本，必须纳入扫描；只删除其中明确列出的旧 shader 子目录/脚本。`examples/` 若已删除则不作为独立 `rg` 路径传入；根 CMake 对它的引用由 M-1-C05 检查。证据为命令日志。
- [x] **M-1-C05**：历史 M-1 检查证明 `radrayrender` 与 `radrayruntime` 的 CMake 目标不再引用旧的精确 target `radrayshader`，且 `rhi.h` 不再 include `radray/shader/*` 或 `radray/json.h`；当前 `radrayshadercompiler` 只在 compiler/JIT 能力打开时作为可选 client 出现，runtime-only 仍不发现它。`modules/CMakeLists.txt` 无 shader 子目录，`examples/` 仍无回流。根 CMake 的 tools 子目录后来由 M4 以 `RADRAY_BUILD_SHADER_TOOLS` 条件重新接入，当前反向依赖边界由 tools-only preset 与 map 检查守住；证据为 target graph、include 和 map 检查。
- [x] **M-1-C06**：`modules/render/tests/` 下存在被 `RadRayRenderPsoSmoke` 与 `RenderPassRegistryTest` 共用的 GPU fixture header；`rg` 证明设备/队列/Vulkan 环境建立与关停代码只有一份实现；证据为命令日志与文件 diff。

## M0：契约冻结与 golden fixture

**前置**：M-1 全部通过。

**目标**：把第一期的 wire、身份、错误和测试向量冻结，避免实现阶段重新发明协议。

**实现项**：

- 建立 RadRay extension ABI、source contract、CompileVariant request/result、metadata envelope 和两个 target payload 的版本化头文件草案。
- 建立最小 HLSL fixture 集：无资源 graphics、仅 vertex、texture+sampler、shadow/static sampler、多个 DXIL RootConstants、单 SPIR-V push block、不同 target binding 集合、完整 nested cbuffer/struct type tree、**compute**。
  compute fixture 是**新增**而非迁移 —— 仓库此前零 compute shader。它落在测试 fixture，不进
  `shaderlib/`：`shaderlib/` 是产品 HLSL 库，第一期没有产品功能需要 compute。
- raw golden bytecode/metadata fixture 以**文件**形式版本控制在 `modules/render/tests/data/shader_artifacts/`，
  测试通过 `RADRAY_PROJECT_DIR` 定位（与现有测试约定一致，runtime-only preset 下同样有效，
  不引入 SDK 依赖）。测试只用 C++ fixture table 映射 case 与 raw blob，不引入生产 index 或 JSON。
- **`.gitattributes` 必须显式标注 fixture 为二进制**（`modules/render/tests/data/** -text` 或按扩展名）。
  仓库根是 `* text=auto`，Git 靠 NUL 字节猜二进制；metadata blob 若恰好全是可打印字节会被 LF
  规范化而静默损坏，而 M8-C02 恰好要断言"损坏 metadata 时 fail closed"，真损坏会被误判为通过。
- M0 阶段 fork 尚不存在，fixture 的 bytecode 用 stock DXC 真实编译，metadata blob 手写构造；
  M3 用 fork 真产物替换手写部分（见 M3-C09）。
- 为每个 fixture 固定 keyword domain、assignment、Defines、CompilePolicy、期望 entry topology、active binding 名称和 target-specific layout facts。

**hash 规格（M0 冻结，之后改动即 schema 变更）**：

- 持久化 hash 是 **128 位、little-endian 固定字节序**的 POD 值类型，提供 `operator==`/`<=>` 与
  `ToHex`（仅供诊断）。**不提供 `FromHex`** —— 第一期无内容寻址，blob 文件名反解的用途不存在，
  少一个需要测试的解析路径。
- **hash 完全由 compiler 计算，RadRay 只比较、不重算**。metadata blob 携带 compiler 算好的
  `GpuArtifactHash`，decoder API 另外接收 caller/fixture 提供的可信 `expected GpuArtifactHash`，
  只做二者 `==`；`ContractHash` 由 discovery result 带回。RadRay 侧不实现 hash 算法，也不宣称
  能检测 metadata 内容被篡改但 hash 字段未变的完整性问题；wire 越界和 identity mismatch 仍须
  fail closed。
  fork 内部算法（建议 XXH3_128bits）属 M1 实现细节，M0 只冻结"16 字节 POD + 小端"。
- `ContractHash` 覆盖 canonical keyword domain、entry topology 及影响 discovery 的 Defines/policy。
- `CompileInputHash` 覆盖 root source bytes、**本次实际打开的每个 include 的内容字节（按 compiler
  打开顺序累加）**、canonical assignment/Defines、target、resolved policy、fork/ABI/schema/toolchain identity。
- **`SourceName` 使用稳定的 shaderlib-root-relative 逻辑路径并进入 `CompileInputHash`**，同时
  用作 include 解析、诊断与 `__FILE__` 的 identity。include hash 包含 compiler 实际打开的
  规范化逻辑路径与内容；仓库绝对路径、部署目录和其他物理路径不进 hash。这样 JIT 与未来 cook
  只要提交相同逻辑 `SourceName`，就不会因物理位置不同产生不同身份；若 `__FILE__` 或 debug
  info 使产出字节变化，相关 artifact hash 也会正常变化。
- **include 集合天然 per-lane/per-Variant**：老实现自己扫文本找 `#include` 且刻意不求解
  `#if`，把被条件排除的 include 也计入依赖，因此所有 variant 共享一个 source identity。
  新实现记的是"compiler 实际打开的"，所以 `#ifdef` 包住的 include 会让不同 assignment 得到
  不同 `CompileInputHash`。这与老实现直觉相反，M0 必须有 fixture 显式覆盖。
- `GpuArtifactHash` 只覆盖 bytecode 与 GPU layout metadata。compiler 输出的 type-tree record
  不作为任何 hash 的独立输入，但 source/include bytes 仍按 `CompileInputHash` 的正常规则参与。
- `BytecodeHash` 与 `PipelineLayoutHash` 使用同一 128 位 POD 表示，并由 compiler 产生：前者覆盖
  target lane 的完整 bytecode 字节；后者覆盖 canonical、target-specific GPU layout records（不含
  CPU type tree）。`GpuArtifactHash` 的输入明确包含完整 bytecode 与同一份 GPU layout metadata，
  因此 `PipelineLayoutHash` 只是 metadata 中可比较的组成身份，不是 runtime 重算或缓存 key。
- 第一期不定义 `ArtifactContentHash` 或 `CpuSchemaHash`。`PassName`/`AssetId` 不进 hash。

**`CompilePolicy` 字段清单（M0 逐字段冻结，不留"以后再加"）**：

| 字段 | 说明 |
|---|---|
| `ShaderModel` | 老实现是 per-pass 作者字段（manifest `ShaderModel`），新契约归 caller/build profile |
| `Optimize` | `-O3` 与否 |
| `DebugInfo` | **与 `Optimize` 解耦**。老实现耦合（非 optimize 时 `-Od` 且 DXIL lane 追加 `-Zi`），`-Od` 与 `-Zi` 是两件事 |
| `AllResourcesBound` | 老 `EnableUnbounded` 的取反，对应 `-all_resources_bound`。语义反转，不沿用旧名 |
| `WarningPolicy` | 新增 |
| `SpirvTargetEnv` | 新增，对应 `-fspv-target-env=` |
| `HlslVersion` | **字段而非常量**（老实现硬编码 `-HV 2021`）。它直接改变语言语义，比 optimization 更该显式，且入 `CompileInputHash` 可防止升版本后静默复用旧产物 |

`-fspv-reflect` **不进 `CompilePolicy`**：老实现无条件对 SPIR-V lane 加它，是为了让外部反射器
（SPIRV-Cross）读到 HLSL semantic decoration。新契约下 vertex reflection 由 fork 内部产出，
它是否需要该 flag 属 fork 实现细节，M3 自行决定。

**检查站（全部通过才完成）**：

- [x] **M0-C01**：`git diff --check` 通过，所有 wire record 都是固定宽度整数/offset/length 或受控 blob 引用，不含 pointer、`Vk*`、`D3D12_*` 内存结构；hash 字段为 16 字节小端 POD；证据为 CI 命令日志。
- [x] **M0-C02**：`RadRayShaderContract` 的 golden assertion table（仅测试断言输入，不是生产 authoring 文件或 metadata）逐条列出 fixture 的 expected target facts，且 DXIL/SPIR-V 明确允许不同 binding 数字与集合；compute fixture 的 entry topology 单独断言；证据为测试输出和 fixture 表。
- [x] **M0-C03**：fixture table 固定 canonical request 的字段顺序、整数宽度、assignment/Defines 排序、policy 默认值和 mutation matrix；同一输入的 request bytes 逐字节相同，并区分逻辑 `SourceName` 变化与物理文件搬迁。实际 compiler-owned hash 生成与 mutation 断言后移到 M2/M3，避免在 compiler/discovery 尚未实现时要求 M0 产生 hash；证据为 request dump 与 fixture table。
- [x] **M0-C04**：wire/schema coverage table 明确 type-tree record 不进入 `GpuArtifactHash`，不存在任何 type-tree hash 或 schema hash；hash POD API 不存在 `FromHex`。实际替换 type tree 后重算/比较 hash 的断言后移到 M3-C08；证据为 schema table、头文件 API 检查与 `rg` 日志。
- [x] **M0-C05**：`rg` 在 M0-M8 的实现项与检查站文本中找不到 cook/bake/artifact index/artifact loader/content-address publisher 作为**完成条件**；本文"后续但不阻塞"小节与 ADR-0016 对应段落列出同一份排除清单；证据为命令日志与两处文本对照。
- [x] **M0-C06**：`git check-attr -a modules/render/tests/data/shader_artifacts/<fixture>` 显示 `-text`/`binary`；把 fixture 内容改为全可打印字节后重新 checkout，字节逐字节不变；证据为命令输出。

## M1：RadRay DXC SDK 与扩展 ABI 骨架

**前置**：M0。

**实现项**：

- 在 fork 中新增 `dxcapi_radrayext.h`、RadRay CLSID/IID、`IRadRayDxcCompiler`、`IRadRayDxcResult` 和 version/ABI/schema handshake；upstream `dxcapi.h`、IID、vtable、exports 不变。
- 独立 autobuild 生成 `RadRayDXC` package：`Headers`、`Compiler`、可选 `Validator`、可选 `CLI` imported targets；identity 使用完整 upstream version + `.radray.<release>`，例如 `1.9.2607.radray.1`。
- 保留独立 SDK manifest/prefix/hash 校验由 `fetch_sdks.py` 负责；RadRay 主工程不
  `add_subdirectory`、`FetchContent` 或 `ExternalProject` DXC。
- ABI/package handshake 由 RadRay consumer test 覆盖；不再维护独立 probe 或错误 DLL fixture，
  compiler 内部语义测试归入 Clang/LLVM/DXC 自带 harness。
- fork 内部实现 M0 冻结的 128 位小端 hash（建议 XXH3_128bits）；hash 只由 compiler 计算。

**检查站**：

- [x] **M1-C01**：在干净目录对 fork SDK 执行 `cmake --install` 后，RadRay client 只包含公开的
  `dxcapi.h` 与 `dxcapi_radrayext.h` 即可创建 RadRay extension；ABI/package handshake、错误 ABI
  fail-closed 和 result lifetime 由 `test_radray_shader_compiler_client` 覆盖。compiler 内部的
  frontend 语义测试归入 ClangHLSL harness（当前本机缺少 TAEF binaries 时只阻塞该独立 harness
  target，不影响 RadRay consumer/full CTest）。
- [x] **M1-C02**：`find_package(RadRayDXC CONFIG REQUIRED COMPONENTS Headers Compiler)` 在
  `SDKs/radray_dxc/extracted` 的 Name-derived prefix 成功，CMake 使用
  `NO_DEFAULT_PATH`，`Compiler` target 只依赖 `Headers`，不隐式引入 Validator/CLI；正式 RadRay
  configure/build 与 fork SDK package 文件清单通过。默认搜索路径隔离由 CMake `NO_DEFAULT_PATH`
  固定。
- [x] **M1-C03**：`fetch_sdks.py` 对 `radray_dxc` 本地包执行 manifest/state/archive
  身份与 SHA-256 校验，并把 SDK 安装在 `SDKs/radray_dxc`；主工程 configure 只使用
  Name-derived prefix，
  不重复解析这些 JSON 或 archive。正式 compiler 配置不提供
  `RADRAY_DXC_FORK_PACKAGE_ROOT`/`RADRAY_DXC_SDK_ROOT` override。fork ABI/schema/toolchain
  identity 由 package config、client handshake 校验；fork 发布物的远程 manifest pin 仍待
  发布流水线。
- [x] **M1-C04**：fork package 的 `Compiler` install 产物包含 `dxcompiler.dll`、import library、
  public headers 与 package config（`dxcapi_radrayext.h` 随 `dxc-headers` 组件）；打包产物还含
  `bin/dxc.exe`、`bin/dxil.dll` 与 `lib/dxil.lib`。RadRay copy rule 通过 `RadRayDXC::Compiler`
  imported target 部署 runtime，compiler-off 不创建该 rule。证据为 package 文件清单与 formal build。
- [x] **M1-C05**：真实 D3D12/Vulkan PSO、draw、同步和 artifact 消费统一由 RadRay 的
  `RadRayRenderPsoSmoke`、`RadRayRuntimeShaderJit` 及全量 CTest 覆盖；fork 自有的
  `radray_dxc_d3d12_smoke` executable 已删除，避免把 compiler 内部语义测试复制成独立程序。

## M2：Source contract discovery 与 HLSL authoring

**前置**：M1。

**实现项**：

- fork frontend 正式解析根 `.hlsl` 的 `#pragma radray_keyword_group`；`.hlsli` **不**扩张 Pass domain，pragma 必须位于条件编译之外。
  这是相对老实现的**破坏性行为变更**：老 `view.hlsli` 在 `.hlsli` 里声明两个 keyword group，
  每个 include 它的入口自动继承；新契约下 group 只能由根 `.hlsl` 声明。M-1 已删除该文件，
  新 pass 必须把 group 写在入口。
- 通过标准 shader-stage attributes 推导 entry topology；禁止 stage/entry 列表由 caller 维护，禁止一个 source unit 混合 graphics/compute。
- 使用 DXC 内建 `__spirv__`（及 compiler version macros）区分 SPIR-V lane。注意这是**首次真正启用**
  `vk::binding`/`vk::push_constant`：老 `platform.hlsli` 的 gate 是 `defined(VULKAN)`，而
  `VULKAN` 从未被任何编译路径定义（`_BuildCompileArgs` 只在 SPIR-V lane 加 `-spirv -fspv-reflect`），
  因此 `VK_BINDING` 等宏一直恒展开为空，SPIR-V lane 的 set/binding 全靠 DXC 默认映射。
  切到 `__spirv__` 是实质 ABI 行为变更，必须配套测试 set/binding、push constant 与
  Root Signature/static sampler bridge。
- 新 `core/platform.hlsli` **只封装 target gate，不封装编号**：保留 `VK_BINDING(b, s)` 这类宏但
  gate 改为 `defined(__spirv__)`（DXIL lane 下裸写 `[[vk::*]]` 非法，这是宏存在的唯一硬需求）；
  **不重建** `RADRAY_FORWARD_*` 那种把 group 与 slot 编号一起打包的宏 —— 那层是老 manifest ABI
  的产物，会把 DXIL/SPIR-V 两套数字强制耦合，而新契约明确"两套数字不要求相等"。
- 实现 compiler-owned `DiscoverSourceContract`：对 DXIL/SPIR-V 各做轻量 discovery，比较 domain、entry names/stages、kind/cardinality，输出一个 canonical `ContractHash`。
- 编译请求只接受结构化 `Defines`、`KeywordAssignments` 和 typed `CompilePolicy`，不接受 raw `-D` 或 raw DXC arguments。

**检查站**：

- [x] **M2-C01**：`RadRayShaderContract` 中合法 fixture 的两个 target lane 得到相同 topology/domain/hash；`__spirv__` 只改变允许的 target facts；证据为 canonical contract golden。
- [x] **M2-C02**：重复 group、跨组重复 keyword、非法 assignment、conditional entry、缺 vertex/重复 stage、graphics/compute 混合、`.hlsli` 内声明 group 均在 discovery 阶段失败，并有稳定 diagnostic code；证据为负向测试日志。
- [x] **M2-C03**：普通 define 不能覆盖 keyword domain；assignment 顺序被 canonicalize；相同逻辑输入的 discovery result 与 hash 稳定；证据为 canonical request dump。
- [x] **M2-C04**：改变 source/include 的 contract 部分后 discovery 产生不同 `ContractHash`；只改变函数体或普通 include 内容但保持 topology/domain 时 `ContractHash` 不变；不允许 caller 自行扫描 pragma。`CompileVariant(ExpectedContractHash)` 的 drift 拒绝与 `CompileInputHash` 变化后移到 M3-C10；证据为正/负 discovery 测试。
- [x] **M2-C05**：新写的 graphics、depth-only 与 compute fixture 各至少一个，全部使用标准 `[shader("...")]` topology、根 `.hlsl` 声明的 keyword group、`__spirv__` gate 与新 `VK_*` 宏；`rg` 证明 fixture 中不存在编号封装宏；证据为 `RadRayShaderContract` 输出与命令日志。

## M3：Compiler 内 concrete Variant 编译与 metadata wire v1

**前置**：M2。

**实现项**：

- 实现 `CompileVariant`：caller 提供一个确定 assignment 和 target mask；DXIL/SPIR-V lane 独立编译、优化、校验，任一 requested lane 失败则整个 batch 失败。为生成 SPIR-V lane 的 immutable sampler metadata，compiler 可以在内部额外执行 DXIL-mode RootSignature/static-sampler analysis；该辅助 lane 不产生可访问的 DXIL result，result 只返回 caller 请求的 target lane。
- compiler 内按每个 active stage 资源做 variant-level merge；每个 Variant 只输出实际使用的 binding 集合与 stage visibility。
- DXIL 解析 HLSL RootSignature 并投影 exact active Root Signature；SPIR-V 读取标准 `vk::binding`/`vk::push_constant`；静态 sampler 按 declaration identity 生成 immutable sampler metadata。compute entry 独立解析自己的 RootSignature。
- 输出 target-specific metadata wire：entry facts、active bindings、target-native layout、vertex reflection、完整 cbuffer/struct member type tree、RootSignature/immutable sampler facts、`BytecodeHash`、`PipelineLayoutHash`、`GpuArtifactHash` 和 toolchain identity；compiler 输出的 type-tree record 不作为独立 hash 输入。辅助 DXIL analysis 的事实只投影为请求 lane 所需的 immutable-sampler metadata，不改变 result lane 集合。
- metadata 原样作为 result blob 返回；不得让 runtime/cook 重序列化或通过 reflection 再推导。

**检查站**：

- [x] **M3-C01**：`RadRayDxcMetadata` 中同一 concrete assignment 重复编译的 bytecode、metadata、`BytecodeHash`、`PipelineLayoutHash` 与 `GpuArtifactHash` 逐字节/逐值稳定；证据为双 target lane 与 stage bytecode 重复编译断言；不同 assignment 的 layout 矩阵仍待真实 fork metadata。
- [x] **M3-C02**：no-resource、texture/sampler、shadow、未使用资源、不同 target binding 集合 fixture 的 active set 与 visibility 逐项匹配；inactive declaration 的 lookup 明确失败；`RadRayRenderShaderArtifact` raw golden 与 `RadRayRuntimeShaderJit.FixtureCaseReportCoversTargetNativeJitFacts` 提供证据。
- [x] **M3-C03**：DXIL fixture 证明多个 RootConstants 合法并分别可查；SPIR-V fixture 证明多个 ranges 仍来自单一 push block，第二个 active block 编译失败；`RadRayDxcMetadata.RootAndPushConstantMetadataFacts` 提供 diagnostics 与 metadata 证据。
- [x] **M3-C04**：`RadRayDxcMetadata` 已由真实 fork 覆盖 graphics 不同 RootSignature、compute
  独立 RootSignature，以及显式 RootSignature 与 active resource union 不一致时的 diagnostic
  `2106` fail closed。
- [x] **M3-C05**：`RadRayDxcMetadata.StaticSamplerIsTargetSpecificButImmutable` 已由真实 fork
  覆盖 static sampler declaration identity、immutable flags 与 DXIL/SPIR-V target-specific
  binding 数字；缺失、重复/冲突 policy 与 SPIR-V-only request 仍保持 fail closed。
- [x] **M3-C06**：DXIL + SPIR-V batch 中任一 requested lane 故意失败时，`RadRayDxcMetadata.FailedLaneDoesNotPublishPartialBatch` 断言 result 为 failed、所有 target lane 不可访问且 diagnostics 保留 lane failure；没有成功 lane 半成品。
- [x] **M3-C07**：`RadRayRenderShaderArtifact` 覆盖截断 envelope、magic/range、target/toolchain identity、GPU hash、duplicate/invalid records；decoder 全部 fail closed，wire 中没有 native pointer 或平台 ABI。
- [x] **M3-C08**：两个 target lane 逐项断言 nested member、array/matrix stride、offset/size，`NestedArray::Values` 与 `NestedRoot::Data` 携带并校验 underlying `TypeIndex`，且 type-tree mutation 不改变 `GpuArtifactHash`；formal fork 重新生成的 DXIL/SPIR-V golden 与 `RadRayRenderShaderArtifact` 提供 compiler/fork 和 CPU schema 消费证据。
- [x] **M3-C09**：`modules/render/tests/data/shader_artifacts/` 下 M0 手写 metadata blob 已由
  `radray_shader_fixture_generator` 通过 formal fork preset 重新生成；`RadRayShaderContract`
  expected target facts 未改变，`RadRayRenderShaderArtifact` 与 formal `RadRayDxcMetadata`
  suite 均通过，`.gitattributes` 保持 raw binary `-text`。
- [x] **M3-C10**：`CompileVariant(ExpectedContractHash)` drift rejection 与 root bytes、逻辑 `SourceName`、实际打开 include 的逻辑路径/内容与顺序、assignment、Defines、target、policy mutation matrix 已由 `RadRayDxcMetadata` 覆盖；重复运行逐字节稳定，物理路径不在 request identity 中。

## M4：`radrayshadercompiler` client 与 CMake 能力开关

**前置**：M1、M3 ABI 稳定。

**实现项**：

- 新建可选 `radrayshadercompiler`，只依赖 `RadRayDXC::Headers`，动态加载 canonical bare platform library name，使用 extension CLSID/ABI/toolchain identity 校验，不回退 upstream compiler API。
- 将 `DiscoverSourceContract`/`CompileVariant` request/result、COM lifetime、diagnostics 和 blob ownership 映射为 RadRay C++ API；不拥有 render layout、asset identity、coverage、cook 或 JIT policy。
- 把 `RADRAY_BUILD_SHADER_COMPILER` 的语义从 M-1 的"有 stock DXC headers"升级为"发现并导入
  RadRay DXC SDK + 构建 client"；重新引入 `RADRAY_ENABLE_SHADER_JIT`（依赖 compiler）；
  未来 `RADRAY_BUILD_SHADER_TOOLS` 默认 OFF 且依赖 compiler。compiler 开启时公共 build output
  自动带 compiler binary。重建 `tools/CMakeLists.txt` 与根 `add_subdirectory(tools)`（M-1 已删）。
- 按 `RadRayDXC::Headers/Compiler/Validator/CLI` 做组件裁剪；纯 runtime preset 完全不 find package。
- **建立两个隔离 CMake preset**（原计划在 M8，前移到此处，因为 M4-C03 与 M7-C03 都依赖它们）：
  `win-x64-debug-shader-compiler`（compiler ON、JIT ON、tools OFF，binaryDir `build_shader_compiler`）
  与 `win-x64-debug-runtime-only`（compiler/JIT/tools OFF，binaryDir `build_runtime_only`）。
  两者不共享 build cache。实际输出目录是 `build_<name>/_build/<Config>/`
  （`RADRAY_BUILD_PATH` 默认 `${CMAKE_BINARY_DIR}/_build`，`radray_set_build_path` 再加 `$<CONFIG>`）。
- `RadRayRenderPsoSmoke` 的门控跟随改名后的 `RADRAY_BUILD_SHADER_COMPILER`；runtime-only preset 下不注册。
- **install/export 层不在第一期建立**。RadRay 自有 CMake 目前没有任何 `install(...)`，
  四个模块是纯 `STATIC` 且无 `EXPORT`；建立 install/export 层是独立工作量，与 compiler-free
  边界验证无关。第一期的 compiler-free gate 全部走 build-tree 证据（见 M4-C03、M8-C01）。

**检查站**：

- [x] **M4-C01**：`RadRayShaderCompilerClient` 用 canonical bare library name 成功加载并用缺失库名
  负向验证平台搜索路径；`test_radray_shader_compiler_client.map` 不含 `radrayrender`、
  `radrayruntime`、D3D12/Vulkan backend，`llvm-readobj --coff-imports` 未发现 compiler DLL
  import。证据为 compiler preset 的目标构建、map 文件和 loader CTest 输出；map 中的
  `dxcompiler` 字符串仅是 client 的动态库名字，不是 import。
- [x] **M4-C02**：fork 编译路径只编译 extension client，不含 upstream `IDxcCompiler3` fallback；
  formal `RadRayDxcAbiProbe` 正向通过，stock DXC 与错误 ABI fixture 的负向 probe 通过，失败时
  不发布 lane。dev stock adapter 仅保留在显式开发 preset。
- [x] **M4-C03**：compiler preset 输出目录存在 `dxcompiler.dll` 与 compiler tests；runtime-only 递归文件清单匹配 0 个 DXC/compiler 文件，cache 无 `RadRayDXC_DIR`/`RADRAY_DXC_*`，configure 只走内建选项 gate，不发现 RadRayDXC package。
- [x] **M4-C04**：compiler-off + JIT-on 与 compiler-off + tools-on 两次 configure 均以退出码 1 拒绝；tools 默认 OFF 时 CTest discovery 不注册 shader tools。
- [x] **M4-C05**：`win-x64-debug-shader-tools` 独立 configure/build 注册 `radray_shader_compile`；生成的
  Visual Studio target graph 只引入 `radraycore`、`radrayshadercompiler` 与基础依赖，未引入
  `radrayrender`、`radrayruntime`、D3D12/Vulkan backend。`radray_shader_compile.map` 与
  `llvm-readobj --coff-imports` 均通过，CLI 对 `shaderlib/passes/depth.hlsl` 输出 DXIL/SPIR-V
  两个 raw metadata blob。该工具不宣称 artifact index、cook/publisher 或 fork ABI 完成。

## M5：`radrayrender` target-native runtime representation

**前置**：M3 的 golden metadata；可与 M4 并行实现，但不得改变 M3 wire。

**实现项**：

- 将 bytecode view、metadata decoder、target-native binding/layout、vertex interface、完整 CPU type tree 和 backend pipeline-layout 构造拆分：wire contract、metadata decoder 与双 target artifact view 归入独立 `radrayshader`（不依赖 DXC/render），target-native layout 与 vertex schema 校验留在 `radrayrender`。decoder API 必须接收独立可信的 `expected GpuArtifactHash`，与 metadata 中 compiler-produced hash 比较；不得在 RadRay 侧重算 hash。
- **公共 RHI 不再表达 pipeline layout**。删除 `PipelineLayoutDescriptor`、`PushConstantDescriptor`、
  `ShaderParameterSetLayoutDescriptor`、`ShaderParameterSetLayoutEntryDescriptor` 四个结构与
  `Device::CreatePipelineLayout`。改为 backend-specific 入口：D3D12 从 DXIL metadata view 构造，
  Vulkan 从 SPIR-V metadata view 构造，各自直接吃对应 target 的 payload。公共 `PipelineLayout`
  保持现有纯虚基类形态（只有 `GetTag()`，无数据），`RootSigD3D12`/`PipelineLayoutVulkan`
  继续实现它。这让"DXIL 与 SPIR-V 不可互换"由类型系统承担，而非靠测试覆盖去追；
  "多个 RootConstants"的表达问题也随之消失 —— 它只存在于 DXIL payload 里。
- **提交路径只使用不透明 `BindingHandle`**。`rhi.h` 提供 `BindingHandle`（32/64 位不透明值），
  由 backend 的 `PipelineLayout` 在解析 HLSL declaration name 时颁发，backend 内部解回
  root parameter index / descriptor set+binding / push range。
  `ShaderParameterSet::Set` 与 `SetPushConstants` 改收 handle，不再收 `(groupIndex, binding)` 数字。
  `BindShaderParameterSet` 的 `groupIndex` **保留** —— binding group 同时是 D3D12 register space
  与 Vulkan descriptor set index，这是后端已硬化、任何一层都不重映射的不变量，是唯一可以
  留在公共层的数字。handle 不跨 target/Variant/recompile 保证数值稳定；`RADRAY_IS_DEBUG` 下
  handle 内嵌 layout generation 以拦住跨 layout 误用。
- **layout 与 compiled Variant artifact 一对一**，不共享。`PipelineLayoutHash` 只作为 metadata
  字段用于比较，不驱动任何缓存。M-1 已删除 `PipelineLayoutCache`/`SharedPipelineLayout`，
  第一期不重建：生命周期变成"artifact 死则 layout 死"，没有 cache detach、refcount 或关停顺序问题
  （老实现专门有两个用例守 `CacheMayDieBeforeItsHolders`）。共享机制记入"后续但不阻塞"。
- `ComputePipelineStateDescriptor` 的 `PipelineLayout*` 来源跟随上述改动。
- PSO builder 将 compiler vertex reflection 与外部 `PrimitiveVertexLayout` 组合；compiler 不拥有 stride/slot/step/format/offset。

**检查站**：

- [x] **M5-C01**：`RadRayRenderShaderArtifact` decoder golden tests 覆盖两个 target 的全部 11 个 fixture（含 compute），并逐 lane 调用 `MakeBackendPipelineLayoutInput`；运行时不调用 DXIL/SPIR-V reflection API。
- [x] **M5-C02**：DXIL register/space 与 SPIR-V set/binding 不一致的 fixture 同时成功；typed artifact overload 与反向入口的 `static_assert` 编译边界、target-native layout input 和双 backend PSO smoke 均通过。
- [x] **M5-C03**：artifact suite 覆盖 static sampler immutable 标记、multiple DXIL RootConstants、single SPIR-V push block、两 target nested type layout 与 target-specific vertex interface；schema v3 为 composite member 提供 `TypeIndex`，render decoder 在 layout/CPU schema 消费前校验其范围和根类型，正式 fork golden 已锁定该 payload。
- [x] **M5-C04**：`RadRayRenderShaderArtifact.RejectsDuplicateAndMalformedRecords` 对 synthetic compiler-free artifact 覆盖 duplicate entry/binding、type parent 越界和 root-constant size 非法；同 suite 另覆盖截断、range、GPU hash 与 toolchain mismatch，均在 layout 创建前 fail closed。
- [x] **M5-C05**：共享 `ValidateVertexInputState` 在 D3D12/Vulkan native PSO 前拒绝空 semantic、duplicate location、UNKNOWN format、未声明 slot 和 offset/stride 不兼容；双 backend 各有正/负 PSO smoke。
- [x] **M5-C06**：`radrayshadercompiler` target 只链接 `radraycore`、`radrayshader` 与 DXC headers；`radrayrender` library target 未链接 compiler/runtime，且 shader compiler vcxproj 没有 render/runtime/backend project reference；Pso smoke 的 SDK 依赖仅在 test target。
- [x] **M5-C07**：`rhi.h` 已删除旧 layout descriptor 与 `Device::CreatePipelineLayout`；parameter set 写入使用 `BindingHandle`，`BindShaderParameterSet` 仍接收 group index；证据为头文件扫描与 target-native backend API。

## M6：`radrayruntime` JIT orchestration 与双后端垂直切片

**前置**：M4、M5。

**实现项**：

- 将 assignment 选择、JIT 调度/缓存和 fail-closed policy 放入 `radrayruntime`；第一期不实现 artifact index、磁盘 artifact loader 或 AssetId/PassName publication mapping。
- 开发期 JIT 只在明确 capability 开启时调用 client；请求带 expected ContractHash 和一个 concrete assignment；不提供 legacy/new fallback。
- **caller 传给 compiler 的 `SourceName` 必须是 shaderlib-root-relative 的逻辑路径**。它进入
  `CompileInputHash`，同时决定诊断可读性与 include 解析基准；orchestration 是唯一有能力保证
  这一逻辑 identity 的层。
- 以 per-Variant artifact 为 key 重建 PSO 缓存（M-1 删除的 `PipelineStateCache` 以 `ShaderPassProgram*`
  为 key，与新契约无法对齐，故重写而非改造）。
- 使用 M0 golden HLSL fixture 与 M3 compiler result 形成临时内存 JIT artifact，验证"compiler result 原样交给 render decoder"。这不是 cook，也不引入生产 JSON 或 artifact index。

**检查站**：

- [x] **M6-C01**：`RadRayRuntimeShaderJit` 的 D3D12/Vulkan graphics draw、Vulkan validation 与两个 backend compute dispatch 均通过硬编码 pixel/buffer readback 断言；当前 compiler preset GPU smoke 5/5 通过。
- [x] **M6-C02**：`RadRayRuntimeShaderJit.FixtureCaseReportCoversTargetNativeJitFacts` 覆盖 no-resource graphics、texture+sampler、depth-only/static sampler、多个 DXIL RootConstants、SPIR-V push block、target-specific binding 集合、compute 七组场景。
- [x] **M6-C03**：`RadRayRuntimeShaderJit.InvalidRequestAndArtifactIdentityFailClosed` 覆盖 target mask 缺失、keyword assignment 非法、ContractHash drift、corrupt metadata 与 toolchain mismatch；JIT 不切换另一 lane。
- [x] **M6-C04**：binding handle 只能由当前 compiler result 的 HLSL declaration name 解析；inactive/unknown name 查找失败；`RADRAY_IS_DEBUG` 下跨 layout 使用 handle 被 assert 拦住；`RadRayRenderPsoSmoke` 的 D3D12/Vulkan debug death test 与 artifact/JIT lookup 覆盖上述行为。
- [x] **M6-C05**：当前 runtime JIT 路径只调用 `radrayshadercompiler` client；源码与 compiler-enabled target graph 无 SPIRV-Cross 或 artifact loader，runtime-only artifact test 的 COFF imports 也无 compiler DLL。

## M7：新 shaderlib pass 与文档重建

**前置**：M6 的双后端垂直切片稳定。

**实现项**：

- 在 M-1 保留的着色数学层（`core/{math,color,frame}`、`bsdf/*`、`lighting/*`、`shadow/*`）之上
  **从头写**新的 forward/depth/compute pass：标准 stage attributes、根 `.hlsl` 声明的 keyword
  pragma、`__spirv__` 条件宏、标准 `vk::binding`/`vk::push_constant`、可复用 RootSignature、
  新 `core/platform.hlsli` 的 target-gate 宏。不迁移老 pass —— 老绑定层已在 M-1 删除。
- PassName/AssetId 在资产调用方映射，不进 compiler request/metadata/hash。
- **重建 `docs/architecture/shader-pipeline.md`** 与 **`docs/guide/shader-authoring.md`**
  （M-1 已删除同名文件），内容描述新契约：compiler-owned metadata、双 target lane、
  discovery/CompileVariant、per-Variant layout、binding handle。
- 同步更新 `docs/architecture/shaderlib.md`（新 HLSL 库分层与 target gate 约定）、
  `docs/architecture/overview.md` 的模块表与文档索引、`docs/guide/build-test.md` 的选项表与
  target→suite 对照表、`AGENTS.md` 的依赖链（补回 client）与文档索引表。

**检查站**：

- [x] **M7-C01**：`RadRayShaderLibPass` 2/2、`RadRayRuntimeShaderJit` 9/9（含七类 fixture report 与 Vulkan validation）、`RadRayDxcMetadata` 8/8 和 `RadRayDxcAbiProbe` 2/2 通过；GPU 结果使用测试内硬编码断言，没有引入基线机制。
- [x] **M7-C02**：architecture/guide 扫描无旧并行 metadata 或 reflection-validation 路线；当前 `RADRAY_BUILD_SHADER_TOOLS` 命中属于 M4 的 tools-only gate 与 raw CLI，不再声称它是唯一默认选项命中。
- [x] **M7-C03**：shader compiler preset configure/build 成功，CTest discovery 注册 shader compiler/JIT/Pso suites；独立 tools-only preset 额外构建 raw CLI，CTest 不注册该 CLI，且 map/import 检查证明不存在反向 render/runtime/backend link。
- [x] **M7-C04**：`RadRayRuntimeShaderJit.ShaderlibPassMetadataCorruptionFailsClosed` 篡改 forward pass compiler metadata 后明确失败；路径不重新反射，也没有 JSON fallback。
- [x] **M7-C05**：`python tools/check_docs.py` 通过（当前复核为 234 files / 34 docs / 104 anchors）；overview、AGENTS 文档索引与实际新增文件一致。

## M8：第一期切换完成与 compiler-free build gate

**前置**：M7 全部通过。

**实现项**：

- 设置默认产品路径为新 HLSL + forked DXC contract；确认 M-1 的删除没有任何回流。
- 纯 runtime preset（M4 已建立）不发现/导入 RadRay DXC，不编译 client/tools，不复制 compiler binary、headers、CLI、import libraries、`dxcompiler`/`dxil`，只保留 artifact decoder/layout/backend consumption。
- 用 M3 编译并版本控制的 raw golden bytecode/metadata fixture 作为 compiler-free decoder boundary test 输入；测试直接传入 target bytecode/metadata 和 fixture table 提供的 expected `GpuArtifactHash`，不使用 artifact index/loader；正式 cook/publisher 不在本里程碑实现。
- 完成 docs index、CONTEXT 术语、ADR 状态和本 TODO 的收尾记录；确认逻辑 `SourceName`/include identity 与物理路径排除规则在三份权威文档中一致。

**检查站**：

- [x] **M8-C01**：runtime-only fresh configure/build 成功；输出文件扫描 0 个 DXC/compiler/client 文件，cache 0 个 `RadRayDXC_DIR`/`RADRAY_DXC_*` 条目；两次非法能力组合 configure 均明确失败。
- [x] **M8-C02**：runtime-only `RadRayRenderShaderArtifact` 4/4 通过 raw golden consumption；`llvm-readobj --coff-imports` 未发现 `dxcompiler`/compiler import；GPU hash mismatch fail closed。
- [x] **M8-C03**：formal fork shader compiler preset build 成功，
  `build_shader_compiler/_build/Debug/dxcompiler.dll` 存在，compiler tests、fixture generator
  与 JIT binary 均生成；cache 的 `RADRAY_DXC_PACKAGE_MODE=fork`。
- [x] **M8-C04**：compiler/runtime 两个隔离 preset binary directory 分别为 `build_shader_compiler`/`build_runtime_only`，另有
  `build_shader_tools` 的 tools-only preset；compiler preset 注册 `RadRayShaderCompilerClient`、`RadRayDxcMetadata`、
  `RadRayShaderLibPass`、`RadRayRuntimeShaderJit` 与 Pso，runtime-only 只注册 `RadRayRenderShaderArtifact`
  作为 compiler-free shader suite，tools-only 只构建 `radray_shader_compile`。
- [x] **M8-C05**：D3D12/Vulkan Pso smoke、runtime JIT graphics/compute smoke 与 `RadRayRuntimeShaderJit.VulkanValidation` 均通过；validation layer 已真实加载并完成 draw/readback。
- [x] **M8-C06**：`git diff --check`、`python tools/check_docs.py`、target graph/static import inspection 已通过；2026-08-08 formal compiler CTest 为 182/182，runtime-only artifact suite、tools CLI raw 输出和 map/graph 证据已复核；未将 stock adapter 结果写成 fork ABI 完成证据。

## 后续但不阻塞第一期

以下内容保留为后续 TODO，不得作为 M-1 至 M8 的完成条件：

- 正式 cook/bake enumeration、artifact index/loader、artifact publisher、内容寻址目录发布、
  完整平台 coverage、生产 AOT artifact index 生成、纯 runtime release 包验收。
- **RadRay install/export 层**：四个模块目前是纯 `STATIC` 且无 `EXPORT`/`install(TARGETS)`。
  建立它之后才能做 install-tree 的 compiler-free 验收与 `Runtime`/`ShaderCompiler` 组件裁剪。
- **PUBLIC 头传播缺口**：build-tree 证据无法覆盖"`radrayrender` 的公共头间接 include 了
  DXC header"这一场景 —— 它编得过（include path 在），只有 install + 下游 consumer 编译才会
  暴露。M5-C06 的 `rg` 只能部分兜住直接 include。这个缺口随 install 层一并关闭。
- **layout 共享**：第一期 layout 与 artifact 一对一。等 cook 落地、真实 variant 数量已知后，
  再按 `PipelineLayoutHash`（来自 compiler metadata，不由 runtime 重算）引入共享，并重建
  refcount/detach/关停顺序机制。
- **GPU 结果基线机制**：`RADRAY_TEST_UPDATE_BASELINE` 已由 `radray_add_radray_gtest_case` 注入
  但无消费者。若未来 GPU 断言数量增长到硬编码不可维护，再建基线文件机制。
- **Metal**：`RADRAY_ENABLE_METAL` 与 `ShaderBlobCategory::MSL`/`METALLIB` 已在 M-1 删除。
  重新引入时按新契约新增 target lane 与 enum 成员，不复用旧名。
- **imgui**：全套已在 M-1 删除。重新引入时其 pass 必须按新契约书写绑定，不得手写
  `[[vk::*]]` 字面量。

后续实现必须直接消费本 ADR 的 v1 compiler wire，不得重新引入手写 JSON、runtime reflection、
type-tree hash 或临时第二套 metadata。
