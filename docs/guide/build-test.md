> - 适用: 首次配置仓库；构建失败排查；跑测试；生成 compile_commands.json
> - 权威: 本文是当前构建与测试命令的唯一来源
> - 锚点: `CMakePresets.json`, `CMakeLists.txt`, `examples/CMakeLists.txt`, `examples/example_lambert_sphere/CMakeLists.txt`, `cmake/Utility.cmake`, `tools/fetch_third_party.py`, `tools/fetch_sdks.py`

# 构建与测试

## 首次准备

依赖树不在版本控制内，configure 前恢复依赖：

```powershell
python tools/fetch_third_party.py restore
python tools/fetch_sdks.py restore
```

两个脚本支持 `list`、`upgrade`、`lock`、`prune` 和 `refetch`。`third_party/` 与 `SDKs/`
由脚本维护，不能手工编辑。

`project_manifest.json` 的 `Artifacts` 除了远程 GitHub release 包，还支持本地包：Url 用
仓库根相对或 `file://` 绝对路径（例如 `SDKs/staging/radray-dxc-...zip`），`fetch_sdks.py`
直接复制本地 archive 而不是联网下载，并同样校验 `EnforceHash`。RadRay DXC fork 就以这种
本地包形式交付：`SDKs/` 目录不在版本控制内，fork 开发者重新打包后用新 hash 更新 manifest
即可，其他机器 restore 时按 manifest 复制/校验。

## 配置与构建

| 预设 | 生成器 / 工具链 | 输出目录 |
|---|---|---|
| `win-x64-debug` | Ninja + MSVC | `build_debug/` |
| `win-x64-release` | Ninja + MSVC | `build_release/` |
| `win-x64-debug-clangcl` | Visual Studio 18 2026 + ClangCL | `build_debug/` |
| `win-x64-release-clangcl` | Visual Studio 18 2026 + ClangCL | `build_release/` |
| `macos-arm64-debug` / `-release` | Ninja | `build_debug/` / `build_release/` |

当前 CMakePresets.json 不再提供独立的 shader-compiler / shader-tools / runtime-only 预设；
这些配置通过主预设加 `-D` 开关与 `-B` 输出目录表达（见下方「隔离边界」）。

Ninja 与 ClangCL 预设不要交替复用同一个 binary directory。Windows 机器上也可以按当前
工作树的生成器执行：

```powershell
cmake --preset win-x64-debug-clangcl
cmake --build build_debug --config Debug --parallel 24
```

二进制落在 `build_debug/_build/<Config>/`。

## 运行 shader JIT 样例

`example_lambert_sphere` 是普通 executable，不注册 CTest。它默认按 D3D12 构建，运行时把
工程根目录作为当前工作目录，把 `shaderlib/` 物理目录作为 root-relative HLSL include path；
shaderlib 不会复制到输出目录。资产根优先读 `RADRAY_ASSETS_DIR`，否则使用构建时注入的
`${CMAKE_SOURCE_DIR}/assets`。该目录不进入版本控制；运行样例前需通过项目约定的外部分发渠道
准备对应资产包，或用环境变量指向已准备的资产根。构建不复制或校验运行资产。
`RADRAY_ENABLE_SHADER_JIT=OFF` 时目标仍可构建，但应用会在创建窗口前记录错误并返回非零码。

```powershell
cmake --build build_debug --config Debug --target example_lambert_sphere --parallel 24
Set-Location F:\cpp\RadRay
.\build_debug\_build\Debug\example_lambert_sphere.exe --multithread --valid-layer
```

使用 Vulkan lane 时增加 `--vulkan`；`--d3d12` 可显式选择默认 lane。窗口关闭后程序正常退出。

## 主要选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `RADRAY_BUILD_TESTS` | ON | 构建 core、render、runtime 测试 |
| `RADRAY_BUILD_BENCHMARKS` | Release 默认 ON | 构建性能基线 |
| `RADRAY_BUILD_WINDOW` | ON | 构建窗口模块 |
| `RADRAY_BUILD_RENDER` | ON | 构建 RHI 与后端 |
| `RADRAY_BUILD_RUNTIME` | ON | 依赖 render 与 window |
| `RADRAY_BUILD_EXAMPLES` | ON（依赖 runtime） | 构建普通 executable 样例，包括 `example_lambert_sphere` |
| `RADRAY_ENABLE_D3D12` | Windows 下 ON | D3D12 backend |
| `RADRAY_ENABLE_VULKAN` | ON | Vulkan backend |
| `RADRAY_BUILD_SHADER_COMPILER` | ON | 从 `project_manifest.json` 的 `radray_dxc` 本地包发现 RadRay DXC fork SDK，构建可选 `radrayshadercompiler`、source-contract/metadata suite 与 fixture generator |
| `RADRAY_ENABLE_SHADER_JIT` | ON（依赖 compiler） | 开启 runtime 对 compiler client 的开发期 JIT；compiler 关闭时强制 OFF |
| `RADRAY_BUILD_SHADER_TOOLS` | ON（依赖 compiler） | 构建只依赖 compiler client 的 `radray_shader_compile` raw-lane 工具；compiler 关闭时强制 OFF |
| `RADRAY_ENABLE_MIMALLOC` | ON | 使用 mimalloc |
| `RADRAY_ENABLE_ZLIB` / `RADRAY_ENABLE_LIBPNG` | ON | 图片支持 |
| `RADRAY_ENABLE_LIBJPEG` | ON | JPEG 支持 |

`RADRAY_BUILD_SHADER_COMPILER=OFF` 不应改变 render 库的公共依赖。编译器能力只影响明确
门控的 client、JIT 和 compiler tests；关闭 compiler 时用独立 binary directory 隔离（见下）。

## 测试

构建完成后单独运行 CTest：

```powershell
ctest --test-dir build_debug -C Debug -R AssetSlotTest --output-on-failure
```

`-R` 匹配 gtest suite 名，不是 CMake target 名。当前 target 与 suite：

| target | suite |
|---|---|
| `test_asset_slot` | `AssetSlotTest` |
| `test_asset_database` | `AssetDatabaseTest` |
| `test_render_pass_registry` | `RenderPassCacheKeyTest`, `FramebufferCacheKeyTest`, `RenderPassRegistryTest` |
| `test_radray_render_pso_smoke` | `RadRayRenderPsoSmoke` |
| `test_radray_shader_compiler_client` | `RadRayShaderCompilerClient` |
| `test_radray_dxc_metadata` | `RadRayDxcMetadata` |
| `test_shaderlib_passes` | `RadRayShaderLibPass` |
| `test_runtime_shader_jit` | `RadRayRuntimeShaderJit`（graphics/compute readback、fixture case report、metadata negative） |
| `test_material` | `RadRayRuntimeMaterial`（vertex layout 解析、type tree 打包、多 cbuffer 配对、residency policy） |
| `test_mesh_draw` | `RadRayRuntimeMeshDraw`（排序、双后端 dynamic offset/indexed draw、material 资源按 flight 轮转） |
| `test_forward_pipeline` | `RadRayRuntimeForwardPipeline`（双后端跑真实窗口帧循环，程序化 quad 走完 ForwardPipeline 编排） |
| `test_radray_render_shader_artifact` | `RadRayRenderShaderArtifact` |
| `test_radray_shader_contract` | `RadRayShaderContract` |
| 其余 core target | 对应源码中的 suite 名 |

无可用后端设备的 GPU 测试应 `SKIP`；已创建设备后出现资源、PSO、提交或读回错误必须
`FAIL`。不要同时运行 build 和 ctest。

新增测试放在 `modules/<module>/tests/`，通过同目录 `CMakeLists.txt` 的 `radray_add_test`
注册。新增源文件后重新 configure；测试命令不会替你构建。

## 隔离边界

CMakePresets.json 只保留通用预设；隔离配置用主预设加 `-B`（输出目录）与 `-D`（选项）表达。
默认 `win-x64-debug` 即包含 compiler、JIT 与 tools（三者默认 ON）。

编译器开发路径（默认配置即覆盖，不设任何 env/cache override）：

```powershell
python tools/fetch_sdks.py restore
cmake --preset win-x64-debug
cmake --build build_debug --parallel 24
ctest --test-dir build_debug -C Debug -R "RadRayShaderCompilerClient|RadRayDxcMetadata|RadRayShaderLibPass|RadRayRuntimeShaderJit" --output-on-failure
```

正式 compiler 配置按 Manifest `Name` 使用
`SDKs/radray_dxc/extracted`，再以 `find_package(RadRayDXC CONFIG REQUIRED COMPONENTS
Headers Compiler)` 导入 fork package。`tools/fetch_sdks.py restore` 仍负责按
`project_manifest.json` 下载、版本锁定和 SHA-256 校验；CMake 配置阶段不再解析 manifest、
`.radray-sdk.json` 或 archive。配置阶段不把 DXC fork 加入 RadRay 源码树；它通过
`RadRayDXC::Compiler` 的 runtime copy 与 `RadRayDXC::Headers` 的 include target 接入。
`RadRayShaderCompilerClient` 进一步校验 RadRay extension 的 ABI、schema、toolchain identity；stock DXC
不会被当作兼容 compiler，`RADRAY_SHADER_COMPILER_FORK` 在 compiler 构建中恒定义。

shader tools 隔离路径（`-B` 覆盖主预设的 binaryDir，`-D` 关掉 render/runtime/tests）：

```powershell
cmake --fresh --preset win-x64-debug -B build_shader_tools `
    -DRADRAY_BUILD_TESTS=OFF -DRADRAY_BUILD_RENDER=OFF -DRADRAY_BUILD_RUNTIME=OFF
cmake --build build_shader_tools --target radray_shader_compile --parallel 24
```

`radray_shader_compile` 只输出 raw target metadata blob，不实现 artifact index、cook 或 publisher。

纯 runtime 路径（关 compiler，JIT 与 tools 随之强制 OFF）：

```powershell
cmake --fresh --preset win-x64-debug -B build_runtime_only -DRADRAY_BUILD_SHADER_COMPILER=OFF
cmake --build build_runtime_only --parallel 24
ctest --test-dir build_runtime_only -C Debug -R "RadRayRenderShaderArtifact" --output-on-failure
```

runtime-only 的 build tree 不应包含 `dxcompiler`、`dxc.exe`、`dxil` 或
`radrayshadercompiler` 文件，`build_runtime_only/CMakeCache.txt` 也不应出现 `RADRAY_DXC_*`
或 `RadRayDXC_DIR`。默认（compiler 开启）构建的输出目录应包含 fork 的 `dxcompiler.dll` ——
运行库部署集中在一个 `radray_dxc_runtime_deploy` custom target，消费 target 只做依赖衔接，
`radray_deploy_dxc_runtime` 不再各自拷贝。`test_radray_shader_compiler_client` 是最小 consumer
gate：它用裸的 canonical library name 验证平台搜索路径加载、ABI/schema/toolchain handshake，
并用不存在的库名验证失败；ClangCL 构建还生成
`build_debug/_build/Debug/test_radray_shader_compiler_client.map`，可与
`llvm-readobj --coff-imports` 一起检查 client/tool 没有引入 render/runtime/backend 或 compiler DLL。

## compile_commands

```powershell
python tools/win_gen_compile_commands.py --build-dir build_debug --configuration Debug
```

脚本写入 `.vscode/compile_commands.json` 使用的编译数据库；`.vscode/` 是个人配置，
不提交到仓库。
