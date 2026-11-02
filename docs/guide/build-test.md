> - 适用: 恢复依赖、配置与构建 RadRay、运行测试或生成编译数据库
> - 权威: 本文说明仓库构建操作；选项与预设以锚点中的 CMake 文件为准
> - 锚点: `CMakeLists.txt`, `CMakePresets.json`, `cmake/Utility.cmake`, `cmake/Dependencies.cmake`, `cmake/RuntimeLibrary.cmake`, `cmake/UpdateCompileCommands.cmake`, `cmake/dependencies/libjpeg.cmake`, `project_manifest.json`, `tools/build_third_party.py`, `cmake/Install.cmake`, `cmake/DependencyCatalog.json`, `tools/fetch_third_party.py`, `tools/fetch_sdks.py`, `tools/win_gen_compile_commands.py`, `tools/expand_unity_compile_commands.py`, `modules/*/tests/CMakeLists.txt`, `modules/core/tests/test_memory_override.cpp`, `tools/compile_test_shaders.py`, `tools/compile_attachment_fixtures.py`, `cmake/TestShaders.cmake`

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
python tools/compile_test_shaders.py --build-dir build/windows-x64-msvc/Debug --config Debug
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
python tools/compile_test_shaders.py --build-dir build/windows-x64-clangcl/Debug --config Debug
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

需要编译的包默认静态；Windows 动态 CRT 三元组的 mimalloc 同时准备动态库与静态库。保留
spdlog 外部 fmt 与 Windows 宽字符、与三元组一致的 GTest CRT、Tracy on-demand
等必要配置。优先使用上游安装规则；IJG libjpeg 与 stdexec 使用 `cmake/dependencies/` 的离线适配，
不修改源码、不下载额外工具。libjpeg 适配器保留 `FindJPEG` 识别 Debug 所需的 `d` 后缀，
其他库不统一追加后缀。DXC 是独立 SDK 包，准备脚本不构建或验证它。

仓库 preset 设置 `RADRAY_DEPENDENCY_TRIPLET`，主工程根据当前配置把各库安装目录加入包查找路径，
并在 compiler 开启时将仓库 DXC SDK 作为查找提示。显式 `CMAKE_PREFIX_PATH`、包 `_DIR` 和
`RADRAY_DXC_ROOT` 仍可使用；不设置三元组时，由外部消费者自行提供依赖路径。包缺失时配置失败，
不回退源码构建。准备脚本成功只表示所选包安装完成，依赖集成由主工程构建和功能测试验证。

使用其他包时需自行保证 CRT、架构和编译选项兼容；Windows spdlog 必须以宽字符支持编译。

### Windows 动态 CRT 与 mimalloc

Windows `/MD[d]` 下，启用 `RADRAY_ENABLE_MIMALLOC` 时 core 链接上游动态库 target `mimalloc`，
并通过源码 target 和安装导出传递该依赖；`CMAKE_MSVC_RUNTIME_LIBRARY` 未设置时按 CMake 的
动态 CRT 默认值选择。显式空值不能确定 CRT ABI，需要指定非空 CRT 值。复杂或混合静态/动态 CRT
的生成器表达式要求依赖包同时提供 `mimalloc` 和 `mimalloc-static`，链接时按配置选择对应 target。
静态 CRT 与非 Windows 平台仍使用下节的静态链接路径，mimalloc 开关不修改 CRT 设置。

依赖准备脚本对动态 CRT 三元组开启 `MI_BUILD_SHARED`、`MI_OVERRIDE` 和 `MI_WIN_REDIRECT`。
上游构建 `mimalloc.dll`（Debug 为 `mimalloc-debug.dll`），并将自带的 x64 `mimalloc-redirect.dll`
复制、安装到该 SDK 包的 `bin/`；该准备阶段不向应用输出目录部署运行库。已有的纯静态 mimalloc
包需要按更新后的配方重新准备，例如：

```powershell
python tools/build_third_party.py build --triplet windows-x64-clangcl --config Debug --only mimalloc --clean
```

RadRay 主工程只发现并链接依赖，不复制或安装这两个外部 DLL，不修改启动 `PATH`，不添加强制
符号引用、初始化调用或导入表修补来保证 DLL 加载。最终程序负责 DLL 搜索路径与部署、实际导入
以及尽早加载 mimalloc DLL。动态覆盖只有在 DLL 成功加载且 redirect 生效后才会接管经动态 CRT
发起的分配；不能仅凭链接声明断言进程已覆盖。宿主可通过 `mi_is_redirected()` 或
`MIMALLOC_VERBOSE=1` 检查状态，并验证目标分配的归属。见
[mimalloc 3.5.3 Windows 动态覆盖说明](https://github.com/microsoft/mimalloc/blob/v3.5.3/readme.md#dynamic-override-on-windows)。

开发者运行需要 mimalloc DLL 的程序或构建期 GTest discovery 前，可显式设置当前启动环境：

```powershell
$env:PATH = "$(Resolve-Path ./SDKs/mimalloc/windows-x64-clangcl/Debug/bin);$env:PATH"
```

Release 或其他编译器应使用对应 SDK 前缀；外部宿主自行管理其运行环境。

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

`RADRAY_ENABLE_MIMALLOC=ON` 且使用静态链接路径时，core 将 `mimalloc-static` 以 `LINK_LIBRARY:WHOLE_ARCHIVE`
传递到最终 exe/DLL。在 `/MT` 下，上游标准 C 分配函数和 C++ `new/delete` 替换会参与最终链接；
同一映像内静态依赖的标准分配调用通过这些符号进入 mimalloc。
core 按链接依赖提供 override 的约定消费 mimalloc，不包含 `mimalloc-new-delete.h` 或额外定义
全局 `new/delete`。Windows `/MD[d]` 使用前节的动态库接入，不依赖这组静态覆盖符号。
RadRay 的 `memory.cpp` 分配接口及启用的 Vulkan 分配回调直接调用 `mi_*`，不依赖 CRT 符号覆盖。
普通及对齐分配的配对约定见 [Core 基础设施](../architecture/core-facilities.md#容器别名)。

静态覆盖只针对最终映像内这些标准分配入口；系统/驱动内部、GPU 显存和直接使用系统内存 API 的
分配不在覆盖范围。对象和内存由分配它的模块配对释放，不跨 DLL 使用另一套 CRT 的 `free/delete`。
Windows `/MT` 下 mimalloc 默认以 C++ 编译并自带 `new/delete`，宿主不得再包含
`mimalloc-new-delete.h` 或另定义同名替换。

`test_inline_vector` 自己替换 `new/delete` 以计数，只编译容器测试与现有 `logger.cpp`，链接
fmt、spdlog 和 GTest，不链接 core 的 mimalloc 依赖。其余 core 测试仍链接完整 core；静态 CRT
且启用 mimalloc 时加入 `MimallocOverride` suite，检查标准 C 分配及 realloc 数据保留、RadRay
分配、对齐分配、C++ 对象/数组和标准容器的 mimalloc 归属。构建完成后可单独执行：

```powershell
ctest --preset win-x64-release-staticcrt -R MimallocOverride
```

`MemoryTest` 在所有配置下验证 RadRay 普通/对齐分配的数据、对齐与配对释放；启用 mimalloc 时
额外使用 `mi_any_heap_contains` 检查归属，包括 `/MD[d]` 构建，不以全局 CRT 覆盖为前提。

`MemoryTest` 只验证 RadRay 分配接口；静态 CRT 和最终 DLL 的标准分配覆盖仍需使用
`MimallocOverride` 及目标映像分别验证。

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
| `RADRAY_BUILD_RUNTIME` | ON；要求 render |
| `RADRAY_ENABLE_D3D12` | Windows 且 render 开启时默认 ON |
| `RADRAY_ENABLE_VULKAN` | render 开启时默认 ON |
| `RADRAY_BUILD_SHADER` | ON；独立 shader contract/decoder |
| `RADRAY_BUILD_SHADER_COMPILER` | ON；要求 shader |
| `RADRAY_BUILD_SHADER_TOOLS` | 独立构建 ON；子工程 OFF；要求 shader compiler |
| `RADRAY_ENABLE_PROFILER` | 所有配置默认 ON；链接 Tracy client，见[性能采样](performance.md#性能采样tracy) |

验证不带编译器的基础模块时使用独立目录。先按[本地测试 shader](#本地测试-shader)在 compiler 开启的配置中生成夹具；以下配置共享默认夹具目录，不查找或加载 DXC：

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
覆盖；普通变量与缓存设置均受支持。mimalloc 的链接依赖和 Tracy 会传递到最终程序；静态路径
保留 mimalloc override 的 whole-archive，Windows 动态 CRT 路径使用 `mimalloc` 动态库 target。
宿主不需要时应显式关闭 `RADRAY_ENABLE_MIMALLOC` / `RADRAY_ENABLE_PROFILER`；动态库运行环境
由宿主管理，见[Windows 动态 CRT 与 mimalloc](#windows-动态-crt-与-mimalloc)。

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
`RadRay::runtime`、`RadRay::shadercompiler`，以及 CLI 的 `RadRay::shader_compile`。源码构建提供相同别名，
旧的实际 target 名称保留。组件为 `core`、`shader`、`window`、`render`、`runtime`、`shadercompiler`、
`tools`；模块依赖自动加载，不指定组件时加载全部已安装组件。`RadRay_AVAILABLE_COMPONENTS`
按实际生成 target 列出安装能力。render 组件只加载 core 和启用后端的外部包，无需 shader 或 DXC。
runtime 组件传递加载 render/core；公共类型直接位于 `radray`，头文件位于 `<radray/runtime/...>`。
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
| `test_radray_core` | `RuntimeTypeIdTest`、`ManualCoroutineScheduler`、`MemoryTest`、`ObjectArenaTest`、`ArenaAllocatorTest` |
| `test_radray_render` | `ExplicitBindingLayoutTest`、`ExplicitLayoutOwnershipD3D12`、`ExplicitLayoutOwnershipVulkan`、`DescriptorDirtyRangesD3D12Test` |
| `test_radray_render`（D3D12 诊断） | `D3D12DiagnosticsTest`、`D3D12DiagnosticsSmoke` |
| `test_radray_render`（Vulkan） | `VulkanDeviceFeaturePolicy`、`VulkanDeviceFeatureConsumers`、`VulkanDeviceFeatureGpu`、`VulkanSwapChainRetirementTest`、`VulkanSwapChainNativeTest`；Win32 另含 `VulkanSwapChainWsiTest` |
| `test_radray_window`（Win32） | 用 `ctest -N` 查看当前注册用例 |
| `test_radray_shader` | `RadRayShaderDiagnostics`、`RadRayShaderArtifact`、`RadRayShaderContract`（不依赖 compiler） |
| `test_radray_shader_compiler`（compiler 开启） | `RadRayShaderCompilerClient`、`RadRayShaderLibPass` |

ObjectArena 与 STL allocator 的独立验证入口如下，不需要 GPU 或外部资产。完整 core 回归还包含
使用 assets 的测试；内存复用基准见[性能测量](performance.md#object-arena)。

```powershell
cmake --build --preset win-x64-debug-clangcl --target test_radray_core --parallel 8
ctest --preset win-x64-debug-clangcl -R '^(ObjectArenaTest|ArenaAllocatorTest)\.' --output-on-failure --timeout 30
```

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
render 测试读取 `RADRAY_TEST_SHADER_DIR/shader_bytecode/` 中的本地预生成字节码，运行时无需 shader/compiler/DXC；准备步骤见[本地测试 shader](#本地测试-shader)。
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

### 本地测试 shader

Git 只保存测试 HLSL、生成代码和 metadata/GPU hash 的文本断言，不保存生成的 `.bin`。
统一入口 `tools/compile_test_shaders.py` 先增量构建 compiler 侧的
`radray_shader_fixture_generator`（`EXCLUDE_FROM_ALL`），生成 metadata 与 raw stage 字节码，
再调用 `compile_attachment_fixtures.py` 的 DXC 配方生成附件、sampler、command 和 interop 变体。
每次准备完整的 DXIL + SPIR-V 夹具，单后端测试也使用同一套输出；metadata 的 golden hash 不自动更新。

干净检出先恢复并准备依赖、配置 compiler 开启且 tests 开启的构建，再运行脚本，最后构建和运行测试：

```powershell
cmake --preset win-x64-debug-clangcl
python tools/compile_test_shaders.py --build-dir build/windows-x64-clangcl/Debug --config Debug
cmake --build --preset win-x64-debug-clangcl --parallel 8
ctest --preset win-x64-debug-clangcl --output-on-failure
```

需要 Python >= 3.10、CMake、该 build 目录对应的 C++ 工具链和匹配的 **RadRay DXC fork** 包。
默认 DXC 是 `SDKs/radray_dxc/extracted/bin/dxc.exe`，也可传 `--dxc <fork-package>/bin/dxc.exe`；
该目录须有匹配的 `dxcompiler.dll`、`dxil.dll`。CMake 找到的 fork headers/package 也必须匹配。
stock DXC 不能生成 RadRay metadata。脚本只为子进程设置 DLL 搜索路径，不下载 SDK 或第三方依赖；
缺包、compiler 关闭、配置不匹配、DXC 编译失败都会停止并显示原因，不将缺夹具的测试标成跳过。

默认输出为仓库的 `build/test_shaders/`，含 `shader_artifacts/`、`shader_bytecode/` 和
`fixtures.sha256`。它们属于被忽略的本地构建产物，不应加入 Git；旧源码下的两个生成目录也加入
`.gitignore`，防止旧命令重新产生的 bin 被误提交。真实 HLSL、TSV 及其他测试数据仍在源码目录。
不同 Debug/Release、单/双后端与 compiler-disabled 配置可共享该完整目录，不并发运行生成脚本。

脚本每次请求 CMake 增量构建生成器；输入源码、include、配方、生成器及 DXC 内容都不变且产物完整时
跳过 shader 编译。输入变化、bin 丢失/损坏或 `--force` 会重新生成；两类生成全部成功后才替换夹具，
清理该输出目录内已不需要的 bin，最后写校验清单。新内容与旧内容相同时保留 bin 时间戳。

```powershell
python tools/compile_test_shaders.py --check
python tools/compile_test_shaders.py --build-dir build/windows-x64-clangcl/Debug --config Debug --force
```

shader/render 测试及 IPC worker 的构建依赖 `radray_check_test_shaders`，只检查源指纹、清单与文件
SHA-256，不启动 DXC。缺失或过期时构建失败并提示上面的生成命令；configure 允许夹具尚未存在，
因此可以先构建生成器。直接运行已有 exe/CTest 不会触发构建检查，修改输入或搬运产物后先运行 `--check`。

关闭 compiler 时保留原有边界：库、测试和 IPC worker 均不增加 DXC 链接或运行时依赖；
首次准备必须使用另一个 compiler 开启、tests 开启的配置，之后再配置/构建 compiler-disabled 消费者。
纯库构建（`RADRAY_BUILD_TESTS=OFF`）不要求这些夹具或 Python 检查器。
目前 compiler client 只在 Windows 支持生成；其他平台可从同版本源码的 Windows 准备环境复制完整目录
并运行 `--check`，不把 Windows 的测试结果当作其他平台验证。

自定义目录需让生成脚本与所有消费配置一致：

```powershell
python tools/compile_test_shaders.py --build-dir build/windows-x64-clangcl/Debug --output-dir build/custom_test_shaders
cmake --preset win-x64-debug-clangcl -B build/no_compiler -DRADRAY_BUILD_SHADER_COMPILER=OFF -DRADRAY_BUILD_SHADER_TOOLS=OFF -DRADRAY_TEST_SHADER_DIR="$PWD/build/custom_test_shaders"
cmake --build build/no_compiler --config Debug --parallel 8
ctest --test-dir build/no_compiler -C Debug --output-on-failure
```

`RADRAY_TEST_SHADER_DIR` 只影响测试数据路径，不改变源码根或产品安装内容。
底层 `compile_attachment_fixtures.py --output-dir <raw-directory>` 仅供维护局部 DXC 配方，不能替代完整
准备步骤，也不更新完整清单；使用它后应再运行统一入口。

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


### Windows 资源互操作验收

`ExternalResourceConfigurationTest` 覆盖双后端默认关闭、显式开启、重复查询与 RHI validation 包装行为，
以及关闭时的普通资源创建和 Vulkan Win32 external 能力关闭状态。下述互操作 fixture 与 IPC worker
在 device descriptor 中显式开启 `EnableExternalResource`。

`test_radray_render` 的 `ExternalResourceTest` 覆盖运行时能力失败、错误消息及原生诊断、设备身份兼容性、独立句柄的
移动/关闭，以及非对齐 Buffer 和共享纹理范围内各格式 `257×131` 纹理的重复双向读写。copy、sampled、storage、render-target
分别创建并执行，检查帧号、每个像素及边界；同时验证 GENERAL→GENERAL ownership 转移和第一次导入内容。
BGRA8 的四种 usage 使用独立用例，检查内存 B/G/R/A 顺序与采样 R/G/B/A 值；storage 实际执行 shader 写入，
Vulkan 侧要求已启用 `shaderStorageImageWriteWithoutFormat`。资源能力或该可选 feature 缺失时明确跳过成功路径；
资源创建不支持仍由失败用例断言。
`ExternalResourceTextureTest` 将 route、描述和用途拆为独立 case：七种维度、1/3/5 mip、非二次幂尺寸、12 层 CubeArray、奇数深度 3D；
小尺寸二维数组遍历全部颜色格式的 copy/sample/storage/RT，深度组覆盖四种 DS 格式及两个 aspect、多 mip/层，MSAA 查询 2/4/8/16 samples。
检查创建/导出/导入描述，关闭句柄后执行四次双向往返，核对所有 texel 和最后子资源；各端分别计算复制布局，D3D12 使用真实 plane footprint。
Storage 逐子资源写入并逐范围回到 Common 后整资源 Release；DS 使用部分 scissor 与混合 DSV 权限，核对另一 aspect 和未写区域。
MSAA 逐 sample 读取；UNORM 附件允许一个量化单位。格式策略组创建 sRGB SRV/RTV，使用中间亮度图案验证 sRGB 采样解码，并检查后端特有兼容格式。
该组 raw copy 逐字节比较；sRGB 解码误差在编码域内按半个 8-bit code value 判断。
这是 [Direct3D 功能规范 v1.17 §3.2.3.7](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm) 的精度判据；
Vulkan 侧目前沿用它作为项目验收阈值，不能声称它是 Vulkan 的规范保证。
Vulkan 1.2 的 [Texel Input Format Conversion](https://github.com/KhronosGroup/Vulkan-Docs/blob/v1.2.198/chapters/textures.txt)
规定 EOTF、alpha 不变，但没有该半码值条款；其数值章节的一般浮点精度也不能直接推导固定功能 EOTF 的这一误差界。
typed RGBA8/BGRA8 拒绝 sRGB view；D3D12 原生 DENY_SHADER_RESOURCE 用例检查附件导入成功而 sampled 导入失败。
DS 另有不声明附件用途的纯复制用例。MSAA 的 LocalControl 使用同一个原对象完成往返，供区分本地命令和导入对象问题。
新纹理组使用独立 RHI device；同后端 Vulkan 共用 instance，D3D12 共用显式诊断上下文。
D3D12 在同进程、同适配器上复用原生 device（测试记录 COM identity）；独立原生 device 的验收依赖 IPC，不能用两个 RHI wrapper 代替。
`ExternalResourceTextureMapping` 无需 GPU 即可检查原生描述与完整 ownership range，不能替代 GPU 内容验收。
查询不支持只跳过该 case；查询支持后，创建、导入、提交、view 或读回错误都必须失败。
每条 Fence 只由一端 signal，导入方的 `GetLastSignaledValue()` 不随远端 signal 改变。
`ExternalResourceIpcTest` 由父进程监督两个独立 GPU worker，覆盖单 slot、三 slot、延迟消费者、四个 generation（RGBA8 2D、RGBA16 多 mip 数组、BGRA8 CubeArray、R32 奇数深度 3D）与 resize、
旧 generation 拒绝、正常退出与消费者异常退出；阻塞的生产端由监督进程终止，不复用其在途资源。

```powershell
cmake --build --preset win-x64-debug --target test_radray_render --parallel 8
ctest --preset win-x64-debug -R ExternalResource --output-on-failure --timeout 90
```

两端 native validation 和 RHI validation 默认开启；可用 `RADRAY_TEST_GPU_VALIDATION=1` 额外开启 GPU-assisted 检查。
route 参数 0/1/2/3 分别是 D3D12 句柄的 DX→DX、DX→VK、VK→DX、VK→VK，参数 4 是显式选择 opaque 的 VK→VK。
原有同进程同后端 route 使用同一 device 上的独立导入对象；opaque route 使用两个独立 device，
共用由源 fixture 管理的 instance，销毁两个 device 后才关闭 instance；该 instance 收集两端的 native validation。
`ExternalResourceOpaqueTest` 另外验证 UUID 与本端设备的兼容性、实际 memory requirements 对内存类型及分配大小的限制、
输入句柄借用和失败边界单次 error。测试不再向扩展提交违反描述、对象归属/存活、导出资格或 barrier 前置条件的调用，
也不要求这些误用返回失败；外部资源契约检查已移除。支持路径仍以两端 native validation 和数据读回验证交接正确性。
IPC 测试覆盖独立 device / 进程。helper 是独立可执行文件，GTest 用例仍在现有 render 测试目标。
也可在构建输出目录直接执行：

```powershell
.\radray_external_worker.exe d3d12 vulkan 3 normal
.\radray_external_worker.exe d3d12 vulkan 3 interrupt
.\radray_external_worker.exe vulkan vulkan 3 normal opaque-win32
.\radray_external_worker.exe vulkan vulkan 3 interrupt opaque-win32
```

末尾模式参数可选，省略或传入 `d3d12-resource` 使用原路径；`opaque-win32` 要求两个后端均为 Vulkan。
返回 0 表示通过，77 表示设备/能力缺失（对应成功路径未覆盖），78 表示下述明确已知问题隔离，1 表示失败。
查询 NativeApi/NativeCode 表示的非 FORMAT_NOT_SUPPORTED 原生错误返回 1；双方 hello 中一方失败、另一方返回 77 时优先返回 1。
IPC 在 generation 不支持、已知问题隔离或失败时停止本 case，后续 generation 属于未执行，不能由聚合 skip/失败推断它们的能力或内容结果。
已知问题返回 78 前正常停止两端 worker 并检查退出；清理或退出失败仍返回 1。
worker 使用本地 named pipe、版本 5 逐字段小端编码和 `DuplicateHandle`，拒绝旧协议；
设备身份仍为 LUID / NodeMask；helper 的 hello 另传 backend、vendor/device ID 与原始 driver version，用于精确限定测试隔离范围，获取失败通过响应状态报告。
资源导出描述不携带版本字段，协议版本仅由 helper 管理。
导出描述按类型解码和编码转发，包含完整纹理描述、格式重解释策略、句柄类型与可选 Vulkan 元数据，不使用固定字段数定位句柄；不传 C++ struct 内存布局或源进程裸句柄值给目标使用。
当前工作目录的 `interop-worker-<supervisor pid>-<0|1>.log` 保存两端 native diagnostics。
父进程对 IPC 设 30 秒上限，并以 kill-on-close job object 管理 worker；GPU 等待超时不会被当作资源完成。

#### 已知 GPU 问题的测试隔离

默认测试在实际能力查询成功后，按 [known_gpu_issues.cpp](../../modules/render/tests/known_gpu_issues.cpp) 的精确描述跳过已复现的内容错误。
来源必须 CanCreateExportable、接收端必须 CanImport；原生查询异常先失败，能力不支持保留原 unsupported skip。
这只改变测试选择，不修改生产 ExternalSupport、资源参数、usage、同步和内容判据。所有根因仍未确认。
每次命中打印 `KNOWN_ISSUE_SKIP`，XML 写入 `known_issue_skip_count` 和 `known_issue_skips`；统计时与能力不支持及缺后端分开，不能记作通过。
普通纹理参数化 case 在 SetUp 查询后整体隔离，因此命中 case 的创建、导入、view 和内容阶段本轮均未执行；这些阶段需用复现开关检查，不能用本轮 skip 替代历史证据。

环境只匹配 NVIDIA GeForce RTX 4080 的 PCI vendor/device `10de:2704` 与 NVIDIA 621.01：
D3D12 使用 DXGI `32.0.16.2101`（原始值 `0x0020000000100835`），Vulkan 使用 `driverVersion=0x9b404000`。
DXGI 版本来自 [CheckInterfaceSupport(IDXGIDevice)](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiadapter-checkinterfacesupport) 的 UMD 版本，
Vulkan 使用 [VkPhysicalDeviceProperties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceProperties.html) 的 vendorID/deviceID 及供应商编码的 driverVersion；不把它当作 apiVersion 解析。
跨 API 两端都必须匹配，仍通过现有 LUID/node 检查确保同一物理设备；MSAA 两端都是 D3D12，外部句柄都为 D3D12_RESOURCE。
本机 Intel、其他 NVIDIA 型号、未知/其他驱动、opaque 句柄、反向 route 和 LocalControl 不命中。
已验证环境为 ksgfk-big 的默认 implicit layers 和关闭 implicit layers 两组；runtime/驱动根因未分离。
当前规则以型号和 driver version 为匹配边界，进行 OS/runtime/层版本对照时必须使用下面的复现开关。

设置 `RADRAY_TEST_RUN_KNOWN_GPU_ISSUES=1` 可恢复原始执行。变量只作用于当前进程及其 IPC 子进程；取消设置即恢复隔离。
例如在已构建的 Debug 目录执行：

```powershell
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
$env:RADRAY_TEST_RUN_KNOWN_GPU_ISSUES = '1'
& ./build/windows-x64-clangcl/Debug/_build/Debug/test_radray_render.exe '--gtest_filter=ExternalResourceNativeProbe.*:*Shape_2DArray_RGBA8_UNORM_M3_U2_Route1:*Msaa_2D_RGBA8_UNORM_S2_Route0'
Remove-Item Env:RADRAY_TEST_RUN_KNOWN_GPU_ISSUES
```

copy-only D32+S8 的复现需显式选择 `*DepthCopyOnly_D32_FLOAT_S8_UINT_Route1`，历史会 DeviceLost；默认保留隔离，不在常规复现命令中包含它。
`KnownGpuIssueContract` 用独立 [已观测 case 清单](../../modules/render/tests/data/known_gpu_issue_cases.tsv) 校验 188+20+1 个纹理条目，
并检查相邻尺寸/mip/layer/sample、用途、格式策略、句柄、未知设备/驱动以及查询错误不误命中；IPC/native 的额外描述和 resolve 模式单独校验。
此清单是测试输入，208 个已完成纹理失败加 1 个历史崩溃，不代表 209 个独立根因。

以下 C 表示 CopySource|CopyDestination；所有外部描述均为 Device memory、dedicated，未列出的字段组合不推断为同一已知问题。

| 已知问题及历史数量 | 精确形状、格式策略和用途 |
|---|---|
| DX→VK 部分 mip 数组：8 个 shape | 1DArray=17×1/3层、2DArray=17×9/3层、Cube=17×17/6层、CubeArray=17×17/12层；全部3 mip、1×；RGBA8_UNORM或RGBA16_FLOAT；typed；C|UnorderedAccess。 |
| 同上：166 个 color format/use | 2DArray，9×5、2层、2 mip、1×、typed；下表42个颜色格式分别为 C、C|Resource、C|UnorderedAccess、C|RenderTarget。两个 sRGB storage 组合由能力查询否定，仍属于 unsupported，不在166项内。 |
| 同上：12 个 depth/use | 2DArray，9×5、2层、3 mip、1×；D16_UNORM、D32_FLOAT、D24_UNORM_S8_UINT、D32_FLOAT_S8_UINT；C|DepthStencilWrite 为typed，增加Resource时开启重解释，附件测试为 C|DepthStencilRead|DepthStencilWrite 且typed。 |
| 同上：2 个 reinterpret | 2DArray，9×5、2层、3 mip、1×；RGBA8_UNORM或BGRA8_UNORM；开启重解释；C|Resource|RenderTarget|UnorderedAccess。以上四组共188项。 |
| D3D12→D3D12 shared MSAA：20 项 | 8×8、1 mip，2D/1层或2DArray/2层，2×或8×；RGBA8_UNORM、RGBA16_FLOAT、R32_UINT使用Resource|RenderTarget且typed；D32_FLOAT、D24_UNORM_S8_UINT使用Resource|DepthStencilRead|DepthStencilWrite并开启重解释。4×、16×及LocalControl不在规则内。 |
| DX→VK copy-only D32+S8：额外1个崩溃 | 2D，9×5、1层、3 mip、1×，D32_FLOAT_S8_UINT，typed，C。不能把它与188个数组内容错误合并为同一已确认原因。 |
| DX→VK IPC：2 个 case | SingleSlot/1、ThreeSlotsResizeDelayedConsumer/1 的generation 1：RGBA16_FLOAT、2DArray、35×19、3层、3 mip、1×、typed、C。generation 0先正常执行；XML记录完成代数、隔离代数及后续未执行代数，后续CubeArray/3D不被推断为已知失败或通过。 |
| D3D12 native MSAA 与 IPC MSAA 诊断 | 仅RGBA8_UNORM、2D、8×8、1层、1 mip、2×/8×、typed。native用RenderTarget；IPC generation 0用C|Resource|RenderTarget。未观察到的后续RGBA16/array generation不扩大匹配。 |
| 非共享 depth Average resolve | D3D12，D32_FLOAT_S8_UINT，2D、8×8、1层、1 mip、4×源和1×目标，DepthStencilRead|DepthStencilWrite|Resource，区域(2,2,4,4)，仅depth=Average/stencil=None。其余10个受支持模式继续执行。 |

上表166项覆盖的42个颜色格式固定如下；未来新增格式不会自动进入隔离集合：

| 格式族 | 格式 |
|---|---|
| R8 / RG8 | R8_SINT、R8_UINT、R8_SNORM、R8_UNORM；RG8_SINT、RG8_UINT、RG8_SNORM、RG8_UNORM |
| R16 / RG16 | R16_SINT、R16_UINT、R16_SNORM、R16_UNORM、R16_FLOAT；RG16_SINT、RG16_UINT、RG16_SNORM、RG16_UNORM、RG16_FLOAT |
| R32 / RG32 | R32_SINT、R32_UINT、R32_FLOAT；RG32_SINT、RG32_UINT、RG32_FLOAT |
| RGBA8 / BGRA8 | RGBA8_SINT、RGBA8_UINT、RGBA8_SNORM、RGBA8_UNORM、RGBA8_UNORM_SRGB；BGRA8_UNORM、BGRA8_UNORM_SRGB |
| packed | RGB10A2_UINT、RGB10A2_UNORM、RG11B10_FLOAT |
| RGBA16 / RGBA32 | RGBA16_SINT、RGBA16_UINT、RGBA16_SNORM、RGBA16_UNORM、RGBA16_FLOAT；RGBA32_SINT、RGBA32_UINT、RGBA32_FLOAT |

原生数组探针的9×5、2层、2 mip、RGBA8、C|Resource已在166项内；1/4 mip控制继续执行。
原生 MSAA 的4×、两种barrier模式继续执行。聚合 probe/resolve 只有在其余组合无失败后才将整个 GTest case标为已知问题skip，
并保存 `executed_mip_combinations`、`executed_sample_barrier_combinations`、`executed_resolve_modes`，不将部分覆盖报告为完整通过。
能力查询、环境读取、后续操作或清理产生错误时仍失败，不能由后来的已知问题skip覆盖。

规范与根因证据边界见 [RHI 互操作规范边界](../architecture/render-rhi.md#互操作规范边界) 及下文的原生实验说明。
尤其不能由这些隔离条目宣称 COMMON↔GENERAL 完整跨 API 映射已获权威确认，也不能据此省略独立 native stencil 复现或 runtime/驱动版本对照。

#### 当前隔离配置的验证

2026-10-29，ksgfk-big，Windows x64 / ClangCL，RTX 4080 / 621.01；关闭全部 implicit layers，保留显式 native/RHI validation。
下表为当前代码重新构建后的结果；每批串行执行，并检查 XML 用例数量，未执行项不计入通过。

| 配置与范围 | 通过 | 已知问题 skip | 能力不足 skip | 不适用/缺后端 skip | 失败 |
|---|---:|---:|---:|---:|---:|
| Debug，完整 ExternalResource + KnownGpuIssueContract，1465 项 | 649 | 213 | 602 | 1 | 0 |
| Release，同上，1465 项 | 649 | 213 | 602 | 1 | 0 |
| Debug / Release，各自的视图、附件、命令、framebuffer 与 device feature 回归，113 项 | 107 | 1 | 1 | 4 | 0 |
| Release，IPC MSAA 2×/4×/8×，3 项 | 1 | 2 | 0 | 0 | 0 |
| Release，Intel UHD 770 / 32.0.101.7092 对照，7 项 | 6 | 0 | 1 | 0 | 0 |
| Release，GPU validation 重点纹理，5 项 | 4 | 1 | 0 | 0 | 0 |
| D3D12-only Debug，CPU/query/native/纹理/resolve 定点，14 项 | 7 | 3 | 0 | 4 | 0 |
| Vulkan-only Debug，同上，14 项 | 8 | 0 | 0 | 6 | 0 |

完整矩阵逐项对照隔离前索引：原 641 个通过项全部继续通过，原 210 个失败项全部变为明确的已知问题 skip；
额外 3 个已知 skip 是 copy-only D32+S8 历史崩溃与两个 native probe。213 不是修复数量，也不是成功路径数量。
能力不足的 602 项保持原分类；另一项为 D3D12 原生契约在其他 route 上不适用。
回归的 4 个条件 skip 是设备所有受支持采样格式均支持 min/max、无法构造不支持格式；另 1 个能力 skip 是 Intel 只有一条可用队列。
Intel 的数组 native probe 返回 FORMAT_NOT_SUPPORTED；其 MSAA native/RHI 与旧 resolve 对照通过，不命中 NVIDIA 隔离规则。

聚合覆盖继续显式记录：数组 native probe 执行 1/4 mip 两项，跳过 2 mip；MSAA native probe 执行 4× 的两种 barrier 模式，跳过 2×/8× 的四项；
旧 resolve 执行其余 10 个模式，只跳过 Average/None。两个 DX→VK IPC case 完成 generation 0，在 generation 1 隔离，后两代未执行。
默认矩阵没有重新触发 copy-only D32+S8 DeviceLost。

开启 `RADRAY_TEST_RUN_KNOWN_GPU_ISSUES=1` 的 Release 复现组为 **1 通过、5 原始内容失败、0 已知 skip**：
两个 native probe、一个部分 mip 数组、一个导入 MSAA 和旧 depth Average resolve 失败，MSAA LocalControl 通过。
独立 IPC 2× 复现另有 **1 原始失败、0 已知 skip**。这验证了复现入口，不能将预期复现失败重新记为通过。

完整矩阵没有 native/RHI error 或诊断采集失败；每个配置的 native MSAA 对照有 4 条预期 D3D12 ID 820 性能 warning。
启用复现和 Intel native MSAA 对照各有 12 条同类 warning。GPU validation 另有一条同时启用 core 与 GPU-assisted 的性能设置 warning；
Loader 的 forced-disabled layer 提示单独保留，不当作内容错误。所有这些 warning 均不构成根因结论。

双后端 Debug/Release 和两个单后端 Debug 均构建通过；双后端 Release、两个单后端 Debug 分别安装到新的独立前缀，外部 CMake 消费工程配置、编译和运行通过。
消费工程只包含公共 external_resource 头并链接 RadRay::render，限定对应的 CMAKE_CONFIGURATION_TYPES；不准备或下载依赖。
文档检查与 diff whitespace 检查通过。本地 `build/external-texture-known-issues/` 保存分批 XML、worker 日志、逐项索引和复现输出；不新增开发历史。
本轮未完成 runtime/驱动版本对照、独立 native stencil 复现、跨 API 完整布局对应的权威核对，以及缺失导出方向的硬件内容验收。

#### 隔离前的内容错误与规范调查

以下失败计数来自引入已知问题隔离前的运行；复现开关恢复这些原始路径。引入 skip 不改变内容正确性尚未验收的结论。
调查机器为 RTX 4080 / NVIDIA 621.01。Vulkan 的 D3D12_RESOURCE 与 timeline D3D12_FENCE 仍仅含 IMPORTABLE，
VK→DX、D3D12 句柄 VK→VK 的成功路径属于硬件待验收；OPAQUE_WIN32 memory 和 timeline semaphore 支持双向。
DX→VK 的部分 mip 链数组存在查询支持但内容不一致的失败；DX→DX 的 2×/8× MSAA 导入对象往返也有内容错误，
相同 MSAA 描述的 LocalControl 通过。DX→VK 的 D32_FLOAT_S8_UINT 纯复制用例触发 DeviceLost，后续批次明确排除重跑，
不能把该项列为成功或能力 skip。GPU validation 重点用例没有新增原生错误，但内容错误仍存在；其同时启用 core validation 的性能 warning 单列。
Intel UHD 770 / 32.0.101.7092 对照中，2×/8× RGBA8 MSAA 导入往返通过。
此前 Intel 数组跨后端失败受 `VK_LAYER_OBS_HOOK` 干扰：本机 graphics-hook64.dll 1.8.7.0 加载时查询报告 IMPORTABLE，
仅关闭该层后精确描述返回 `VK_ERROR_FORMAT_NOT_SUPPORTED`。分别关闭 NV_present、NV_optimus 不能消除该差异。
因此不能把此前 Intel 的内容错误归为 Intel 原生驱动缺陷，也不能把关闭 OBS 后的 skip 算作成功路径。
原生 API 记录确认了失败数组用例的完整创建描述、dedicated/offset 0 和整资源 ownership range，不能以参数映射测试替代内容验收。
旧 RTX 3060 / 591.44 记录不代表本次机器已验证。

定位互操作故障时，应另开进程并用 `VK_LOADER_LAYERS_DISABLE=~implicit~` 对照，同时保留显式 validation。
Loader Layer Filtering 需要以 Vulkan headers 1.3.234 或更新版本构建的 loader；必须从 loader 日志确认实际禁用生效。
该变量只用于测试进程，不修改系统层注册或生产设备配置。必须重新查询并记录能力，不能沿用带 hook 的查询结果。
`ExternalResourceNativeProbe` 提供两条原生对照：数组探针直接创建 D3D12 committed texture，逐 mip/layer 原生复制写入，
手动调用 Vulkan external image/allocate/bind、整资源 acquire 和紧凑读回，绕过 RHI 描述转换、纹理复制及 VMA image 分配；
MSAA 探针直接 shared handle/open、RTV clear、resolve 和读回，无 shader 或 RHI render pass；
分别运行独立 ExecuteCommandLists、独立提交再加 `{nullptr, nullptr}` aliasing barrier 两种模式。
两种模式在 NVIDIA 上均为 2×/8×失败、4×通过，Intel 均通过；该额外 barrier 仅为诊断对照，不是 committed resource 的新增生产要求。
带 committed resource 指针的 aliasing barrier 被原生验证拒绝（ID 528），属于无效实验，已从探针移除，不能作为 GPU 内容证据。
RTX 4080 在关闭全部隐式层后，前者单 mip/完整 4 mip 链通过、部分 2 mip 链第二层为零；后者 4×通过、2×/8×在导入对象写入后失败。
相同 MSAA 原生程序在 Intel 上通过；IPC 单 slot 的 2×/8×失败、4×通过，排除了同进程 device 复用这一解释。
IPC MSAA 诊断入口为进程环境变量 `RADRAY_TEST_EXTERNAL_IPC_MSAA=2|4|8`，使用 RGBA8/RGBA16、2D/2DArray 四代资源，XML 记录实际 samples；不计入默认复制矩阵。
这些结果将问题定位到原生互操作路径，仍需驱动版本对照或厂商确认才能确定内部原因，不能由原生 validation 无错误直接推断驱动有错。

D32_FLOAT_S8_UINT 的拆分诊断中，生产端本地预检通过；关闭隐式层后 depth-only 四轮往返通过，stencil-only 首次导入读取就 DeviceLost，未执行消费者写回。
保持尺寸、mip 和复制操作，显式增加 DepthStencilWrite 用途后两个 aspect 四轮往返通过。
这支持检查原生 DS 创建模式与 stencil 布局兼容性的假设，尚未证明必须增加用途；生产映射未自动增加该用途，原始纯复制用例仍未验收。
上述缩减 aspect/只读/增加用途的临时探针已移出正式用例，源码快照和结果保存在本地 `build/external-texture-investigation-probes/` 与 `build/external-texture-investigation-*.log/.xml`。

完整矩阵的验收结果（Windows x64 / ClangCL，RTX 4080 / 621.01；默认隐式层环境，不含后续两个 native probe）：

| 配置与范围 | 通过 | 失败 | skip |
|---|---:|---:|---:|
| Debug，ExternalResource，主矩阵与补测按 case 最新结果合并 | 641 | 210 | 603 |
| Release，ExternalResource，主矩阵及格式策略复测 | 641 | 210 | 603 |
| Debug / Release，各自的视图、附件、命令和设备 feature 回归 | 107 | 1 | 5 |
| Debug，D3D12-only 的本后端 route | 284 | 20 | 22 |
| Debug，Vulkan-only 的 opaque route | 274 | 0 | 13 |
| Release，GPU validation 重点纹理用例 | 5 | 2 | 0 |

Debug 的纯复制 D32+S8 DeviceLost 属于额外一个崩溃用例，不在表中已完成用例计数内；Release 没有重跑它。
关闭隐式层的后续 Debug/Release 定点验收各执行 5 个用例，3 个常规控制通过、两个 native probe 失败；未重新执行完整矩阵。
此前单模式 MSAA 探针各报告 6 条 ID 820 未提供 optimized clear value 的性能 warning；当前双模式为 12 条，原生错误为 0；数组探针错误/警告均为 0。
主矩阵的 210 项失败包含 188 项部分 mip 数组、20 项 D3D12 MSAA 和 2 项受数组问题影响的 IPC。
Release XML 逐项审计确认，208 个纹理内容失败的来源 CanCreateExportable 和接收端 CanImport 均为 true，
没有同一 case 同时含 failure 与 skip。603 个 skip 分为：512 个来源 Vulkan 无 D3D12_RESOURCE 导出能力、
60 个请求 16× MSAA 不支持、10 个 sRGB storage 不支持、20 个旧资源/fence/IPC route 聚合不支持、1 个后端不适用。
旧完整矩阵没有记录全部原生查询数值，不能将后来的 native 数值回填为当时观测。
常规回归的失败是 D3D12 `DepthStencilResolveModesPreserveUntouchedAspectAndOutsideArea`；保留的 2026-10-23 Release 程序也能复现，
该原有 resolve 路径本次未修改。分阶段读回确认初始化 stencil=19，depth-only Average resolve 后矩形内变为 0，矩形外保持 19；
本地独立原生 `depth_resolve_repro` 在 NVIDIA 复现、Intel 通过，无 native error/warning，排除了本次共享纹理改动及测试采样 shader。
已有[原生复现报告](https://forums.developer.nvidia.com/t/d3d12-rtx-4080-depth-only-average-resolve-clears-destination-stencil-driver-32-0-16-2101/386685)，当前未见厂商确认；不据此猜测压缩或写掩码内部机制。
sRGB 原先固定线性域 1e-4 不能用于要求所有 D3D 实现通过；目前阈值与 Direct3D 规则一致。
此前将同一依据用于断定 Vulkan 的偏差也必然合法，证据不足；Vulkan 当前结果只表示通过项目阈值，raw copy 仍逐字节校验。
普通互操作运行的原生错误、警告与诊断采集失败计数为零，不能因此忽略内容错误。
双后端 Release 和两个单后端 Debug 均完成安装及外部 CMake 消费检查。单后端测试不计入被排除的另一后端 route。

规范审计的当前分类（规则与官方链接见 [RHI 规范边界](../architecture/render-rhi.md#互操作规范边界)）：

| 分类 | 结论与仍需的证据 |
|---|---|
| 规范明确要求，已满足已审项目 | 完整形状、mip/layer/sample 查询、external handle 链、dedicated/offset 0、memory type、NT handle 生命周期、完整 ownership range、release/fence/acquire、导入不从 Undefined 丢弃。原生数组探针也执行上述步骤，读回有 transfer→host 屏障、等待和 invalidate。此列表不是全路径合规认证。 |
| 规范明确要求，原实现有遗漏，已修正 | Vulkan 附件尺寸还受 framebuffer 上限约束；external 查询已补检查并有颜色/深度、宽/高测试。数组原生探针现在区分 FORMAT_NOT_SUPPORTED 与其他查询错误，并核对返回 limits，其他错误不再 skip。此修正不解释小尺寸数组或 copy-only DS 失败。 |
| 观察到平台相关行为，根因未确认 | D3D12 shared MSAA 2×/8×、旧 depth-only Average resolve 在 NVIDIA 的原生程序失败、Intel 通过。共同的查询、状态、提交、plane 与采样模式约束已核对，尚未分离 OS runtime 和驱动版本。不能只靠无 validation error 或原生复现宣布驱动缺陷。 |
| 跨 API 规范或实验依据仍不足 | 部分 mip 数组和 copy-only D32+S8 的跨 API 布局对应未得到独立权威确认；后者尚缺完全绕过 RHI 的 stencil-only 原生程序。增加附件用途通过只是改变创建模式的对照，不是合法性证明或可静默采用的修复。 |
| 测试判据需区分 API | D3D sRGB 使用规范半码值；Vulkan 使用同一项目阈值，完整规范精度验收仍待核对 Vulkan 对应 CTS/厂商依据。不能将 OpenCL 或 D3D 精度条款移植为 Vulkan 要求。 |
| 环境影响已确认，层内部原因未确认 | 仅关闭本机 OBS 层就改变 Intel 的精确 external 查询结果；符合 layer 可拦截调用这一机制，但尚未定位 OBS 1.8.7.0 的具体拦截代码或驱动交互，不能推断当前 OBS 上游均有同一问题。 |

规范审计使用 Vulkan 1.2.198 作为历史基线，并与当前官方 refpage 对照；实际设备仍以项目 Vulkan 1.2 请求运行，
需要驱动提供 Win32 external memory/semaphore 扩展及启用 timelineSemaphore。NVIDIA 报告的物理设备 API 为 1.4.357，
本机验证层来自 Vulkan SDK 1.4.363.0；没有因此启用 synchronization2 或其他新版 feature。
本机 D3D12Core.dll/d3d12.dll 文件版本为 10.0.26100.9549，项目没有导出 Agility SDK 选择符；文件版本不替代捕获中实际加载模块的证据。
旧 resolve 原生程序检查 OPTIONS2 的 ProgrammableSamplePositionsTier、4× quality、单采样目标、默认 sample positions 和 depth plane 0。
[D3D12_RESOLVE_MODE](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resolve_mode) 允许 depth Average、禁止 stencil Average；
[Programmable Sample Positions v0.5](https://microsoft.github.io/DirectX-Specs/d3d/ProgrammableSamplePositions.html) 还要求 clear/resolve/transition 的 sample positions 一致。
因此仅 depth resolve 不应把未指定的 stencil plane 当作输出；已观察到的矩形内 stencil 清零仍是失败，外部区域保持不变。

下一步应保留相同描述与相同二进制，分别做 runtime/驱动版本对照；为 copy-only DS 补独立原生 stencil 复现，
将 D3D12 desc、Vulkan create info、handle properties、query limits、barrier 与 fence 完整交给厂商核对跨 API 对应。
OBS 对照必须记录实际加载层、单独 disable OBS 和全部 implicit disable 三组重新查询结果，不能复用另一组能力。
Loader 的过滤机制见 [Khronos Loader Layer Interface](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md)。
这些动作完成前，已知失败按前述精确条件隔离；开启复现时保留原始失败判据。缺失的导出方向继续列为硬件待验收。

隔离前规范审计的定点回归（关闭全部 implicit layers，保留显式 validation；不替代上面的完整矩阵）：

| 配置/检查 | 结果 |
|---|---|
| Debug、Release，各 16 个用例 | 各 13 通过、2 原生复现失败、1 不适用 skip；覆盖新限制、native probe、映射、MSAA4、本地对照、opaque 格式策略、配置与句柄兼容性 |
| Release，GPU validation，3 个用例 | 2 通过、1 原生 MSAA 复现失败；无新增原生 error，性能 warning 单列 |
| D3D12-only Debug，6 个用例 | 3 通过、1 原生 MSAA 失败、2 缺 Vulkan skip |
| Vulkan-only Debug，5 个用例 | 3 通过、2 缺 D3D12 skip |
| Intel 原生对照 | MSAA 的两种 barrier 模式均通过；跨 API 数组查询不支持，记 skip |
| 保留的独立 depth resolve 程序，D32，两个 GPU × none/average/min/max | 7 通过；仅 NVIDIA average 失败，16 个矩形内 stencil 值改变，native error/warning 均为 0 |

双后端 Debug/Release 与两个单后端 Debug 构建通过；文档检查与 diff whitespace 检查通过。
该次规范审计没有重跑完整 1455 个用例、重新安装消费或重新触发 D32+S8 DeviceLost；这些状态沿用前述明确范围，不能计作新通过。
本地审计证据为 `build/external-texture-spec-audit/` 内的 XML/log 与规范来源快照，不属于受版本控制的开发历史。

隔离前的查询与 skip 判定复核（ksgfk-big，2026-10-29 12:35 起，完整矩阵之后；内容定点关闭 implicit layers）：

- `QueryEveryTextureCaseWithoutCreatingResources` 查询生成器的全部 268 个描述，NVIDIA 的 D3D12、Vulkan D3D12_RESOURCE、Vulkan opaque 共 804 次；没有查询异常。
  每组 256 个可用描述、10 个不支持的 16× 描述、2 个 sRGB storage 描述。Vulkan D3D12 句柄的 256 个描述只支持导入，不能运行反向导出 route。
  这是能力查询覆盖，不创建或执行纹理，不能替代 GPU 内容验收，也不等于全部合法 RHI 描述的穷举。
- Intel 默认层、仅禁 OBS、禁全部 implicit 三组，各 536 次查询，无查询异常。后两组的全部 268 个 D3D12_RESOURCE 描述返回 FORMAT_NOT_SUPPORTED；
  默认层有 256 个可导入描述。错误返回时不解释输出结构的其他字段；opaque 不受此变化影响。
- Debug 定点 33 项：16 通过、8 失败、9 skip；Release 定点和视图/设备 feature 回归 55 项：49 通过、5 失败、1 skip。
  两组失败均为已有 native probe、部分 mip 数组、MSAA 2×/8×，Debug 另含两个 IPC 数组失败；Release 的 skip 是 Intel 多队列硬件限制。
- 独立进程重新运行两个 IPC 数组 case、MSAA 2×/4×/8×：数组两项和 2×/8×失败，4×通过。数组失败在 generation 1 的 RGBA16_FLOAT、35×19、3 层、3 mip；后续 generation 没有执行。
  没有以降低参数、增加附件 usage 或改变内容判据绕过错误。
- 完整字节日志修正后 Debug 的 9 项针对性检查为 7 通过、2 个已有数组失败；两个单后端各 4 项为 2 通过、2 缺后端 skip。
  旧 depth Average resolve 的当前 Release RHI 用例重新运行：NVIDIA 失败、Intel 通过；与前述独立原生程序对照一致。
  双后端 Debug/Release 和两个单后端 Debug 构建、文档检查及 diff whitespace 检查通过。
- D3D12 查询现已保留失败 HRESULT；公共纹理测试、旧用例和 IPC 在 skip 前检查查询错误；opaque 同时检查两端能力。
  旧聚合纹理循环对单个不支持组合继续查询其余组合，XML 的 completed_combinations / unsupported_combinations 单列覆盖；独立参数矩阵仍是主要验收入口。
  CPU contract 覆盖错误分类，但未向真实驱动注入内存耗尽或 CheckFeatureSupport 故障。复制失败日志改为打印全部比较字节，内容比较未改变。

本地 `build/external-texture-capability-audit/` 的 `historical-release-index.json` / `historical-failed.csv` / `historical-skipped.csv`
提供全部 1454 项索引及每个失败/skip 的原 XML、时间、日志行、方向和 RHI 查询结果；`current-queries.json` 独立保存当前原生返回值。
该次审计的代码基线为 `7cf4f1f` 加当时未提交工作树，源码 SHA-256 在本地审计目录；旧完整矩阵为默认隐式层环境，该次定点不是一次完整重验收。
该次审计没有重新执行 copy-only D32+S8 DeviceLost、完整内容矩阵或安装消费；未具备的导出方向继续单列，旧 depth Average 与 Vulkan sRGB 精度的证据边界保持上述结论。

D3D12 句柄的导出缺失是一项运行时验收限制，不能为此新增 D3D12 分配桥接或替代路径。换 GPU/驱动后重新查询并执行相同用例。
测试 shader 的源文件在 `modules/render/tests/data/interop_*.hlsl`，通过[本地测试 shader](#本地测试-shader)的统一脚本同时生成 DXIL / SPIR-V fixture；配置/普通测试构建不调用 DXC。

Release 使用 `win-x64-release` 先构建后测试。单后端分别以独立目录配置
`-DRADRAY_ENABLE_VULKAN=OFF` / `-DRADRAY_ENABLE_D3D12=OFF`，构建并运行 ExternalResource suites；
依赖缺失的 route 会跳过。安装消费按本节前面的安装流程，外部工程包含
`<radray/render/external_resource.h>` 并仅链接 `RadRay::render`，检查公共头不暴露原生 SDK 类型。
启用 Tracy 时，直接在同一进程运行完整纹理矩阵可能耗尽 GPU context 编号；使用上述逐 case CTest，
或将 GTest filter 分为小批次串行运行。不能把进程异常、缺失 XML 或未执行的用例计作通过。

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

## Runtime 构建

`RADRAY_BUILD_RUNTIME=ON` 要求 render 开启，提供 `RadRay::runtime` / `radrayruntime`。
当前模块包含 Environment、DeviceContext、材质核心、Scene 绘制输入和 Mesh Drawing Pipeline 基础接口，公共入口为
`<radray/runtime/gpu_env.h>`、`<radray/runtime/gpu_device.h>` 和 `<radray/runtime/scene.h>`。
Scene 头包含材质、Primitive、MeshBatch、VertexInput 与 Processor 的类型；
资源分段描述 `MeshSection` 与 MeshBatch 同在 `<radray/runtime/mesh_batch.h>`。绘制入口为
`<radray/runtime/mesh_pass_processor.h>`（Processor 与输出 Context）与 `<radray/runtime/mesh_draw_command.h>`。
材质资源、定义、实例与渲染快照均在 `<radray/runtime/material.h>`。
各头文件声明的类型见 [Runtime 头文件布局](../architecture/runtime.md#头文件布局)。
多槽帧循环入口为 `<radray/runtime/frame_scheduler.h>`，阶段接口与槽位分别在
`frame_pipeline.h`、`frame_slot.h`；配置与生命周期见 [多槽帧循环](../architecture/runtime.md#多槽帧循环)。

在已按前文准备依赖和工具链的环境中构建：

```powershell
cmake --preset win-x64-debug -DRADRAY_BUILD_RUNTIME=ON
cmake --build --preset win-x64-debug --target radrayruntime --parallel 8
```

安装消费继续使用 `find_package(RadRay CONFIG REQUIRED COMPONENTS runtime)` 和 `RadRay::runtime`，
传递加载 render/core，不要求 shader 或 DXC。`RADRAY_BUILD_TESTS=ON` 时提供
`test_radray_runtime`，验证材质标签登记/查询/复制、参数类型、默认值/覆盖、显式快照同步与生命周期，
以及命令缓存的增量重建、句柄稳定性、静态/动态合并、参数绑定与录制预检。以下 suite 不要求 GPU 或预编译 shader：

```powershell
cmake --build --preset win-x64-debug-clangcl --target test_radray_runtime --parallel 8
ctest --preset win-x64-debug-clangcl -R '^(MaterialSystem|MaterialTags|MeshDrawingPipeline)\.' --output-on-failure
```

同一可执行文件中的 `FrameLoop` suite 使用已编译的 D3D12/Vulkan 后端和 validation，
以每槽独立的 buffer 拷贝/读回验证 1/2/3 槽轮转、跨槽帧编号、可写数据隔离、
多 command buffer 的池化复用、批量与分次提交，以及外部 Scene 的借用边界、WaitIdle 和析构顺序。
同时覆盖同步提交加空操作 Wait 的无显式 fence 实现、可写 Scene 自动增量同步、const Scene 入口与未修改帧的命令复用。
上传缓冲由槽内 ObjectArena 持有，覆盖帧内分配隔离、完成后逆序清理、WaitIdle 幂等与析构顺序。
共享 Buffer 在 Extract / Record 中交给 `FrameSlot::RetireResource`，验证完整轮转后释放，
以及 WaitIdle 先重置全部 allocator 再释放帧内和退役 owner。退役队列另覆盖连续登记、逆序析构、
未使用槽、WaitIdle 后重新登记、Scheduler 析构，以及回调之外登记时终止进程；帧执行不提供失败恢复路径。
不需要 shader，后端初始化失败视为测试失败：

```powershell
ctest --preset win-x64-debug-clangcl -R FrameLoop --output-on-failure
```

Release 使用同名 `win-x64-release-clangcl` 预设。样例的双后端运行覆盖真实 GPU 路径；
后端回归入口见[测试](#测试)，模块接口见 [Rendering runtime](../architecture/runtime.md)。
旧场景、绘制和 RDG 的测试命令及既有验证声明保存在[设计归档](../history/2026-10-31-runtime-design-archive.md)，
只适用于该归档基线。

## Mesh Drawing Pipeline 教学样例

`examples/mesh_drawing_pipeline/` 是独立可执行程序，复用 runtime 的 Environment、DeviceContext 和
Scene 输入数据、材质定义/实例/Proxy 快照、命令数据与 Processor/Context 接口。Depth/Color/Outline/Transparent Processor、相机、材质参数的 GPU 适配、PSO/资源准备和
Pass 调度和可见 Primitive/Batch 的选择属于样例；
Scene 增量缓存、基类 Processor 构建 helper、静态/动态输出 Context、两级 MeshDrawCommand 以及统一校验/录制位于 runtime。
样例通过 Scene 变更接口组织输入，用 SubmitMeshDrawCommands 录制带参数的绘制。
它默认通过 `FrameScheduler` 双槽连续离屏绘制六帧，完成后保存最后一帧的 PNG。五个 Primitive
共享顶点/索引缓冲，各含两个 Section 对应的 Batch，每个 Batch 含一个 Element。
四个 Pass 都收到相同的十个候选 Batch，各自的 Processor 根据样例 `BatchPassPolicy` 和材质标签筛选：

| 对象 | Section 规则 / 材质标签 | Depth | Color | Outline | Transparent | 图片中的表现 |
|---|---|---:|---:|---:|---:|---|
| A：正常物体 | 两个 `DepthAndColor`；`Surface=Opaque` | 2 | 2 | 0 | 0 | 左侧暖色方块、蓝色顶面 |
| B：深度遮挡体 | 两个 `DepthOnly`；`Surface=Opaque` | 2 | 0 | 0 | 0 | 自身不上色，在 A 前方遮出一块背景色缺口 |
| C：只参加颜色 | 两个 `ColorOnly`；`Surface=Opaque` | 0 | 2 | 0 | 0 | 中间绿色方块；跳过预深度仍能正常绘制 |
| D：Section 规则不同且描边 | 侧面 `DepthAndColor`、顶面 `ColorOnly`；`Surface=Opaque`、`Outline=Enabled` | 1 | 2 | 2 | 0 | 右侧紫色方块、粉色顶面和金色轮廓 |
| E：透明叠加 | 两个 `DepthAndColor`；`Surface=Transparent` | 0 | 0 | 0 | 2 | 前方青色透明方块，可透过它看到绿色物体 |

Depth 接受五条、Color 六条、Outline 两条、Transparent 两条，总计 **十五条单实例 Draw**。
E 的 Batch 允许深度和颜色绘制，但 Depth/Color Processor 因材质标签不匹配而拒绝它；
Outline 还会拒绝没有描边标签的 A/C，Transparent 拒绝不透明材质。
左侧缺口是 B 的深度挡住 A、而 B 不写颜色的结果，不是透明材质或 shader discard。
配置表在 `sample.cpp` 的 `cubes` 中，索引 0 为侧面和底面 Section，索引 1 为顶面 Section。
可直接调整 `SectionPasses`，观察 Processor 的接受/跳过日志及图片变化。

材质不再使用独立颜色查找表。`Material` 声明 `BaseColor(float4)` 与 `Brightness(float)` 默认值，
八个 `MaterialInstance` 覆盖颜色，Scene 通过 `MaterialRenderProxy::Update` 接收参数快照。
A/B/C 使用背面剔除不透明材质，D 使用关闭剔除并写模板的不透明材质，E 使用背面剔除的透明材质。
三份 Resource 共享加载的 shader，共准备六个 PSO：普通材质 Depth/Color、描边材质 Depth/Color/Outline、透明材质 Color。
透明材质的 Color 程序/PSO 由 Transparent Processor 使用；程序名不等于 Pass 参与声明。
应用持有 MaterialTagTable；Processor 通过预先登记的 ID 对筛选材质，再结合 BatchPassPolicy 决定参与。
缺少或不匹配的标签会跳过；表身份不同则报错，避免将两个表的相同编号误当作相同语义。
样例还先把绿色实例的 Brightness 同步为 0.5，再改为 1.0：日志先显示实例为 1、Proxy 仍为 0.5，
显式 Update 后 Proxy 才变为 1。Frame 0 提交后执行 Scene::UpdateMaterial，Frame 1 的 `RunFrame(Scene&)` 自动同步，日志验证仅绿色材质关联的一个 Batch 重建，其余九个 Batch 的 CommandHandle 不变，
且该命令自持有的颜色常量 RGB 从 0.5 倍更新为 1.0 倍、alpha 不变。
可调整参数覆盖观察颜色；调整 Tags / RenderState 需要在 Resource 发布和 PSO 创建之前进行。
四个 Pass 按以下顺序执行；每个 Pass 仍自行决定采用或覆盖材质状态：

1. **Depth**：只收不透明材质，沿用剔除和深度偏移，覆盖为 Less、深度测试/写入开启、模板关闭。
2. **Color**：只收不透明材质，沿用材质状态。D 使用 Always/Replace、参考值 7，只在通过深度测试的像素写模板。
3. **Outline**：只收不透明且有描边标签的材质。加载先前结果，用局部放大 1.12 倍的金色几何、关闭剔除、NotEqual 7、模板写掩码 0、深度 LessEqual 且不写深度。
   放大后的正面能通过深度测试，模板负责排除原物体内部；否则金色会盖住紫色。Processor 将固定金色写入命令，Pass UBO 提供放大倍率，不修改 Scene/材质。
4. **Transparent**：只收透明材质，加载已有颜色。沿用材质的 alpha 混合、LessEqual 深度测试、关闭深度写入和模板。
   RGB 为 `src.rgb * src.a + dst.rgb * (1 - src.a)`；alpha 为 `src.a + dst.a * (1 - src.a)`。
   E 的侧面 alpha 为 0.38，顶面为 0.48；alpha 变小本身不会自动开启混合。

描边采用放大几何，粗细随投影变化，共享参考值表示选中物体的并集；不是屏幕空间等宽描边。
透明示例只包含一个凸物体，剔除背面后只画可见表面；多透明物体需要从后往前组织，交叉表面还需要更完整的方案。
完整字段和生命周期见 [材质与 Pass 的边界](../architecture/runtime.md#材质与-pass-的边界)。
Mask、折射、透明排序、OIT、纹理绑定和静态 shader 变体尚未实现。

建议按下面顺序阅读，场景与材质准备保留中文步骤注释，帧循环的职责和生命周期见下文：

| 文件 | 阅读重点 |
|---|---|
| [sample.h](../../examples/mesh_drawing_pipeline/sample.h) / [sample.cpp](../../examples/mesh_drawing_pipeline/sample.cpp) | 准备共享资源与场景，创建 Scheduler，连续 RunFrame，帧间更新材质并验证局部重建，WaitIdle 后保存图片 |
| [material.h](../../modules/runtime/include/radray/runtime/material.h) / [material.cpp](../../modules/runtime/src/material.cpp) | shader/布局借用、材质性质、材质默认值、单层实例继承、类型不变的参数覆盖、Scene 拥有的参数快照 |
| [pipeline.h](../../examples/mesh_drawing_pipeline/pipeline.h) / [pipeline.cpp](../../examples/mesh_drawing_pipeline/pipeline.cpp) | Processor 筛选并复制命令颜色；MeshDrawingFramePipeline 实现 Pass 登记、Extract/Record/Wait，使用槽内 Pass 列表与命令池 |
| [mesh_pass_processor.h](../../modules/runtime/include/radray/runtime/mesh_pass_processor.h) / [mesh_pass_processor.cpp](../../modules/runtime/src/mesh_pass_processor.cpp) | 单个 Batch 的 Processor 接口、可切换输出 Context、共享状态和 Element 构建 helper；AddMeshBatch 返回 void。静态 Context 存入 SparseSet 并回写缓存句柄；动态 Context 自动追加当帧可见命令 |
| [mesh_draw_command.h](../../modules/runtime/include/radray/runtime/mesh_draw_command.h) / [mesh_draw_command.cpp](../../modules/runtime/src/mesh_draw_command.cpp) | 两级命令、自持有 PushConstants、Pass 提交参数与延迟实例流，稳定句柄解析和 SubmitMeshDrawCommands |
| [gpu_support.h](../../examples/mesh_drawing_pipeline/gpu_support.h) / [gpu_support.cpp](../../examples/mesh_drawing_pipeline/gpu_support.cpp) | GpuResources 持有共享只读资源，GpuFrameResources 持有每槽可写资源与 fence；PSO 准备、上传和按行跨度读回 |
| [cube.hlsl](../../examples/mesh_drawing_pipeline/cube.hlsl) | VS 读取 80 字节 Pass UBO 和 64 字节实例矩阵；PS 读取 16 字节命令颜色，Transparent 使用颜色 alpha |

源码对照基线为本地 `F:/cpp/UnrealEngine` 的 UE **5.8.4**（`Engine/Build/Build.version`），
注释中的 UE 路径相对其 `Engine/Source/Runtime/`。对应关系用于解释传统 Mesh Drawing Pipeline 的职责：
本例直接组装 Batch、假定全部候选可见，用 Scene 的持久缓存和脏追踪更新命令，按相同排序键稳定排序；
省略 UE 的多线程代理同步、可见性剔除、LOD、自动实例化、完整材质和 RDG。Color Pass 只是纯色上色，不等价于 UE 完整 Base Pass。
`BasePassRendering.cpp` 的 `FBasePassMeshProcessor::ShouldDraw` 区分不透明和透明类别，
`TranslucentRendering.cpp` 的 `GetTranslucentBlendState` 选择混合状态；本例用应用标签表达类别，状态由材质显式提供。
`CustomDepthRendering.cpp` 的 `FCustomDepthPassMeshProcessor::TryAddMeshBatch` 设置模板状态和参考值，
但 UE 的参与标记/模板值通常来自 PrimitiveSceneProxy。本例使用材质标签和主深度/模板，
几何描边并非对 UE CustomDepth/Stencil + 后处理效果的完整复刻。
材质路径新增 M1～M5 注释，分别对照 UE 的渲染资源、用户定义/实例、渲染侧参数同步、PSO 组装与 shader bindings。
材质职责与所有权见 [Runtime 材质核心](../architecture/runtime.md#材质核心)；本例同步的是 CPU 快照，
没有实现 UE 的 UniformExpressionCache 或稳定 GPU uniform buffer 绑定。
`BatchPassPolicy` 也仅属于样例，通过 Batch 句柄关联记录。其筛选职责对照 UE 的
`FMeshBatch::bUseForDepthPass` / `bUseForMaterial`：`FDepthPassMeshProcessor::AddMeshBatch` 和
`FBasePassMeshProcessor::AddMeshBatch` 分别读取这些标志；UE 还判断材质、遮挡体和其他条件。
候选收集不按 Pass 预先分组，Processor 内的条件才决定是否生成命令。
`MeshDrawingFramePipeline::GetMeshPassRegistrations` 登记四个 Processor，Scheduler 在每次 `RunFrame(Scene&)` 的 Extract 前调用 `Scene::SyncCachedMeshDrawCommands`。
Scene 为脏 Batch 创建静态 Context，Processor 完成 Pass 筛选后调用 runtime 的 `BuildMeshDrawCommands`，
先检查共享输入和全部 Element，再通过 `AddCommand → FinalizeCommand` 输出持久命令并记录句柄。
绿色材质变化后用 `Scene::UpdateMaterial` 标脏，再同步时仅引用它的 Batch 重新进入四个 Processor。
样例显式检查未修改 Batch 的句柄保持不变，修改 Batch 的旧句柄已被新代次替换。

Color/Transparent Processor 在 `AddMeshBatch` 中读取 Proxy 的 BaseColor/Brightness，将 RGB 相乘并保留 alpha，
经 runtime 构建 helper 复制为命令自持有的 16 字节 `MaterialConstants`；Outline Processor 写入固定金色，Depth 不写颜色常量。
命令颜色与 UE 的 `FMeshDrawShaderBindings::LooseData` 职责近似，源栈数据在构建后即可失效。
每个 `GpuFrameResources` 独立创建 Binding 1 的实例缓冲、Group 0 / Binding 0 的两份 Pass UBO 与参数组，
以及颜色/深度附件、读回缓冲和 completion fence。初始化参数组时 Set 后调用 FlushWrites，后续不重写已绑定的描述符。
Extract 在槽完成等待并重置 allocator 后，通过 `UploadSceneParameters` 按候选 Primitive 顺序写入五个 64 字节 `LocalToWorld` 矩阵；
两份 Pass UBO 保存同一 `ViewProjection` 和分别为 1.0 / 1.12 的 `LocalScale`。CBuffer 按设备要求对齐、补零并 FlushMappedRange。
Depth/Color/Transparent 共用普通缩放参数组，Outline 使用放大参数组。
`PreparePassDrawCommands` 只向 `slot.GetPassDrawList(pass)` 收集句柄并把候选 Primitive 下标写入 `FirstInstance`，不访问材质参数。
Record 通过 `slot.GetScene()` 访问源 Scene，使用 `slot.AcquireCommandBuffer()` 申请本槽命令缓冲；
`RecordPass` 设置 viewport/scissor，将本槽 Pass 参数组和实例流通过 `MeshPassSubmitArgs` 交给该 Pass 列表的 Submit。
runtime 先绑定 PSO，再在首次绑定或切换 PSO 后重绑 Pass 参数组，最后完成几何、模板、命令参数和 DrawIndexed；
相机、局部缩放或变换变化不需要重新调用 Processor，材质变化则重建关联命令的颜色副本。
每帧最后记录颜色读回拷贝，提交时 signal 本槽 fence 并返回；复用槽时由 Pipeline.Wait 等待该值。
每槽上传缓冲、附件和读回缓冲均独立；共享几何只在首次提交时从 HostWrite 转为 Vertex/Index。
后续复用沿用本槽颜色 CopySource、深度 DepthWrite 和读回 HostRead 的结束状态，重新写入上传缓冲前先等待并重置 allocator。
主循环只调用 RunFrame，结束时调用 WaitIdle，再从 `(frameCount - 1) % slotCount` 对应的读回缓冲保存图片。
Scheduler 的析构同样等待全部在途槽并重置命令存储，随后才销毁 Pipeline 的每槽资源；外部共享资源和源 Scene 晚于 Scheduler 销毁。
未同步材质、标签表不一致、缺失 PSO、参数类型不匹配或构建失败会使样例放弃渲染；具体校验边界见 [命令校验与录制](../architecture/runtime.md#校验与录制)。

### 准备 shader 与构建

在已准备好 Debug/Release 依赖包的环境中执行。`RADRAY_BUILD_EXAMPLES` 默认 **OFF**；开启时必须
同时开启 `RADRAY_BUILD_RUNTIME`、`RADRAY_BUILD_RENDER` 和 `RADRAY_ENABLE_LIBPNG`（后者依赖 zlib）。
双后端需要 `RADRAY_ENABLE_D3D12=ON`、`RADRAY_ENABLE_VULKAN=ON`，Windows 默认均开启。
样例只链接 `RadRay::runtime` 及其传递依赖，不需要运行时 shader compiler。

先显式预编译 shader，再构建样例：

```powershell
python -X utf8 examples/mesh_drawing_pipeline/compile_shaders.py
cmake --preset win-x64-debug-clangcl -DRADRAY_BUILD_EXAMPLES=ON
cmake --build --preset win-x64-debug-clangcl --target radray_mesh_drawing_sample --parallel 8
cmake --preset win-x64-release-clangcl -DRADRAY_BUILD_EXAMPLES=ON
cmake --build --preset win-x64-release-clangcl --target radray_mesh_drawing_sample --parallel 8
```

[预编译脚本](../../examples/mesh_drawing_pipeline/compile_shaders.py) 默认使用
`SDKs/radray_dxc/extracted/bin/dxc.exe`，可用 `--dxc` 指定本地编译器，`--output-dir` 指定输出目录。
生成 `cube.VSMain.dxil.bin`、`cube.PSMain.dxil.bin` 和对应的两个 `.spirv.bin`，默认位于
`build/mesh_drawing_pipeline/shaders/`。DXIL 使用 SM 6.0；SPIR-V 使用 Vulkan 1.2 target 与 DX 布局。
HLSL 修改后须手动重新执行脚本。脚本、构建和运行均不下载依赖，CMake 不自动编译 shader。

### 运行与核对

```powershell
./build/windows-x64-clangcl/Debug/_build/Debug/radray_mesh_drawing_sample.exe --backend d3d12 --output build/mesh_drawing_pipeline/d3d12-debug.png
./build/windows-x64-clangcl/Debug/_build/Debug/radray_mesh_drawing_sample.exe --backend vulkan --output build/mesh_drawing_pipeline/vulkan-debug.png
./build/windows-x64-clangcl/Release/_build/Release/radray_mesh_drawing_sample.exe --backend d3d12 --output build/mesh_drawing_pipeline/d3d12-release.png
./build/windows-x64-clangcl/Release/_build/Release/radray_mesh_drawing_sample.exe --backend vulkan --output build/mesh_drawing_pipeline/vulkan-release.png
```

参数为 `--backend d3d12|vulkan`、`--shader-dir <目录>`、`--output <文件>`、`--slots <槽数>`、`--frames <帧数>`，`--help` 显示用法。
默认 `--slots 2 --frames 6`；槽数须为正整数，帧数至少为 2，以保留帧间材质更新演示。可以用 `--slots 1`、`--slots 3` 比较轮转；
也可用 `--slots 3 --frames 2` 检查存在未使用槽时的结束清理。
Windows 默认 D3D12，其他平台默认 Vulkan；未指定目录时从编译时仓库根的
`build/mesh_drawing_pipeline/shaders/` 读取 shader，图片写到该仓库根的
`build/mesh_drawing_pipeline/<backend>.png`。显式传入的相对路径以运行时工作目录为准。
所有默认产物都在 Git 忽略的 `build/` 内；样例不进入安装导出。

每次运行请求开启后端验证（Vulkan 同时请求同步验证），实际启用情况和诊断见后端启动日志。
日志应打印五个 Primitive、十个 Batch，以及每帧各 Pass 的 `对象 / Primitive / Batch / Element → Draw`
对应关系；Draw 编号为当前 Pass 输出列表中的序号。跳过日志按 Batch 输出一次，
不分配 Draw 编号，并打印原因。每帧出现 `Frame=<编号> Slot=<槽下标>`、
`Depth=5，Color=6，Outline=2，Transparent=2，总计 15 条单实例 Draw` 和 fence 提交/等待日志；默认槽次序为 0、1、0、1、0、1。
最后打印 PNG 路径及六帧完成日志。同一槽的四个 Pass 使用一份 D32_FLOAT_S8_UINT 附件；Depth Pass 清深度为 1、模板为 0 并保存，
Color Pass `Load` 深度和模板，以材质默认的 `LessEqual` 测试并写深度。
这样既能复用预深度，也能接纳 `ColorOnly` 对象并处理其自身和相互遮挡；如果仍使用 `Equal`，
没有预深度的表面会消失。后续 Outline/Transparent 均 `Load` 颜色、深度和模板。
Pass 之间保留显式 `DepthWrite → DepthWrite`，颜色继续使用时增加 `RenderTarget → RenderTarget` barrier，
Vulkan 用它们同步前一次写入和后一次读写。初始 barrier 还将实例流转为 Vertex，将两份 Pass UBO 转为 CBuffer。
录制顺序为 PSO → Pass 参数组 → 顶点/索引缓冲 → 模板参考值 → 命令参数/颜色常量 → DrawIndexed。
检查 800×600 图片中的三个不透明物体、左侧背景色缺口、右侧物体金色轮廓，以及前方青色物体透出的绿色和背景。
读回逐行去除 padding，日志打印实际 GPU row pitch；Vulkan 使用负 viewport 高度统一输出方向。

指定后端失败时直接报错，不切换后端。退出码 `2` 表示参数或输出目录错误，`3` 表示 GPU/资源/shader
准备失败，`4` 表示帧外材质/缓存构建或增量校验失败，`6` 表示图片读回/写入失败；帧 Extract/Record 错误带诊断终止程序，不返回退出码 `5`。可将 `--shader-dir`
设为不存在的目录检查缺失 shader 提示，将 `--output` 指向已有目录检查路径错误；不会生成伪造图片。
正常退出及帧外返回错误的路径在 GPU 完成后才读取或释放资源；帧执行中的致命错误直接终止，不承诺执行清理回调。

模板参考值的 GPU 回归位于 `AttachmentOperations.StencilReferenceIsDynamicMaskedAndResetPerPass`，
复用现有测试 shader，读回检查 Replace/Equal、掩码、切换 PSO 和新 Pass 重置行为。
在已显式准备测试 shader 后运行（Release 将 preset 改为 `win-x64-release-clangcl`）：

```powershell
cmake --build --preset win-x64-debug-clangcl --target test_radray_runtime test_radray_render --parallel 8
$env:RADRAY_TEST_REQUIRED_BACKENDS = 'd3d12,vulkan'
ctest --preset win-x64-debug-clangcl -R 'MaterialSystem|MaterialTags|MeshDrawingPipeline|AttachmentOperations.*StencilReference' --output-on-failure
Remove-Item Env:RADRAY_TEST_REQUIRED_BACKENDS
```
