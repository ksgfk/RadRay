> - 适用: 恢复依赖、配置与构建 RadRay、运行测试或生成编译数据库
> - 权威: 本文说明仓库构建操作；选项与预设以锚点中的 CMake 文件为准
> - 锚点: `CMakeLists.txt`, `CMakePresets.json`, `cmake/Utility.cmake`, `project_manifest.json`, `tools/fetch_third_party.py`, `tools/fetch_sdks.py`, `tools/win_gen_compile_commands.py`, `modules/*/tests/CMakeLists.txt`

# 构建与测试

命令在仓库根执行。Windows Ninja 预设需要已配置 x64 C++ 工具链的终端，以及 CMake、Ninja、
Python 和 Git。现有构建目录的 generator 以其 `CMakeCache.txt` 为准，不直接用另一 generator
覆盖；需要切换时用 `-B` 指定独立构建目录。

## 恢复与构建

```powershell
python tools/fetch_third_party.py restore
python tools/fetch_sdks.py restore
cmake --preset win-x64-debug
cmake --build build_debug --config Debug --parallel 24
```

恢复脚本读取 `project_manifest.json`。第三方源码进入 `third_party/`，RadRay DXC package
进入 `SDKs/radray_dxc/extracted`；不手改这些目录。shader compiler 开启时需要匹配的 fork
package，stock DXC 不能替代。包的 ABI 与部署边界见 [Shader pipeline](../architecture/shader-pipeline.md)。

`win-x64-debug` / `win-x64-release` 使用 Ninja，分别输出到 `build_debug` / `build_release`。
`win-x64-*-clangcl` 使用 Visual Studio 18 2026 的 ClangCL toolset；它们与 Ninja 预设默认共用
同名目录，切换时应另给 `-B`。例如新建独立 ClangCL 构建：

```powershell
cmake --preset win-x64-debug-clangcl -B build_clangcl
cmake --build build_clangcl --config Debug --parallel 24
```

二进制经 `radray_set_build_path` 放在 `<build-dir>/_build/<Config>/`，可由
`RADRAY_BUILD_PATH` 改写基础路径。macOS 的 Ninja 预设为 `macos-arm64-debug` / `macos-arm64-release`，
是否可用还取决于 manifest 对应平台依赖；不能将 Windows 验收结果当作 macOS 验证。

## 常用配置边界

| 开关 | 默认与依赖 |
|---|---|
| `RADRAY_BUILD_TESTS` | ON |
| `RADRAY_BUILD_BENCHMARKS` | 单配置 Release 或包含 Release 的多配置 generator 默认 ON |
| `RADRAY_BUILD_WINDOW`、`RADRAY_BUILD_RENDER` | ON |
| `RADRAY_BUILD_RUNTIME` | 默认 ON，要求 render 和 window |
| `RADRAY_BUILD_EXAMPLES` | 默认 ON，要求 runtime |
| `RADRAY_ENABLE_D3D12` | Windows 且 render 开启时默认 ON |
| `RADRAY_ENABLE_VULKAN` | render 开启时默认 ON |
| `RADRAY_BUILD_SHADER_COMPILER` | ON |
| `RADRAY_ENABLE_SHADER_JIT`、`RADRAY_BUILD_SHADER_TOOLS` | 默认 ON，要求 shader compiler |

验证不带编译器的 runtime 时使用独立目录：

```powershell
cmake --preset win-x64-debug-clangcl -B build_runtime_only -DRADRAY_BUILD_SHADER_COMPILER=OFF
cmake --build build_runtime_only --config Debug --parallel 24
ctest --test-dir build_runtime_only -C Debug --output-on-failure
```

该配置使 JIT/tools 一起关闭，不发现或部署 DXC。检查实际 target 边使用链接 map（MSVC
`/MAP`）或 Ninja 的 `ninja -C build_runtime_only -t commands`；Vulkan 由 volk 加载，
`dumpbin /DEPENDENTS` 不能证明静态库依赖隔离。

所有自有 target 接入 `radray_default_compile_flags`；它私有设置 C++ RTTI，
不通过 core 向第三方或外部 consumer 传播。对象查询的边界见 [Core](../architecture/core-facilities.md)。

## 测试

先完成构建再运行 CTest，不并发执行两者。`-R` 匹配注册用例名中的 gtest suite，
不是 CMake target；以下是常用目标与 suite 的对应关系，可能还包含同一目标内的其他 suite：

| CMake target | `ctest -R` 示例 |
|---|---|
| `test_runtime_type` | `RuntimeTypeIdTest` |
| `test_asset_slot` | `AssetSlotTest` |
| `test_frame_upload` | `FrameUploadTest` |
| `test_scene_render_state` | `SceneTransformTest`, `SceneRenderStateTest` |
| `test_asset_database` | `AssetDatabaseTest` |
| `test_component_rtti` | `ComponentRttiTest` |
| `test_service_registry` | `ServiceRegistryTest`（含编译期组合校验） |
| `test_render_pass_registry` | `RenderPassCacheKeyTest`, `FramebufferCacheKeyTest`, `RenderPassRegistryTest` |
| `test_device_capabilities` | `TextureDescriptorValidation`, `DeviceCapabilitiesTest` |
| `test_render_foundation` | `RenderWorkloadTest`, `RenderFoundationTest` |
| `test_render_graph_compile` | `RenderGraphCompileTest` |
| `test_render_graph` | `RenderGraphTest` |
| `test_graph_contracts` | `GraphContractTest`（整数数据链、Clear/Load、layer/mip、间接工作量） |
| `test_graph_preparation` | `GraphPreparationTest`（分配/参数故障注入与恢复） |
| `test_renderer_list_pass_bindings` | `RendererListPassBindingsTest` |
| `test_forward_foundation_probes` | `ForwardFoundationProbe`（底层独立 shader，具体 suite 以 `ctest -N` 为准） |
| `test_flight_lifetime` | `FlightLifetimeTest`（真实三 flight、history 退休、外部 output） |
| `test_gpu_test_fixture` | `GpuTestFixture`, `GpuValidationProbe` |
| `test_primitive_history` | `PrimitiveHistory` |
| `test_spot_light` | `SpotLight` |
| `test_view_temporal_commit` | `ViewTemporalGpuTest`, `ViewTemporalContextTest` |
| `test_foundation_compute` | `FoundationComputeTest` |
| `test_view_state` | `ViewStateTest` |
| `test_render_outputs` | `RenderOutputTest` |
| `test_material` | `RadRayRuntimeMaterial` |
| `test_mesh_draw` | `RadRayRuntimeMeshDraw`, `RadRayRuntimeForwardSets` |
| `test_forward_pipeline` | `RadRayRuntimeForwardPipeline`, `RadRayRuntimeMaterial`, `RadRayRuntimeForwardBindings`, `MaterialTechnique`, `FrameDrawResources`, `RenderSceneSnapshot` |
| `test_forward_pipeline`（法线变换） | `ForwardNormalTransform` |
| `test_culling` | `RenderBounds`, `Culling` |
| `test_renderer_list` | `RendererList` |
| `test_stage_b_draw` | `StageBDraw` |
| `test_render_pipeline` | `RadRayRuntimeRenderPipeline`, `RadRayRuntimeRenderSystem`, `RuntimeLayering`, `RadRayRuntimeForwardPipeline` |
| `test_runtime_shader_jit` | `RadRayRuntimeShaderJit` |
| `test_radray_render_shader_artifact` | `RadRayRenderShaderArtifact` |
| `test_radray_shader_contract` | `RadRayShaderContract` |
| `test_radray_render_shader_layout` | `RadRayRenderShaderLayout` |
| `test_radray_render_d3d12_layout` | `D3D12DeviceFixture` |
| `test_radray_render_vulkan_layout` | `VulkanDeviceFixture` |
| `test_radray_render_pso_smoke` | `RadRayRenderPsoSmoke` |
| `test_radray_shader_compiler_client` | `RadRayShaderCompilerClient` |
| `test_radray_dxc_metadata` | `RadRayDxcMetadata` |
| `test_shaderlib_passes` | `RadRayShaderLibPass` |

```powershell
cmake --build build_debug --config Debug --target test_asset_slot --parallel 24
ctest --test-dir build_debug -C Debug -R AssetSlotTest --output-on-failure
```

全量构建后去掉 `-R` 可运行全部已注册测试。GPU 用例区分后端未编译、无 adapter、缺验证层与
初始化失败；可选后端不可用时可 `GTEST_SKIP()`，native 初始化、资源、PSO、提交或读回错误必须
失败。`RADRAY_TEST_REQUIRED_BACKENDS=d3d12,vulkan` 使必测后端缺失也失败。样例资产在被忽略的 `assets/`
下，通过源码仓库外的渠道准备。新增测试源文件后重新 configure；CTest 不负责构建。

涉及 RTTI、公共 C++ ABI 或跨静态库对象查询时，Debug 与 Release 都要分别完成全量构建，
再运行各自配置的测试。其他改动选择相关 suite 验证，不复用旧会话的通过计数。

`radray_add_test` 固定使用 `DISCOVERY_MODE PRE_TEST`：仓库曾遇到同目录多个 target 并行
POST_BUILD discovery 争用中间 JSON，导致随机失败甚至注册到错误可执行文件。将发现推迟到
CTest 阶段避免构建期竞争。修改注册逻辑时，比较各 exe 的 `--gtest_list_tests` 与 CTest 列表及
实际命令，不能仅凭 `ctest -N` 的总数判断正确性。注册写法见 [C++ 约定](cpp-conventions.md)。

`tools/run_render_validation.py` 对已构建配置串行运行 CTest，保存每用例 gtest XML、CTest JUnit、日志
和汇总 JSON。每份结果记录 SHA、未提交改动摘要、配置开关、OS/驱动以及 fixture 提供的 backend、
adapter 和 validation 属性；required backend 必须实际执行，通过总数不能掩盖后端全跳过。输出目录
必须为空，失败与环境失败单独统计。返回非零时先检查 `summary.json` 和对应 XML/日志。

```powershell
python tools/run_render_validation.py --self-test
python tools/run_render_validation.py --build-dir build_debug --config Debug --output-dir validation/debug --required-backends d3d12,vulkan
cmake --build build_debug --config Release --parallel 24
python tools/run_render_validation.py --build-dir build_debug --config Release --output-dir validation/release --required-backends d3d12,vulkan
python tools/run_render_validation.py --build-dir build_debug --config Debug --output-dir validation/gpu-check --regex "GraphContractTest|ForwardFoundationProbe" --gpu-validation
```

正常 GPU fixture 开启 D3D debug layer 或 Vulkan validation + synchronization validation。
`--gpu-validation` 另启 D3D GBV / Vulkan GPU-assisted validation，只用于少量数值用例；延迟 host-signaled
fence 压力和性能基准独立运行，避免验证层 semaphore 跟踪阻塞影响压力协议。H04 在独立测试进程注入
一条原生回调错误，单独记为 expected probe；普通验收的 unexpected validation errors 必须为零。
有意拒绝非法 graph 的产品诊断也不等同于 native validation 错误。

无 JIT 配置仍运行 graph compile、primitive history、spot、culling、renderer-list 等 CPU 用例；依赖
编译器的 GPU shader suites 不注册，CMake cache 中 JIT/compiler 的 OFF 值说明原因。runtime-only
仍可通过加载匹配 backend 的已编译 artifact 使用 runtime；源码请求不会反向链接 compiler client。

## 样例与专项验证

从仓库根运行 JIT 样例，确保源码树的 `shaderlib/` 可访问。资产根优先使用
`RADRAY_ASSETS_DIR`，否则使用构建时的仓库 `assets/`；shaderlib 不复制到输出目录。

```powershell
.\build_debug\_build\Debug\example_lambert_sphere.exe --d3d12
.\build_debug\_build\Debug\example_lambert_sphere.exe --vulkan
```

只构建 raw shader CLI 可以关闭 render/runtime/tests 并单独选择工具目标：

```powershell
cmake --preset win-x64-debug -B build_shader_tools -DRADRAY_BUILD_TESTS=OFF -DRADRAY_BUILD_RENDER=OFF -DRADRAY_BUILD_RUNTIME=OFF
cmake --build build_shader_tools --config Debug --target radray_shader_compile --parallel 24
```

Stage B 的 CPU suites 覆盖 bounds、zero-to-one 视锥、随机 AABB 参考对照、mask 与稳定 list 排序。
`MaterialTechnique` / `FrameDrawResources` 覆盖布局、资源子集、pass 局部失效、不可变 set 与 arena spill。
`StageBDraw` 在 D3D12/Vulkan 实际执行多顶点流的 snapshot → culling → lists → graph → readback；
Forward GPU suites 覆盖深度预通道、缺 DepthOnly、透明混合、共享 attachment 的多视图和多线程寿命压力。

`RenderGraphCompileTest` 以不创建原生对象的 fake device 覆盖 indirect usage/capability/alignment/range/
跨图 handle、resolve 格式/尺寸/sample/array/culling 与 raster UAV stage 拒绝。`RenderGraphTest` 在
D3D12/Vulkan 实机覆盖 Compute 生成 Draw/DrawIndexed/Dispatch 参数 → MSAA raster → resolve → Compute
→ readback 的整链，以及 array-layer resolve、canonical/数组/动态 offset/static sampler 参数、Graph
先析构后的 flight 寿命和 Pixel/Vertex raster UAV。设备缺失仍 SKIP；已经创建设备后的能力或执行失败
必须 FAIL，Vertex UAV 只有报告能力不足时才允许显式 SKIP。

Tidal Atrium 的有限帧 smoke 可直接验证 ForwardGraph 与样例 pass 的单图组合；`--tour` 还会切换深度、
线框、history、split view、RenderScale，并触发 resize/restore：

```powershell
.\build_debug\_build\Debug\example_tidal_atrium.exe --backend d3d12 --tour --frames 360
.\build_debug\_build\Debug\example_tidal_atrium.exe --backend vulkan --valid-layer --frames 120
```

`example_pipeline_probe` 直接使用内置 `ForwardPipeline`，提供 PBR 材质、cutout、移动刚体、四级联
阴影、局部灯群、透明层和间接萤火虫；不建立效果算法的金图或 SSIM 测试。以下运行打开主窗口、
分屏和离屏观察者，并在所属 flight fence 后输出 PNG、graph JSON/DOT：

```powershell
.\build_debug\_build\Debug\example_pipeline_probe.exe --backend d3d12 --profile temporal --frames 200 --tour --split --observer --fireflies --multithread --capture-dir validation/probe-d3d12 --dump-graph
.\build_debug\_build\Debug\example_pipeline_probe.exe --backend vulkan --profile msaa --frames 200 --tour --split --observer --fireflies --multithread --capture-dir validation/probe-vulkan --dump-graph
```

WASD/QE 移动，方向键转向，R/F 调曝光；F2 阴影、F3 AO、F4 分屏、F5 debug 显示、F6 TAA、F7
RenderScale、F8 Temporal/MSAA、F9 Bloom、F10 Forward+、F11 萤火虫；P 暂停第二视图，Space 暂停运动。
窗口标题显示当前配置。`--tour` 自动改变比例、尺寸、AA、暂停/恢复第二视图和历史显示，再恢复原值。
debug 包括深度/法线/motion、AO、cascade、tile 占用及 overflow、当前/历史 HDR、Bloom 与 depth pyramid。
图报告用于检查资源裁剪、物理复用和 barrier；截图只作为人工巡检证据。

RenderGraph 的 `*G08DependentGraphs*` 用例包含固定种子的 100/1000-pass chain/fan-out/mip 图，
每种重复 100 次并对照独立依赖遍历与确定性 JSON/DOT，记录 median/p95、报告容器容量与分配数。
这是所测 CPU 编译/报告容器数据，不是进程总内存或帧 GPU 时间；使用 Release 记录基线，并注明
机器与配置，不混用 Debug 数据。排查相关内存错误时，Windows/MSVC
AddressSanitizer 使用独立目录和 Developer PowerShell（PATH 需含 ASAN runtime），关闭 mimalloc：

```powershell
cmake --preset win-x64-debug -B build_graph_asan -DRADRAY_ENABLE_MIMALLOC=OFF -DRADRAY_BUILD_EXAMPLES=OFF -DRADRAY_BUILD_BENCHMARKS=OFF '-DCMAKE_CXX_FLAGS_DEBUG=/Od /Z7 /fsanitize=address' '-DCMAKE_C_FLAGS_DEBUG=/Od /Z7 /fsanitize=address' '-DCMAKE_EXE_LINKER_FLAGS_DEBUG=/INCREMENTAL:NO'
cmake --build build_graph_asan --target test_render_graph_compile test_render_graph test_view_state test_forward_pipeline test_render_outputs --parallel 24
$env:ASAN_OPTIONS = 'alloc_dealloc_mismatch=1'
ctest --test-dir build_graph_asan -C Debug -R 'RenderGraph|ViewStateTest|RenderOutputTest|OffscreenViews|MultithreadedDrawsWhileGameStateChanges' --output-on-failure
```

## 编译数据库与文档检查

Ninja 配置已开启 `CMAKE_EXPORT_COMPILE_COMMANDS`，直接让 clangd 指向构建目录，或复制到
IDE 默认读取的 `.vscode/compile_commands.json`：

```powershell
New-Item -ItemType Directory -Force .vscode | Out-Null
Copy-Item -LiteralPath build_debug/compile_commands.json -Destination .vscode/compile_commands.json
```

Visual Studio/MSBuild 构建使用仓库脚本求值项目，不编译源码：

```powershell
python tools/win_gen_compile_commands.py --build-dir build_clangcl --configuration Debug
```

默认输出 `.vscode/compile_commands.json`。IDE 设置见 [开发环境](dev-env.md)。
文档或技能修改运行 `python tools/check_docs.py` 与 `git diff --check`，详见[文档维护](documentation.md)。
