> - 适用: 恢复依赖、配置与构建 RadRay、运行测试或生成编译数据库
> - 权威: 本文说明仓库构建操作；选项与预设以锚点中的 CMake 文件为准
> - 锚点: `CMakeLists.txt`, `CMakePresets.json`, `cmake/Utility.cmake`, `cmake/Dependencies.cmake`, `cmake/RuntimeLibrary.cmake`, `cmake/UpdateCompileCommands.cmake`, `cmake/dependencies/libjpeg.cmake`, `project_manifest.json`, `tools/build_third_party.py`, `cmake/Install.cmake`, `cmake/DependencyCatalog.json`, `tools/fetch_third_party.py`, `tools/fetch_sdks.py`, `tools/win_gen_compile_commands.py`, `tools/expand_unity_compile_commands.py`, `modules/*/tests/CMakeLists.txt`, `modules/core/tests/test_memory_override.cpp`

# 构建与测试

命令在仓库根执行。Windows Ninja 预设需要已配置 x64 C++ 工具链的终端，以及 CMake、Ninja、
Python >= 3.10 和 Git。现有构建目录的 generator 以其 `CMakeCache.txt` 为准，不直接用另一 generator
覆盖；需要切换时用 `-B` 指定独立构建目录。

## 恢复与构建

```powershell
python tools/fetch_third_party.py restore
python tools/fetch_sdks.py restore
python tools/build_third_party.py build --triplet windows-x64-msvc --config Debug --parallel 8
cmake --preset win-x64-debug
cmake --build --preset win-x64-debug --parallel 24
ctest --preset win-x64-debug
```

恢复脚本读取 `project_manifest.json`。第三方源码进入 `third_party/`，RadRay DXC package
进入 `SDKs/radray_dxc/extracted`；不手改这些目录。用户随后手动运行依赖准备脚本，
按固定平台库集合编译、安装 CMake 包。RadRay 主工程仅通过 `find_package` 消费包，
配置、构建和安装均不自动恢复或构建依赖。shader compiler 开启时需要匹配的 fork
package，stock DXC 不能替代。包的 ABI 与部署边界见 [Shader pipeline](../architecture/shader-pipeline.md)。

`fetch_third_party.py restore` 优先恢复已有 `project_manifest.lock.json` 中的提交；没有 lockfile
时按 manifest 恢复，已安装的默认分支或显式 `Branch` 不会自动检查远端更新。需要更新这些可变引用时运行：

```powershell
python tools/fetch_third_party.py restore --apply
```

`restore --apply` 与 `restore --update` 都会拉取远端默认分支或指定 `Branch` 的最新提交，固定
`Tag` / `Commit` 保持不变。已有 lockfile 的对应条目在安装成功后同步更新；没有 lockfile 时不会
自动生成。`--only xxHash` 可限定依赖，`--no-lock` 可跳过 lockfile；默认分支更新支持 detached HEAD，
并重新读取远端 HEAD，远端切换默认分支后也能跟随。升级固定版本使用 `upgrade --apply`。

十个 configure/build/test preset 使用相同名称；构建目录统一为 `build/<三元组>/<Debug|Release>/`。
Windows MSVC 使用 `windows-x64-msvc` 与 Ninja，ClangCL 使用 `windows-x64-clangcl` 与
Visual Studio 18 2026；macOS 使用 `macos-arm64-appleclang` 与 Ninja。每个目录只对应一种配置，
ClangCL 预设也将 `CMAKE_CONFIGURATION_TYPES` 限定为 Debug 或 Release。旧构建目录不自动迁移或删除。
Windows 另外提供四组 `-staticcrt` 预设与两种独立三元组，见下文[静态 CRT 与 mimalloc](#静态-crt-与-mimalloc)。

```powershell
python tools/build_third_party.py build --triplet windows-x64-clangcl --config Debug --parallel 8
cmake --preset win-x64-debug-clangcl
cmake --build --preset win-x64-debug-clangcl --parallel 8
ctest --preset win-x64-debug-clangcl
```

二进制经 `radray_set_build_path` 放在 `<RadRay-binary-dir>/_build/<Config>/`，可由
`RADRAY_BUILD_PATH` 改写基础路径。macOS 是否可用还取决于 manifest 对应平台 SDK；当前 DXC
manifest 只有 Windows 包，macOS 无匹配包时须关闭 compiler。不能将 Windows 验收结果当作 macOS 验证。

### 开发时加载 DXC

Windows configure preset 将 `SDKs/radray_dxc/extracted/bin` 加到子进程 `PATH` 前端，关联的 build/test
preset 默认继承该环境，覆盖构建期测试发现和 CTest。测试、shader CLI 与 fixture generator 不再复制
DXC DLL；它们从启动环境查找 SDK。VS Code 调试配置见[开发环境](dev-env.md#launchjson)。

`cmake --build <目录>`、`ctest --test-dir <目录>`、直接运行 exe 和其他脚本不会读取 preset 环境。
使用这些入口时，先在仓库根为当前 PowerShell 设置一次：

```powershell
$env:PATH = "$(Resolve-Path ./SDKs/radray_dxc/extracted/bin);$env:PATH"
```

环境只作用于当前终端及其子进程，configure 不会修改父终端。`CMAKE_PREFIX_PATH`、包 `_DIR` 和
`RADRAY_DXC_ROOT` 只决定 CMake 找包；改用其他 DXC 包时，启动环境也应指向该包的 bin。
旧构建目录中已复制的 `dxcompiler.dll`、`dxil.dll` 应显式移除；exe 同目录副本会优先于 `PATH`。

### 手动准备依赖包

```powershell
python tools/build_third_party.py list --triplet windows-x64-msvc --config Debug
python tools/build_third_party.py build --triplet windows-x64-msvc --config Debug --only spdlog
python tools/build_third_party.py build --triplet windows-x64-msvc --config Release --parallel 8
python tools/build_third_party.py build --triplet windows-x64-msvc --config Debug --clean
```

脚本接受上述三种三元组、两个 Windows `-staticcrt` 三元组和 Debug/Release，不读取 preset，不接受工程 `-D` 或任意 toolchain。
Windows MSVC 需要 x64 开发终端；ClangCL 使用 VS 2026 的 ClangCL toolset；macOS 使用系统
AppleClang 并指定 arm64。原 Windows 三元组使用动态 CRT（`/MD`、`/MDd`）；`-staticcrt` 三元组在两种配置中均使用 `/MT`。C++ 最低要求为 C++20。

默认准备依赖目录表中该平台的全部库，包含 GTest、Benchmark 和 Tracy；macOS 排除两个 DirectX 库。
工程 tests、profiler、backend 等开关只影响主工程消费，不影响准备列表与路径。manifest 中目前未使用的
freetype、imgui 不构建。`--only` 可重复或使用逗号，自动包含前置依赖；只检查所选源码是否存在。
缺源码会提示 `fetch_third_party.py restore --only <name>`，不自动恢复。恢复记录、lockfile 与 Git
一致性由 fetch 阶段管理，准备阶段不重复核对。`list` 只查看库、依赖关系和路径，不要求源码、SDK
或 CMake 已就绪，不创建目录。

每个包使用 `build_dependencies/<库>/<三元组>/<模式>/build/` 构建，直接安装到
`SDKs/<库>/<三元组>/<模式>/`。SDK 目录包含头文件、库、运行库与 CMake 包配置；构建缓存、对象文件
和日志留在 build 目录。
Debug/Release 不合并前缀，保留上游库名和安装文件，包括 `.pc`。每次 build 都执行
configure/build/install，由 CMake 增量构建；不维护 hash、ready 状态或安装文件校验。
源码、配方或工具链升级后显式使用 `--clean`，它只删除本次所选库及其前置依赖在当前三元组、模式下的
构建目录和 SDK 安装目录；输出路径越出对应库目录或经过链接时拒绝操作，不清理其他库或 DXC SDK。
旧 hash 目录不自动清理。
失败即停止，日志保存在对应 build 目录；不向同一包、三元组和模式并发写入。

需要编译的包默认静态，保留 spdlog 外部 fmt 与 Windows 宽字符、与三元组一致的 GTest CRT、Tracy on-demand
等必要配置。优先使用上游安装规则；IJG libjpeg 与 stdexec 使用 `cmake/dependencies/` 的离线适配，
不修改源码、不下载额外工具。libjpeg 适配器保留 `FindJPEG` 识别 Debug 所需的 `d` 后缀，
其他库不统一追加后缀。DXC 是独立 SDK 包，准备脚本不构建或验证它。

仓库 preset 设置 `RADRAY_DEPENDENCY_TRIPLET`，主工程根据当前配置把各库安装目录加入包查找路径，
并在 compiler 开启时将仓库 DXC SDK 作为查找提示。显式 `CMAKE_PREFIX_PATH`、包 `_DIR` 和
`RADRAY_DXC_ROOT` 仍可使用；不设置三元组时，由外部消费者自行提供依赖路径。包缺失时配置失败，
不回退源码构建。准备脚本成功只表示所选包安装完成，依赖集成由主工程构建和功能测试验证。

使用其他包时需自行保证 CRT、架构和编译选项兼容；Windows spdlog 必须以宽字符支持编译。

### 静态 CRT 与 mimalloc

Windows 静态 CRT 路线使用独立的三元组、构建目录和 SDK 前缀，不覆盖既有动态 CRT 包：

| 编译器 | 依赖三元组 | configure/build/test preset |
|---|---|---|
| MSVC | `windows-x64-msvc-staticcrt` | `win-x64-debug-staticcrt`、`win-x64-release-staticcrt` |
| ClangCL | `windows-x64-clangcl-staticcrt` | `win-x64-debug-clangcl-staticcrt`、`win-x64-release-clangcl-staticcrt` |

静态 CRT 路线的 Debug 和 Release 均设置 `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`，即 `/MT`。
mimalloc 3.5.3 的 Windows 静态覆盖要求使用 Release CRT，不能把 `/MTd` 当作同等支持的覆盖路径。
Debug 仍按 Debug 配置编译，保留调试信息、低优化、断言和 `RADRAY_IS_DEBUG`；mimalloc 也使用
Debug 构建的内部检查。该路线不启用 Debug CRT，由 `/MTd` 隐含的 `_DEBUG` 和 STL Debug iterator
检查也不启用；工程不手动改写 `_ITERATOR_DEBUG_LEVEL`。需要原来的 Debug CRT 行为时使用原动态
CRT 三元组。[mimalloc 3.5.3 静态覆盖说明](https://github.com/microsoft/mimalloc/blob/v3.5.3/readme.md#static-override)

以下命令由开发者显式执行；首先按前文恢复第三方源码和所需 SDK：

```powershell
python tools/build_third_party.py build --triplet windows-x64-msvc-staticcrt --config Release --parallel 8
cmake --preset win-x64-release-staticcrt
cmake --build --preset win-x64-release-staticcrt --parallel 8
ctest --preset win-x64-release-staticcrt
```

调试配置需要单独准备 Debug 包：

```powershell
python tools/build_third_party.py build --triplet windows-x64-msvc-staticcrt --config Debug --parallel 8
cmake --preset win-x64-debug-staticcrt
cmake --build --preset win-x64-debug-staticcrt --parallel 8
ctest --preset win-x64-debug-staticcrt
```

ClangCL 使用表中的对应名称。不要把原有 `/MD[d]` 构建目录改成 `/MT` 后继续使用缓存包。
`cmake/RuntimeLibrary.cmake` 在主工程和依赖准备时检查所选三元组与 CRT 是否一致；不匹配则停止，
不会自动修改宿主的 CRT。显式包 `_DIR`、`CMAKE_PREFIX_PATH`、已有 imported target 或自备包仍由
调用方保证 ABI 一致，目录隔离不能校验任意外部二进制的实际编译选项。

`RADRAY_ENABLE_MIMALLOC=ON` 时，core 继续将 `mimalloc-static` 以 `LINK_LIBRARY:WHOLE_ARCHIVE`
传递到最终 exe/DLL。在 `/MT` 下，上游标准 C 分配函数和 C++ `new/delete` 替换会参与最终链接；
RadRay 的 `Malloc/Free`、对齐分配及同一映像内静态依赖的标准分配调用通过这些符号进入 mimalloc。
Windows `/MD[d]` 下上游跳过该替换，单纯链接 mimalloc 并不意味着覆盖 CRT 或 C++ 分配。
启用的 Vulkan 分配回调直接调用 `mi_*`，不依赖 CRT 符号覆盖。工程不添加 mimalloc redirect DLL。

静态覆盖只针对最终映像内这些标准分配入口；系统/驱动内部、GPU 显存和直接使用系统内存 API 的
分配不在覆盖范围。对象和内存由分配它的模块配对释放，不跨 DLL 使用另一套 CRT 的 `free/delete`。
Windows `/MT` 下 mimalloc 默认以 C++ 编译并自带 `new/delete`，宿主不得再包含
`mimalloc-new-delete.h` 或另定义同名替换。Raynium 采用静态 CRT 三元组时也需移除其手动替换头，
并与 RadRay 统一 `/MT`；本项目不修改外部宿主配置。

`test_inline_vector` 自己替换 `new/delete` 以计数，只编译容器测试与现有 `logger.cpp`，链接
fmt、spdlog 和 GTest，不链接 core 的 mimalloc 依赖。其余 core 测试仍链接完整 core；静态 CRT
且启用 mimalloc 时加入 `MimallocOverride` suite，检查标准 C 分配及 realloc 数据保留、RadRay
分配、对齐分配、C++ 对象/数组和标准容器的 mimalloc 归属。构建完成后可单独执行：

```powershell
ctest --preset win-x64-release-staticcrt -R MimallocOverride
```

这些是已加入的构建和检查入口，尚不表示静态 CRT、最终 DLL 覆盖或运行验证已经通过。

### 编译契约与优化

Debug 保留工具链的编译检查；CRT 与 STL 调试行为取决于所选路线，工程不改写 `_ITERATOR_DEBUG_LEVEL`。宿主、RadRay 与其
第三方依赖必须使用兼容的 CRT、STL ABI 和 Eigen 编译配置。

core 公开传递 C++20 最低要求、`RADRAY_PLATFORM_*` 和 `RADRAY_IS_DEBUG`；后者保持
非 Release 配置定义的既有语义。消费者可使用 C++23，不需要调用内部编译选项函数。
`RADRAY_ENABLE_AUTO_OPTIMIZATION` 控制自有目标的 Release SIMD/FMA/IPO/LTO。独立非安装
构建默认 ON，`RADRAY_INSTALL=ON` 和子工程默认 OFF；显式设置优先。关闭时继承工具链的
架构、IPO、CRT 和 PIC 设置，不附加自动优化。此默认值在首次配置时确定。

### 预编译头与 Unity Build

项目不主动设置全局 `CMAKE_UNITY_BUILD`。未指定时，只有 `radray_enable_unity_build` 的目标默认启用，
core 与 `test_inline_vector` 都不参与。显式传入 `-DCMAKE_UNITY_BUILD=OFF` 会关闭这些目标的 unity build；
传入 `ON` 时遵循 CMake 的全局初始化行为，其他目标也可能参与。删除缓存项（`-U CMAKE_UNITY_BUILD`）可恢复项目默认。

需要原始 `.cpp` 的编译数据库时，可使用独立的非 unity Ninja 配置，无需构建：

```powershell
cmake --preset win-x64-debug -B build_clangd -DCMAKE_UNITY_BUILD=OFF
```

`radrayrender` 和 `test_radray_render` 只预编译 `<radray/render/rhi.h>`。D3D12 与 Vulkan 头不放进这份预编译头，因为一个目标只能有一份。

## 常用配置边界

| 开关 | 默认与依赖 |
|---|---|
| `RADRAY_BUILD_TESTS` | 独立构建 ON；子工程 OFF |
| `RADRAY_BUILD_BENCHMARKS` | 独立构建 ON（所有配置）；子工程 OFF |
| `RADRAY_BUILD_COMPILE_COMMANDS` | 独立构建 ON；子工程 OFF；仅 Ninja/Makefile 提供辅助目标 |
| `RADRAY_BUILD_WINDOW`、`RADRAY_BUILD_RENDER` | ON |
| `RADRAY_ENABLE_D3D12` | Windows 且 render 开启时默认 ON |
| `RADRAY_ENABLE_VULKAN` | render 开启时默认 ON |
| `RADRAY_BUILD_SHADER` | ON；独立 shader contract/decoder |
| `RADRAY_BUILD_SHADER_COMPILER` | ON；要求 shader |
| `RADRAY_BUILD_SHADER_TOOLS` | 独立构建 ON；子工程 OFF；要求 shader compiler |
| `RADRAY_ENABLE_PROFILER` | 所有配置默认 ON；链接 Tracy client，见[性能采样](performance.md#性能采样tracy) |

验证不带编译器的基础模块时使用独立目录：

```powershell
cmake --preset win-x64-debug -B build_base_no_compiler -DRADRAY_BUILD_SHADER=OFF -DRADRAY_BUILD_SHADER_COMPILER=OFF -DRADRAY_BUILD_SHADER_TOOLS=OFF
cmake --build build_base_no_compiler --config Debug --parallel 8
ctest --test-dir build_base_no_compiler -C Debug --output-on-failure
```

该配置关闭 shader/compiler/tools，render 及其测试仍可独立构建，不发现或部署 DXC。
compiler=ON 且 shader=OFF、tools=ON 且 compiler=OFF 都明确报错；切换已有 build cache 时必须同时修改相关开关。Vulkan 由 volk 加载，
`dumpbin /DEPENDENTS` 不能单独证明静态库依赖隔离。

## 作为子工程引用

先恢复源码并手动准备依赖，给宿主传入相同的 `CMAKE_PREFIX_PATH` 或 toolchain。RadRay
只通过规范包 targets 消费依赖；允许包本身复用已定义 targets，但包必须满足完整接口与 ABI 要求。
包缺失时配置失败，不回退源码构建。准备脚本的源码固定来自 RadRay 自身的 `third_party/`；
宿主可以提供外部兼容安装包，包括 libjpeg-turbo。

宿主 CMake 示例：

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyApplication LANGUAGES CXX)

# 只使用原始 shader 字节码和显式布局。
set(RADRAY_BUILD_SHADER OFF)
set(RADRAY_BUILD_SHADER_COMPILER OFF)
set(RADRAY_BUILD_SHADER_TOOLS OFF)
add_subdirectory(external/RadRay)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE RadRay::render RadRay::window)
```

子工程默认关闭 tests、benchmarks、shader CLI 和编译数据库辅助目标；compiler、Tracy、
mimalloc、图像支持与 window/render/backend 维持功能默认。使用已有 `RADRAY_*` 开关显式
覆盖；普通变量与缓存设置均受支持。mimalloc override 的 whole-archive 和 Tracy 会传递到
最终程序，宿主不需要时应显式关闭 `RADRAY_ENABLE_MIMALLOC` / `RADRAY_ENABLE_PROFILER`。

宿主的 `BUILD_SHARED_LIBS`、安装和包发现配置不会被 RadRay 强制覆盖；RadRay 自有库保持
静态库。其产物默认在自己的 binary subtree，`RADRAY_BUILD_PATH` 可改写，宿主产物目录
保持原设置。包含路径、公共宏和依赖通过 target 传递；最低 C++20，不强制降低更高标准。

启用 compiler 的消费者链接模块，并在启动时提供 DXC 运行库搜索路径：

```cmake
add_executable(shader_app main.cpp)
target_link_libraries(shader_app PRIVATE RadRay::shadercompiler)
```

仓库三元组配置将 SDK 的独立前缀作为查找提示；
`RADRAY_DXC_ROOT` 也可作为 fork package 的显式查找提示；详见 [Shader pipeline](../architecture/shader-pipeline.md#build-boundary)。

如需在宿主顶层运行 RadRay 测试，应在顶层调用 `enable_testing()` 或 `include(CTest)`，并在
引入前开启 `RADRAY_BUILD_TESTS`。

## 安装与包消费

```powershell
$env:PATH = "$(Resolve-Path ./SDKs/radray_dxc/extracted/bin);$env:PATH"
cmake --preset win-x64-debug -B build_install_debug -DRADRAY_INSTALL=ON
cmake --build build_install_debug --config Debug --parallel 8
cmake --install build_install_debug --config Debug --prefix "C:/local/RadRay"
```

`RADRAY_INSTALL` 默认 OFF。安装仅包含 RadRay 公共头、静态模块、已开启的 CLI、shaderlib、
许可证和 CMake 导出；不复制第三方、测试、benchmark 或 fixture generator。Debug/Release 可
分别构建并安装到同一前缀；该前缀只用于同一源码内容、工具链、架构、CRT、ABI 与功能组合。安装前会检查配置与源码指纹，
拒绝把不同实现或编译选项混装到已有前缀。
库放在 `lib/<Config>/`，CLI 在 `bin/<Config>/`，包配置在 `lib/cmake/RadRay/`，
shaderlib 在 `share/RadRay/shaderlib/`。对应目录可由 `GNUInstallDirs` 的相对路径变量改写。
不生成 ConfigVersion，不提供版本匹配或跨版本 ABI 承诺。

```cmake
find_package(RadRay CONFIG REQUIRED COMPONENTS render)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE RadRay::render)
```

公开 targets 为 `RadRay::core`、`RadRay::shader`、`RadRay::window`、`RadRay::render`、
`RadRay::shadercompiler`，以及 CLI 的 `RadRay::shader_compile`。源码构建提供相同别名，
旧的实际 target 名称保留。组件为 `core`、`shader`、`window`、`render`、`shadercompiler`、
`tools`；模块依赖自动加载，不指定组件时加载全部已安装组件。`RadRay_AVAILABLE_COMPONENTS`
按实际生成 target 列出安装能力。render 组件只加载 core 和启用后端的外部包，无需 shader 或 DXC。
支持重复/追加组件及标准 `OPTIONAL_COMPONENTS`；必需但未安装的组件失败。
只请求 core 不查找图形或 DXC 包；静态库 PRIVATE 链接依赖仍通过 `find_dependency` 发现。

消费者必须提供依赖前缀；移动 RadRay 安装目录后仍可消费，不依赖原源码和构建目录。
缺失 Debug 库不会自动映射到 Release；多配置消费者应安装其 `CMAKE_CONFIGURATION_TYPES`
列出的配置，或显式设置 `CMAKE_MAP_IMPORTED_CONFIG_<CONFIG>` 并承担 ABI 兼容责任。

compiler 消费者与安装后的 CLI 通过启动环境提供 DXC 运行库；包配置不执行优化探测，RadRay
普通安装不复制 DXC。需要脱离开发环境运行的完整 SDK 或工具发布包，由其打包步骤收集运行库。
`RadRay_SHADERLIB_DIR` 是可迁移路径，由消费者
显式加入 shader include 搜索路径。详见 [Shader pipeline](../architecture/shader-pipeline.md#build-boundary)。

## 性能测量

Tracy、CPU/GPU 基准、配对测量与内存口径统一见[性能测量](performance.md)。
先完成构建与正确性回归，再串行运行 benchmark；历史报告由[架构地图](../architecture/overview.md#历史资料)索引。

## 测试

先完成构建再运行 CTest，不并发执行两者；CTest 不负责构建。`-R` 匹配注册的 gtest suite，不是 CMake target。
同一模块的 GTest 源文件链接进一个可执行文件，替换全局 operator new 的 `test_inline_vector` 独占进程。
测试直接使用 CMake `GoogleTest` 模块的 `gtest_discover_tests` 注册，不附加测试专用的 CMake 版本限制。
未设置发现模式时采用官方默认 POST_BUILD；可通过 `CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE` 选择模式。
POST_BUILD 在链接后运行测试程序枚举用例，所需运行库应在启动环境中可查找；使用上面的开发环境配置。

| 目标 | suite 示例 |
|---|---|
| `test_radray_core` | `RuntimeTypeIdTest`、`ManualCoroutineScheduler` |
| `test_radray_render` | `ExplicitBindingLayoutTest`、`ExplicitLayoutOwnershipD3D12`、`ExplicitLayoutOwnershipVulkan`、`DescriptorDirtyRangesD3D12Test` |
| `test_radray_render`（D3D12 诊断） | `D3D12DiagnosticsTest`、`D3D12DiagnosticsSmoke` |
| `test_radray_render`（Vulkan） | `VulkanDeviceFeaturePolicy`、`VulkanDeviceFeatureConsumers`、`VulkanDeviceFeatureGpu`、`VulkanSwapChainRetirementTest`、`VulkanSwapChainNativeTest`；Win32 另含 `VulkanSwapChainWsiTest` |
| `test_radray_window`（Win32） | 用 `ctest -N` 查看当前注册用例 |
| `test_radray_shader` | `RadRayShaderDiagnostics`、`RadRayShaderArtifact`、`RadRayShaderContract`（不依赖 compiler） |
| `test_radray_shader_compiler`（compiler 开启） | `RadRayShaderCompilerClient`、`RadRayShaderLibPass` |

```powershell
cmake --build --preset win-x64-debug --parallel 8
ctest --preset win-x64-debug
```

render 测试覆盖显式布局、各类资源、数组、固定 sampler、动态偏移、部分 push 更新、跨 PSO/布局共享，
通过双后端真实 dispatch/读回和原生验证检查行为。三组绑定用例按 view/material/object 分层录制，
在普通参数集切换及同一 object 参数集配合动态偏移两种路径下，验证只重绑 object 时其他组仍有效。
参数集分配范围通过 D3D12 实际 CPU/GPU descriptor 区间和 Vulkan 测试侧原生分配调用记录验证，
确保每个实例只分配所属组的 descriptors；Vulkan pool 可按页预留多个实例的容量。
布局直接创建、释放和失败清理使用测试侧 probe 或原生 debug 消息观察，
不在生产代码添加测试字段。交换链回收/WSI 测试保留。
D3D12 根签名 debug 消息按实际 COM 对象身份计数，不作为创建 API 调用计数；生命周期回归覆盖
相同/不同描述、重复销毁，以及销毁一个 owner 后通过另一个完成真实 dispatch/读回。
计数观察结构化 probe 的消息区间，不清空原生队列或修改原生过滤器。
普通 validation 下消息数严格对应实际 COM 对象；GBV 可能创建内部辅助根签名，
因此额外记录每个创建区间的消息增量，检查独立释放和最终创建/销毁平衡，不硬编码辅助对象倍数。
render 测试读取 `modules/render/tests/data/shader_bytecode/`，无需 shader/compiler/DXC。
Vulkan feature 策略测试覆盖全支持时的精确请求集、逐位缺失、必需/可选分离、查询结果不变、
独立创建链与无链接快照；consumer probe 验证 sampler 及 indirect batching / 单 draw 回退读取 Enabled。
`VulkanDeviceFeatureGpu` 在启用 validation / synchronization validation 后分别测试物理设备索引 0、1，
检查实际能力快照、UAV stages、buffer limits、普通/immutable sampler、timeline 等待及销毁。
不存在的第二块设备单列跳过；已枚举 adapter 的设备创建失败必须失败，不能因为缺少 GOOGLE
反射扩展而跳过。设置 `RADRAY_TEST_VULKAN_ADAPTER_INDEX` 可让公共 GPU fixture 显式选择设备，
覆盖显式布局、dispatch/读回和 WSI；未设置时选择高性能设备，非法或不存在的指定索引必须失败。
报告记录实际 adapter、索引、validation / synchronization validation 状态与原生错误计数。
验收先分别构建 ClangCL Debug / Release，再运行上述 suites 和现有显式布局、传统 framebuffer、WSI 回归；
另用 `RADRAY_ENABLE_VULKAN=OFF` 配置构建 render 测试，确认后端关闭时无 Vulkan 链接依赖。
artifact golden 与 contract 测试位于 `modules/shader/tests/`。

RenderPass2、view 格式契约与 framebuffer 回归使用以下 suites，同样链接到 `test_radray_render`：

| Suite | 验证内容 |
|---|---|
| `AttachmentOperations` | D3D12 与 Vulkan 四种 feature/cache 组合；非均匀 MSAA resolve、源逐 sample 读回、sRGB、浮点、区域外/未 resolve aspect、局部 layers、混合权限 SRV/PSO、普通/immutable sampler 真采样 |
| `CoupledTextureViewContractTest` | 禁用 separateDepthStencilLayouts 后合法组合与独立转换拒绝 |
| `TextureViewContractTest` | 双后端同格式与兼容格式、UNORM/sRGB 独立用途、后端兼容范围差异、depth/stencil plane、sRGB 清除读回、命令引用的销毁限制 |
| `VulkanViewFormats` | Vulkan 格式兼容类与非 mutable 限制，depth/stencil 不与 color 混淆 |
| `VulkanRenderObjects` | 测试侧 native probe 观察 RenderPass2 参数、缓存创建次数、约束区分、失败/预算、独立附件引用与销毁时序 |
| `VulkanRenderScopeGpu` | 传统/imageless 两模式的 MRT、局部 clear/load/store、mip/layer、depth-only、DS、只读附件和 MSAA |
| `VulkanFramebufferGraphicsGpu` | 两模式下现有 shader 的真实 graphics draw、PSO 与读回 |
| `D3D12DiagnosticsSmoke` | 包含 swapchain UNORM 原生存储与逻辑 sRGB RTV 的回归 |

`VulkanDeviceFeaturePolicy` 同时验证 imageless 为可选请求、缺失仍可创建设备，以及 RenderPass2 必需入口检查。
传统/imageless GPU 测试在创建资源前选择缓存模式；不改 Supported/Enabled 快照。
结构收益由 probe 比较相同附件约束的不同 views 所需的 framebuffer 创建数，不将 GTest 用作性能基准。
非预期原生 validation 错误必须失败；不支持 imageless 的 GPU 用例明确跳过。
`AttachmentOperations` 参数 0 为 D3D12，1/2 为 Vulkan imageless（独立/组合 DS），3/4 为传统 framebuffer（独立/组合 DS）。
fixture 在 vkCreateDevice 前选择请求集，不在创建后伪造 Enabled；报告记录实际 separate_depth_stencil_layouts 与 imageless_framebuffer。
D3D12 报告 render_passes_valid 与 programmable_sample_positions_tier；附件 resolve 能力不足的用例单列跳过。
MinMaxFiltering 的负面格式用例在设备所有受支持 RHI 采样格式均支持时单列跳过，不能伪造格式能力。
probe 验证 resolve 目标的有序附件、约束、缓存复用、去重引用、提前销毁、预算失败/重试；不增加生产观测字段。
读回的误差只用于格式量化，测试结果不作帧率或带宽收益解释。

`DepthStencilResolveModesPreserveUntouchedAspectAndOutsideArea` 可用
`RADRAY_TEST_DEPTH_RESOLVE_STAGED_READBACK=1` 开启分阶段读回，默认关闭。
探针保留原始 render pass 与 ending resolve，在初始化后、resolve pass 的 `EndRenderPass` 后和最终检查前，
通过现有 compute shader 将源逐 sample 与目标 depth/stencil 保存到读回缓冲的独立区域。
三个快照在同一 command buffer 中录制，提交后统一等待队列完成，再映射读取；不依赖捕获工具。
`EndRenderPass` 后的快照位于必要的 ShaderRead 转换之后。日志输出中央/区域外像素、目标 stencil 范围和中央源像素的四个 sample stencil，
GTest XML 的 `resolve_probe_<depth>_<stencil>_<stage>` 属性额外保存完整 8×8 目标 stencil。
测试检查初始化值以及 pass 结束快照与最终读回一致，原有内容断言继续执行。
探针增加资源转换与 dispatch，排查时须对比开关两种运行，确认故障是否仍然复现。

```powershell
$env:RADRAY_TEST_D3D12_ADAPTER_INDEX = '0'
$env:RADRAY_TEST_DEPTH_RESOLVE_STAGED_READBACK = '1'
ctest --preset win-x64-debug-clangcl -R 'AttachmentOperations.*DepthStencilResolveModesPreserveUntouchedAspectAndOutsideArea/0' --verbose
Remove-Item Env:RADRAY_TEST_DEPTH_RESOLVE_STAGED_READBACK, Env:RADRAY_TEST_D3D12_ADAPTER_INDEX
```

同一测试的 D3D12 参数可设置 `RADRAY_TEST_DEPTH_RESOLVE_PIX_CAPTURE=1`，仅捕获 Average depth / None stencil 组合，默认关闭。
从 PIX 的 GPU Capture 启动 `test_radray_render.exe`，工作目录为仓库根，参数使用
`--gtest_filter=BackendsAndFeatures/AttachmentOperations.DepthStencilResolveModesPreserveUntouchedAspectAndOutsideArea/0`。
测试使用 Windows SDK 的 `IDXGraphicsAnalysis::BeginCapture/EndCapture`，捕获前与结束前等待队列完成，
不需要 Present 或额外 WinPixEventRuntime 依赖；未由 PIX 注入时启用此开关会明确失败。
仅在此捕获模式下先提交初始化 clear passes 并等待完成，再开始捕获并重新录制初始化 clear passes、原始 resolve pass 与读回。
预热避免捕获起点快照读取尚未初始化的 DS 资源；捕获内的 clear 确保每次回放显式恢复源与目标初值，
不依赖工具恢复 MSAA DS 初始内容，报告记录 `depth_resolve_pix_initialization_submit=warmup_then_captured_clear`。
可同时开启分阶段读回，将结果与 PIX 中的 `DepthResolveTarget1x`、`DepthResolveReadback` 对照。
捕获完成后在 PIX 保存为 `.wpix`，在同一 GPU/驱动下 Start Analysis，查看原始 BeginRenderPass 的 ending access、resolve 参数与目标 stencil。
PIX 的临时捕获文件可能保存在系统临时目录；没有自动打开页面时先检查 `.wpix` 是否已经生成。
如果分析时报告 NVIDIA profiler session 冲突，可在 PIX 设置中启用 Disable GPU Plugins（旧版为关闭 Enable GPU Plugins）后重新打开捕获；
此选项关闭硬件性能插件，API 参数和资源正确性分析仍可使用。命令行对应 `open-capture <file> --disable-gpu-plugins`。
接口与注入要求见 [PIX 程序控制捕获](https://devblogs.microsoft.com/pix/programmatic-capture/)。

```powershell
cmake --build --preset win-x64-debug --target test_radray_render --parallel 8
$env:RADRAY_TEST_REQUIRED_BACKENDS = 'd3d12,vulkan'
$env:RADRAY_TEST_GPU_VALIDATION = '1'
ctest --preset win-x64-debug -R 'AttachmentOperations|TextureViewContract|Vulkan|ExplicitBindingLayoutTest|D3D12Diagnostic|ExplicitLayoutOwnership' --output-on-failure --timeout 60
```

Release 使用对应 `win-x64-release` preset，仍先构建后测试。按上面的 adapter 环境变量逐设备运行；
另在独立构建目录分别配置 `RADRAY_ENABLE_VULKAN=OFF` 和 `RADRAY_ENABLE_D3D12=OFF`，
构建 render 测试并运行仍启用后端的 suites，确认后端专属校验不会引入已关闭后端的链接依赖。
`RadRayDxcMetadata.SpirvMetadataDoesNotRequireGoogleExtensions` 检查优化/调试编译组合的原始
SPIR-V 不声明 GOOGLE 反射扩展且独立 metadata 仍完整；render 的字节码回归不依赖 DXC。
`VulkanShaderBytecode` 检查所有原始 SPIR-V fixture 及对齐/非对齐输入原样提交；
`VulkanGpuTest` 使用无 GOOGLE 扩展的 VS/PS 完成传统 framebuffer 绘制并验证像素读回。

显式更新 fixture 时开启 compiler，然后手动构建并运行生成器：

```powershell
cmake --build --preset win-x64-debug --target radray_shader_fixture_generator
& build/windows-x64-msvc/Debug/_build/Debug/radray_shader_fixture_generator.exe
```

附件与 sampler 的小型 GPU fixture 直接从测试 HLSL 生成原始 DXIL/SPIR-V，显式更新使用：

```powershell
python tools/compile_attachment_fixtures.py
```

可用 `--dxc <path>` 指定已安装 DXC；脚本不下载依赖，普通构建/测试不调用脚本。

生成器目标为 compiler 侧的 `EXCLUDE_FROM_ALL`；普通构建与测试不构建或调用它。
GPU 用例区分后端未编译、无 adapter、缺验证层与初始化失败；可选后端不可用可跳过，
原生资源、PSO、提交或读回错误必须失败。通过 `RADRAY_TEST_REQUIRED_BACKENDS=d3d12,vulkan`
将后端设为必测，`RADRAY_TEST_GPU_VALIDATION=1` 开启 GPU 验证。缺失与跳过必须单列，不能计为通过。
部分 core 测试需要被忽略的 `assets/`，通过源码仓库外渠道准备。

Vulkan 交换链的退休状态机与原生函数表故障测试不要求 GPU；Win32 WSI 用例使用真实设备与
短暂的无激活测试窗口，覆盖连续呈现、resize、可用 present mode、双交换链、创建/部分初始化
失败后重试，以及 RHI 校验包装。WSI 测试启用 Vulkan validation 和 synchronization validation，
不启用 maintenance1。关闭时无验证错误不代表无扩展的呈现完成保证已补足，契约见
[RHI 同步](../architecture/render-rhi.md#同步)。其他平台不注册 Win32 WSI 用例。

```powershell
$env:RADRAY_TEST_REQUIRED_BACKENDS = 'vulkan'
cmake --build --preset win-x64-debug-clangcl --target test_radray_render --parallel 8
ctest --preset win-x64-debug-clangcl -R VulkanSwapChain --output-on-failure --timeout 45
```

双 GPU P0 验收先用 `VulkanDeviceFeatureGpu` 的报告确认索引与设备名称，再分别指定索引运行
Vulkan suites；Debug / Release 各自先构建后测试，不并发构建与测试。例如每个配置构建完成后：

```powershell
$env:RADRAY_TEST_REQUIRED_BACKENDS = 'vulkan'
foreach ($adapterIndex in 0, 1) {
    $env:RADRAY_TEST_VULKAN_ADAPTER_INDEX = "$adapterIndex"
    ctest --preset win-x64-debug-clangcl -R 'Vulkan|ExplicitBindingLayoutTest.*<01-' -E 'ExplicitBindingLayoutTest.*<00-' --output-on-failure --timeout 60
    if ($LASTEXITCODE -ne 0) { break }
}
Remove-Item Env:RADRAY_TEST_VULKAN_ADAPTER_INDEX
```

`tools/run_render_validation.py --help` 提供已构建配置的串行 CTest 与后端证据收集；它不负责构建。
D3D12 descriptor 正确性由 `ExplicitBindingLayoutTest` 与 `DescriptorDirtyRangesD3D12Test` 覆盖。


### D3D12 / DXGI 采集验收

公共 GPU fixture 先创建 `DXGIDiagnosticContext` 并订阅结构化诊断，再将同一上下文传给 Factory，
用唯一应用消息和同步检查点确认 D3D12/DXGI 通路。同一 case 同时创建多个 Factory 时，必须显式
传入同一个上下文；`TryCreateDevice` 的 diagnosticContext 参数接受共享上下文。顺序 case 各自创建并最终释放上下文。
没有后台自检。报告记录 callback/polling、原生 InfoQueue1 查询 HRESULT、native/base-interface-only
选择、callback/checked_clear 策略、完整性、同步检查点、最终收尾、预期匹配及错误/警告数。
`RADRAY_TEST_D3D12_ADAPTER_INDEX` 与 Vulkan 的同名索引变量分别选择各自枚举中的 adapter；
非法或不存在的索引失败，不回退。

`D3D12DiagnosticsTest` 覆盖自然选择、强制基础接口、最后一步错误、fixture 清理阶段错误、
重复检查点、结构化原生 allocator Reset 错误。`D3D12DiagnosticsSmoke` 覆盖 DXGI 两个 producer、
容量违约、全部队列/slot、重复 Reset、共享通道、再次创建及 swapchain 重建，也检查 debug Factory
缺少显式上下文时拒绝创建，以及 Factory 先释放后 Device 仍能采集、最终上下文无残留引用。
`D3D12DiagnosticsReaders` 使用真实小容量队列与测试侧 COM spy，覆盖完整复制、读取失败、
复制期间变化、复制后清空前插入、清空后新消息、溢出、外部清空和多 Factory 聚合；另以活动上下文与
未建立原生通道的空上下文验证检查点、observer 和完整性故障不会跨上下文传播。
callback 回归检查立即分发日志/probe、零存储队列调用、注册失败不降级与在途注销。
`diagnostic_callback_runtime=native/modeled` 区分原生接口与旧系统上的测试模型；模型通过不能
替代支持 InfoQueue1 的实际运行时验收。`D3D12DiagnosticCounters` 检查异常计数，
`D3D12DiagnosticFinalizer` 检查未预期错误、故障和区间外消息会产生 GTest 失败。
实际资源销毁消息由 `ExplicitLayoutOwnershipD3D12` 覆盖。

预期错误/故障在测试侧按来源、producer、ID、severity、数量、消息区间以及故障类型/HRESULT
精确匹配，直接运行 GTest 也会断言；`evidence_class` 与预期计数只用于报告归类，不能代替匹配。
`run_render_validation.py` 要求必测后端实际执行 validation-enabled 用例；缺失 GTest/CTest 证据、
崩溃、超时、自检/收尾/匹配失败、模式不一致、非预期错误或采集故障都失败。预期采集故障允许
对应检查点为 failed，但最终匹配必须通过。warning 和可选 skip 分列，不默认为零错误。
算法证据使用 `diagnostic_algorithm`，不依赖测试名称特判。
每次使用空输出目录，保存 metadata、GTest XML、CTest 日志和 summary.json：

```powershell
python tools/run_render_validation.py --self-test
cmake --build --preset win-x64-debug-clangcl --target test_radray_render --parallel 8
$env:RADRAY_TEST_D3D12_ADAPTER_INDEX = '0'
$env:RADRAY_TEST_VULKAN_ADAPTER_INDEX = '0'
python tools/run_render_validation.py --build-dir build/windows-x64-clangcl/Debug --config Debug --output-dir build/validation/debug-gpu0 --required-backends d3d12,vulkan --regex 'D3D12Diagnostic|Vulkan|ExplicitBindingLayoutTest|ExplicitLayoutOwnership|DescriptorDirtyRangesD3D12Test'
Remove-Item Env:RADRAY_TEST_D3D12_ADAPTER_INDEX, Env:RADRAY_TEST_VULKAN_ADAPTER_INDEX
```

对 NVIDIA / Intel 分别选择索引执行；Release 使用对应 preset、配置及独立报告目录，
始终先构建再测试。另选 `D3D12DiagnosticsGbv.AsyncErrorIsDeliveredBeforeFinalization` 和单个 dispatch/readback 用例，
加 `--gpu-validation` 做小范围 GBV；前者验证 GPU 异步状态错误在最终收尾前送达，默认运行跳过。
不要将 GBV 用于性能比较。用独立构建目录分别设置 `RADRAY_ENABLE_VULKAN=OFF`、
`RADRAY_ENABLE_D3D12=OFF`，构建 render 测试并运行剩余后端回归；缺失后端不列为必测。
fixture/报告改动也必须运行受影响的 Vulkan validation / synchronization validation 回归。

**Win10 验收是独立必需项，不能用本机 Win11 结果代替。** 在真实 Win10 安装 Graphics Tools，
记录 OS caption/version/build/UBR、GPU 驱动和系统 D3D12/D3D12Core/SDKLayers/dxgidebug 文件版本
（不存在的拆分 runtime 文件明确记录 not_present）。不要求挂 debugger：
必须观察原生 InfoQueue1 查询 `E_NOINTERFACE`，正常创建自动选择 polling，
同步自检、allocator Reset 与 DXGI 原生错误被收到，复制清空累计计数、多 Factory 和共享通道生命周期测试通过。
Debug/Release 都先构建后运行；NVIDIA/Intel 分别保留实际可用设备的证据，硬件缺失明确列为未覆盖。
在上述完整回归命令后加 `--require-win10-legacy`，任一环境或证据缺失使报告失败。
Windows 11 上强制 base-interface-only 只是算法/路径测试；默认报告的
`win10_legacy_acceptance` 会保持 pending 并列出缺失证据，不能据此宣布 Win10 验收完成。
若 Win10 实测 ClearStoredMessages 不保留累计 accepted 语义，应报告不支持/采集不完整，
不得静默跳过对账或当作零错误通过。

采集契约与 callback 覆盖边界见 [RHI 诊断通道](../architecture/render-rhi.md#d3d12--dxgi-诊断通道)。

## 编译数据库与文档检查

启用 `RADRAY_BUILD_COMPILE_COMMANDS` 时，Ninja/Makefile 配置开启
`CMAKE_EXPORT_COMPILE_COMMANDS`。配置完成后，调用独立目标生成
IDE 默认读取的 `.vscode/compile_commands.json`，不编译 C++：

```powershell
cmake --build --preset win-x64-debug --target radray_compile_commands
```

该目标仅在开关开启的 Ninja/Makefile 配置中创建，此时才要求 Python >= 3.10。它调用展开脚本，为 unity 中的原始 `.cpp` 生成独立条目，
普通条目原样保留；原始构建数据库不被覆盖。

VSCode 使用 CMake Tools 的 `postConfigureTask` 在每次成功配置后运行
`cmake/UpdateCompileCommands.cmake`；它读取构建目录的 generator，仅为 Ninja/Makefile 调用该目标。
Visual Studio/MSBuild 直接跳过。工作区关闭
`copyCompileCommands` / `mergedCompileCommands`，避免插件再用原始数据库覆盖展开结果。
本地设置和 task 示例见[开发环境](dev-env.md#编译数据库后处理)。
这一步发生在 CMake generate 完成后；`execute_process` 或 `cmake_language(DEFER)` 仍处于
configure 阶段，不能用来处理本次尚未生成的数据库。命令行 configure 后仍需显式运行上述目标。

也可单独调用脚本：

```powershell
python tools/expand_unity_compile_commands.py build/windows-x64-msvc/Debug
```

输入可为构建目录或数据库文件；默认输出 `.vscode/compile_commands.json`，可用 `-o <path>` 改写。
支持 `command` 和 `arguments`，
展开条目统一输出 `arguments`，普通条目原样保留。命令字符串默认按运行平台解码，
可用 `--command-style windows|posix` 指定。展开条目去掉共享输出与 MSVC 二进制 PCH 参数，
保留 `/FI` 等文本强制 include；不改变真实构建。

脚本只处理 CMake 普通 C/C++ `Unity/unity_*.cxx` / `unity_*.c` 文件；相对 include 按 unity 文件目录
和编译命令的 include 目录查找。自定义 unity 代码、宏注入、重复 include 或响应文件会报错，
不会输出不完整的结果。原始源文件需要能独立解析，脚本不重现同批前序 `.cpp` 引入的声明或宏。
新增源码或重新 configure 后需更新数据库；VSCode 的配置完成 task 自动执行，其他环境运行上述目标或脚本。

本项目的 clang-cl 预设使用 Visual Studio/MSBuild，继续单独使用已有仓库脚本求值项目，
不接入上述自动展开链路，不编译源码：

```powershell
python tools/win_gen_compile_commands.py --build-dir build/windows-x64-clangcl/Debug --configuration Debug
```

该脚本默认输出 `.vscode/compile_commands.json`。
IDE 设置见 [开发环境](dev-env.md)。
文档或技能修改运行 `python tools/check_docs.py` 与 `git diff --check`，详见[文档维护](documentation.md)。
