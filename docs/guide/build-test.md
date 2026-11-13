> - 适用: 恢复依赖、配置与构建 RadRay、运行测试或生成编译数据库
> - 权威: 本文说明仓库构建操作；选项与预设以锚点中的 CMake 文件为准
> - 锚点: `CMakeLists.txt`, `cmake/Options.cmake`, `CMakePresets.json`, `cmake/Utility.cmake`, `cmake/Dependencies.cmake`, `cmake/RuntimeLibrary.cmake`, `cmake/UpdateCompileCommands.cmake`, `cmake/dependencies/libjpeg.cmake`, `project_manifest.json`, `tools/dev_env.ps1`, `tools/build_third_party.py`, `cmake/Install.cmake`, `cmake/DependencyCatalog.json`, `tools/fetch_third_party.py`, `tools/fetch_sdks.py`, `tools/win_gen_compile_commands.py`, `tools/expand_unity_compile_commands.py`, `modules/*/tests/CMakeLists.txt`, `modules/core/tests/test_memory_override.cpp`, `tools/compile_test_shaders.py`, `tools/compile_render_fixtures.py`, `cmake/TestShaders.cmake`

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

仓库交付 `project_manifest.lock.json`，以完整提交号固定 Git 依赖；`project_manifest.json` 以版本和 EnforceHash 固定 DXC SDK 归档。
锁文件与 manifest 一起保存和分发。`fetch_third_party.py restore` 优先恢复已有 `project_manifest.lock.json` 中的提交；没有 lockfile
时按 manifest 恢复，已安装的默认分支或显式 `Branch` 不会自动检查远端更新。需要更新这些可变引用时运行：

```powershell
python tools/fetch_third_party.py restore --apply
```

`restore --apply` 与 `restore --update` 都会拉取远端默认分支或指定 `Branch` 的最新提交，固定
`Tag` / `Commit` 保持不变。已有 lockfile 的对应条目在安装成功后同步更新；没有 lockfile 时不会
自动生成。`--only xxHash` 可限定依赖，`--no-lock` 可跳过 lockfile；默认分支更新支持 detached HEAD，
并重新读取远端 HEAD，远端切换默认分支后也能跟随。升级固定版本使用 `upgrade --apply`。

显式重新解析 manifest 的固定版本记录使用 `python tools/fetch_third_party.py lock`；更新依赖时检查并一同提交锁文件。
恢复、依赖包准备和主工程构建仍是三个独立的手动步骤；生成或消费锁文件不改变这一边界。

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
`RADRAY_BUILD_PATH` 改写基础路径。当前 DXC manifest 只有 Windows 包，compiler client 的非 Windows
实现返回不可用；macOS 构建须关闭 compiler 与 shader tools。不能将 Windows 验收结果当作 macOS 验证。

### 开发时加载运行库

仓库可执行文件在启动时需要两类 SDK 中的 DLL：DXC fork（`SDKs/radray_dxc/extracted/bin`）和
`/MD[d]` 三元组的 mimalloc（`SDKs/mimalloc/<三元组>/<配置>/bin`，Debug 为 `mimalloc-debug.dll`）。
构建不复制这些 DLL，程序从启动环境的 `PATH` 查找它们。

Windows configure preset 把这两个目录加到子进程 `PATH` 前端，三元组与配置由 preset 环境变量
`RADRAY_PRESET_TRIPLET`、`RADRAY_PRESET_CONFIG` 决定。关联的 build/test preset 默认继承该环境，
覆盖构建期测试发现和 CTest。VS Code 调试配置见[开发环境](dev-env.md#launchjson)。

`cmake --build <目录>`、`ctest --test-dir <目录>`、直接运行 exe 和其他脚本不会读取 preset 环境。
使用这些入口时，先在仓库根为当前 PowerShell dot-source 环境脚本，三元组与配置须和构建目录一致：

```powershell
. tools/dev_env.ps1 -Triplet windows-x64-clangcl -Config Debug
```

脚本只修改当前终端及其子进程的 `PATH`；重复执行会替换上次加入的 SDK 路径，切换配置不会叠加。
缺少 DLL 时 exe 以 `0xC0000135`（STATUS_DLL_NOT_FOUND）退出，构建期 GTest discovery 也会因此失败。
`CMAKE_PREFIX_PATH`、包 `_DIR` 和 `RADRAY_DXC_ROOT` 只决定 CMake 找包；改用其他 DXC 包时，
启动环境也应指向该包的 bin。exe 同目录的 `dxcompiler.dll`、`dxil.dll` 副本会优先于 `PATH`，应显式移除。

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

redirect 要求 mimalloc DLL 先于 `ucrtbase(d).dll` 初始化，即位于最终可执行文件导入表的前部。
链接器按符号解析顺序排列导入的 DLL；render、runtime 等链接 D3D12/DXGI 的程序中 mimalloc 会被排到后面，
启动时报告 `mimalloc-redirect.dll seems to be initialized after ucrtbase.dll` 并退回普通 malloc。
仓库自有可执行文件（测试、benchmark、示例、shader CLI 与 fixture generator）在 `radray_set_build_path`
后调用 `cmake/RuntimeLibrary.cmake` 的 `radray_load_mimalloc_early(<target>)`：`/MD[d]` 且启用 mimalloc 时
链接 `/INCLUDE:mi_version`，让 mimalloc DLL 排在导入表首位；其他 CRT、平台或关闭 mimalloc 时不做任何事。
新增链接 core 的可执行文件时同样调用它。可用 `dumpbin /dependents <exe>` 确认第一项是 mimalloc DLL。

RadRay 不复制或安装这两个外部 DLL，也不向库目标的使用者传递 `/INCLUDE`。外部宿主负责 DLL 搜索路径、
部署和导入顺序，可在自己的可执行文件上使用同样的链接选项。动态覆盖只有在 DLL 成功加载且 redirect
生效后才会接管经动态 CRT 发起的分配；不能仅凭链接声明断言进程已覆盖。可通过 `mi_is_redirected()` 或
`MIMALLOC_VERBOSE=1`（输出 `malloc is redirected`）检查状态。见
[mimalloc 3.5.3 Windows 动态覆盖说明](https://github.com/microsoft/mimalloc/blob/v3.5.3/readme.md#dynamic-override-on-windows)。

运行时的 DLL 搜索路径见[开发时加载运行库](#开发时加载运行库)。

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
且启用 mimalloc 时加入 `MimallocOverrideTest` suite，检查标准 C 分配及 realloc 数据保留、RadRay
分配、对齐分配、C++ 对象/数组和标准容器的 mimalloc 归属。构建完成后可单独执行：

```powershell
ctest --preset win-x64-release-staticcrt -R MimallocOverrideTest
```

`MemoryTest` 在所有配置下验证 RadRay 普通/对齐分配的数据、对齐与配对释放；启用 mimalloc 时
额外使用 `mi_any_heap_contains` 检查归属，包括 `/MD[d]` 构建，不以全局 CRT 覆盖为前提。

`MemoryTest` 只验证 RadRay 分配接口；静态 CRT 和最终 DLL 的标准分配覆盖仍需使用
`MimallocOverrideTest` 及目标映像分别验证。

### 编译契约与优化

Debug 保留工具链的编译检查；CRT 与 STL 调试行为取决于所选路线，工程不改写 `_ITERATOR_DEBUG_LEVEL`。宿主、RadRay 与其
第三方依赖必须使用兼容的 CRT、STL ABI 和 Eigen 编译配置。

core 公开传递 C++20 最低要求、`RADRAY_PLATFORM_*` 和 `RADRAY_IS_DEBUG`；后者保持
非 Release 配置定义的既有语义。消费者可使用 C++23，不需要调用内部编译选项函数。
`RADRAY_ENABLE_AUTO_OPTIMIZATION` 控制自有目标的 Release SIMD/FMA/IPO/LTO。独立非安装
构建默认 ON，`RADRAY_INSTALL=ON` 和子工程默认 OFF；显式设置优先。关闭时继承工具链的
架构、IPO、CRT 和 PIC 设置，不附加自动优化。此默认值在首次配置时确定。

### 预编译头与 Unity Build

项目不主动设置全局 `CMAKE_UNITY_BUILD`，当前自有 target 也没有调用 `radray_enable_unity_build`。
未指定时不主动启用 unity build；显式传入 `-DCMAKE_UNITY_BUILD=ON` 或 `OFF` 时遵循 CMake 的 target 初始化行为。
`cmake/Utility.cmake` 保留该 helper，但定义它本身不启用任何 target。删除缓存项（`-U CMAKE_UNITY_BUILD`）可恢复默认。

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
| `RADRAY_BUILD_EXAMPLES` | ON（含子工程）；要求 runtime、render、libpng |
| `RADRAY_BUILD_COMPILE_COMMANDS` | 独立构建 ON；子工程 OFF；仅 Ninja/Makefile 提供辅助目标 |
| `RADRAY_BUILD_WINDOW`、`RADRAY_BUILD_RENDER` | ON |
| `RADRAY_BUILD_RUNTIME` | ON；要求 render |
| `RADRAY_ENABLE_D3D12` | Windows 且 render 开启时默认 ON |
| `RADRAY_ENABLE_VULKAN` | render 开启时默认 ON |
| `RADRAY_BUILD_SHADER` | ON；独立 shader contract/decoder |
| `RADRAY_BUILD_SHADER_COMPILER` | ON；要求 shader |
| `RADRAY_BUILD_SHADER_TOOLS` | 独立构建且 compiler 开启时 ON；否则 OFF；要求 shader compiler |
| `RADRAY_ENABLE_PROFILER` | 所有配置默认 ON；链接 Tracy client，见[性能采样](performance.md#性能采样tracy) |

验证不带编译器的基础模块时使用独立目录。先按[本地测试 shader](#本地测试-shader)在 compiler 开启的配置中生成夹具；以下配置共享默认夹具目录，不查找或加载 DXC：

```powershell
cmake --preset win-x64-debug -B build_base_no_compiler -DRADRAY_BUILD_SHADER=OFF -DRADRAY_BUILD_SHADER_COMPILER=OFF -DRADRAY_BUILD_SHADER_TOOLS=OFF
. tools/dev_env.ps1 -Triplet windows-x64-msvc -Config Debug
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
set(RADRAY_BUILD_EXAMPLES OFF)
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
examples 默认仍开启，上面的宿主示例显式关闭它；关闭 render/runtime/libpng 时也必须关闭 examples。

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
. tools/dev_env.ps1 -Triplet windows-x64-msvc -Config Debug
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
先完成构建与正确性回归，再串行运行 benchmark；历史报告由[历史目录](../history/README.md)索引。

## 测试

先完成构建再运行 CTest，不并发执行两者；CTest 不负责构建。`-R` 匹配注册的 gtest suite，不是 CMake target。
同一模块的 GTest 源文件链接进一个可执行文件，替换全局 operator new 的 `test_inline_vector` 独占进程。
测试直接使用 CMake `GoogleTest` 模块的 `gtest_discover_tests` 注册，不附加测试专用的 CMake 版本限制。
未设置发现模式时采用官方默认 POST_BUILD；可通过 `CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE` 选择模式。
POST_BUILD 在链接后运行测试程序枚举用例，所需 DLL 须在启动环境中可查找，见[开发时加载运行库](#开发时加载运行库)。
测试的取舍、用例形态与 TDD 流程见[编写测试](testing.md)。

```powershell
cmake --build --preset win-x64-debug-clangcl --parallel 8
ctest --preset win-x64-debug-clangcl
```

| 目标 | suite |
|---|---|
| `test_radray_core` | `BuddyAllocatorTest`、`FirstFitAllocatorTest`、`BasicMathTest`、`BinaryIoTest`、`ChannelTest`、`ManualCoroutineSchedulerTest`、`TaskScopeTest`、`AwaitWithStopTokenTest`、`GetCoroutineStopTokenTest`、`EnumFlagsTest`、`FileTest`、`FormatTest`、`GuidTest`、`HashTest`、`ImageDataTest`、`IntrusivePtrTest`、`IntrusivePtrDeathTest`、`JsonTest`、`JsonSerializerTest`、`JsonDeserializerTest`、`MemoryTest`、`NullableTest`、`RuntimeTypeIdTest`、`ScopeGuardTest`、`SparseSetTest`、`BestFitAllocator`、`StructuredBufferTest`、`TextEncodingTest`、`UtilityTest`、`WavefrontObjTest`、`XmlTest`；libpng 开启时另有 `PngTest`、`PngTest/PngRoundTripTest`；静态 CRT 且启用 mimalloc 时另有 `MimallocOverrideTest` |
| `test_inline_vector` | `InlineVectorTest` |
| `test_radray_shader` | `RadRayShaderDiagnostics`、`RadRayShaderContract`、`RadRayShaderArtifact`（不依赖 compiler） |
| `test_radray_shader_compiler`（compiler 开启） | `RadRayShaderCompilerClient`、`RadRayContractDiscovery`、`RadRayDxcMetadata`、`RadRayShaderLibPass` |
| `test_radray_render` | 见下文 [render 测试](#render-测试) |
| `test_radray_runtime` | `MaterialTagTableTest`、`MaterialTagSetTest`、`MaterialResourceTest`、`MaterialTest`、`MaterialInstanceTest`、`MaterialRenderProxyTest`、`VertexInputTest`、`MeshDrawCommandTest`、`SubmitMeshDrawCommandsTest`、`MeshPassProcessorTest`、`SceneTest`、`FrameSlotTest`、`MeshPassFrameDrawListTest`、`FrameSchedulerTest`、`DeviceContextTest`、`RDG`、`RDGExecutionTest`、`RDGTransientPool`、`RenderGraphFramePipeline`、`GeometryBufferPool`、`UploadArena`、`FrameSchedulerStages`（CPU fake）；`Backends/GeometryUploadGpuTest`、`Backends/BufferUploadGpuTest`、`Backends/TextureUploadGpuTest`（D3D12/Vulkan GPU，几何和纹理用例使用预编译 shader） |
| `test_radray_window`（Win32） | `NativeEventPumpTest`（串行运行，使用短暂的隐藏窗口） |

core 测试使用内联数据或临时目录，不依赖 `assets/`。
`SubmitMeshDrawCommandsTest.DebugPreflightRejectsInvalidListBeforeRecording` 验证 Debug 预检，Release 中跳过。

### render 测试

render 测试在 `test_radray_render` 中，共用 `modules/render/tests/gpu_test_fixture.h`。
GPU suite 以后端参数化，测试名为 `Backends/<Suite>.<Case>/D3D12` 或 `/Vulkan`：

| Suite | 验证内容 |
|---|---|
| `DeviceTest` | 设备与 direct queue 可用，报告实际 adapter |
| `BufferTest` | Upload→ReadBack 复制、persistent map 下的 flush/invalidate、未映射时 cache 操作被 RHI validation 拒绝 |
| `ComputeBindingTest` | 显式布局下的 cbuffer、push constant、SRV/UAV buffer、纹理与 sampler、只重绑一个 group 时其他 group 仍有效、dynamic offset；均以读回内容断言 |
| `RenderPassTest` | clear 读回、全屏三角形、深度测试保留最近片元、模板参考值动态设置且每个 Pass 重置为 0 |
| `TextureViewTest` | 不兼容格式、纹理未声明的用途、越界 mip/layer、缺 stencil aspect 的 depth/stencil view 被拒绝；sRGB view 编码写入 UNORM 纹理；layer view 只写所属层 |

其余 suite：

| Suite | 验证内容 |
|---|---|
| `Routes/ExternalResourceTest` | 同进程外部资源往返，`BufferRoundTrip`、`TextureRoundTrip`（2D RGBA8）各覆盖 `D3D12ToD3D12`、`D3D12ToVulkan`、`VulkanToD3D12`、`VulkanToVulkan`；VK→VK 使用 `OpaqueWin32` 句柄，其余使用 `D3D12Resource`。每个 route 使用独立 device，能力查询不支持时跳过 |
| `VulkanSwapChainRetirement` | 交换链退休状态机，不需要 GPU |
| `VulkanSwapChainWindowTest`（Win32） | 真实窗口上的 present 与 resize 冒烟 |
| `VulkanDeviceFeaturePolicy` | 必需/可选 feature 的请求选择，不需要 GPU |

设备默认开启原生 validation 与 RHI validation；用例结束时等待队列、销毁对象，原生 validation 错误或未被用例取走的
RHI validation 诊断都使用例失败。以下环境变量作用于整个进程：

| 变量 | 作用 |
|---|---|
| `RADRAY_TEST_REQUIRED_BACKENDS=d3d12,vulkan` | 列出的后端未编译或没有可用 adapter 时失败，而不是跳过 |
| `RADRAY_TEST_GPU_VALIDATION=1` | 额外开启 GPU-assisted validation；不要用于性能比较 |
| `RADRAY_TEST_D3D12_ADAPTER_INDEX`、`RADRAY_TEST_VULKAN_ADAPTER_INDEX` | 按各自枚举顺序选择 adapter；未设置时选择高性能设备，非法或不存在的索引失败 |

```powershell
$env:RADRAY_TEST_REQUIRED_BACKENDS = 'd3d12,vulkan'
ctest --preset win-x64-debug-clangcl -R 'Backends/|Routes/|Vulkan' --output-on-failure --timeout 60
```

后端专属代码的链接隔离用独立目录分别配置 `-DRADRAY_ENABLE_VULKAN=OFF`、`-DRADRAY_ENABLE_D3D12=OFF` 构建并运行
`test_radray_render`；被关闭的后端参数与 route 跳过，Vulkan 专属测试源中的用例按编译宏启用。

### 本地测试 shader

Git 只保存测试 HLSL，不保存生成的 `.bin`。统一入口 `tools/compile_test_shaders.py` 准备两类夹具：

- `shader_artifacts/`：先增量构建 compiler 侧的 `radray_shader_fixture_generator`（`EXCLUDE_FROM_ALL`），
  由它编译 `modules/shader/tests/data/shader_sources/*.hlsl`，输出 DXIL / SPIR-V 的 metadata artifact，供 `test_radray_shader` 解码。
- `shader_bytecode/`：`tools/compile_render_fixtures.py` 用 DXC 把 render 与 runtime 的 `modules/<module>/tests/data/*.hlsl` 中登记的 entry
  编译为 `<source>.<entry>.<dxil|spirv>.bin`，供对应模块的 GPU 测试创建 shader。

测试进程只读取这些文件，运行时不加载 DXC。干净检出先恢复并准备依赖、配置 compiler 与 tests 都开启的构建，
再运行脚本，最后构建和运行测试：

```powershell
cmake --preset win-x64-debug-clangcl
python tools/compile_test_shaders.py --build-dir build/windows-x64-clangcl/Debug --config Debug
cmake --build --preset win-x64-debug-clangcl --parallel 8
ctest --preset win-x64-debug-clangcl --output-on-failure
```

需要 Python >= 3.10、CMake、该 build 目录对应的 C++ 工具链和匹配的 **RadRay DXC fork** 包。
默认 DXC 是 `SDKs/radray_dxc/extracted/bin/dxc.exe`，也可传 `--dxc <fork-package>/bin/dxc.exe`；
该目录须有匹配的 `dxcompiler.dll`、`dxil.dll`。stock DXC 不能生成 RadRay metadata。
脚本只为子进程设置 DLL 搜索路径，不下载 SDK 或第三方依赖；缺包、compiler 关闭、配置不匹配或 DXC 编译失败都会停止并显示原因。

默认输出为仓库的 `build/test_shaders/`，含两个夹具目录和 `fixtures.sha256`，属于被忽略的本地构建产物。
Debug/Release、单/双后端与 compiler-disabled 配置可共享同一目录，不并发运行生成脚本。
输入源码、include、配方、生成器及 DXC 都不变且产物完整时脚本跳过编译；输入变化、bin 丢失/损坏或 `--force` 时重新生成，
两类生成全部成功后才替换夹具并写校验清单。

```powershell
python tools/compile_test_shaders.py --check
python tools/compile_test_shaders.py --build-dir build/windows-x64-clangcl/Debug --config Debug --force
```

shader/render 测试的构建依赖 `radray_check_test_shaders`，只检查源指纹、清单与文件 SHA-256，不启动 DXC。
夹具缺失或过期时构建失败并提示生成命令；configure 允许夹具尚未存在，因此可以先构建生成器。
直接运行已有 exe/CTest 不触发该检查，修改输入或搬运产物后先运行 `--check`。

关闭 compiler 的配置不增加 DXC 链接或运行时依赖，但首次准备夹具必须使用另一个 compiler 与 tests 都开启的配置。
纯库构建（`RADRAY_BUILD_TESTS=OFF`）不要求夹具或 Python 检查器。目前只在 Windows 生成夹具；
其他平台可复制同版本源码在 Windows 生成的完整目录并运行 `--check`，Windows 的测试结果不能当作其他平台验证。
自定义目录需让生成脚本与所有消费配置一致：

```powershell
python tools/compile_test_shaders.py --build-dir build/windows-x64-clangcl/Debug --output-dir build/custom_test_shaders
cmake --preset win-x64-debug-clangcl -B build/no_compiler -DRADRAY_BUILD_SHADER_COMPILER=OFF -DRADRAY_BUILD_SHADER_TOOLS=OFF -DRADRAY_TEST_SHADER_DIR="$PWD/build/custom_test_shaders"
. tools/dev_env.ps1 -Triplet windows-x64-clangcl -Config Debug
cmake --build build/no_compiler --config Debug --parallel 8
ctest --test-dir build/no_compiler -C Debug --output-on-failure
```

`RADRAY_TEST_SHADER_DIR` 只影响测试数据路径，不改变源码根或产品安装内容。
单独运行 `compile_render_fixtures.py --output-dir <raw-directory>` 只用于调试 render 配方，不更新校验清单；之后应再运行统一入口。

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
`test_radray_runtime`。CPU 用例使用 `modules/runtime/tests/test_fakes.h` 的 RHI fake；GeometryUploadGpuTest 共用 render 的 GPU fixture，验证分页上传、池间绘制和 BeginFrame 后释放上传缓冲，需预先生成本地测试 shader。
它覆盖材质标签、材质参数与快照、MeshDrawCommand 校验与录制、Processor 构建、Scene 增量缓存，
以及 FrameScheduler 的槽位轮转、等待、Scene 借用、退役资源与析构顺序；RDG 覆盖裁剪、上传、同槽物理状态复用、原地 Compute、UAV 与附件屏障及管线钩子顺序。suite 列表见[测试](#测试)：

```powershell
python tools/compile_test_shaders.py --build-dir build/windows-x64-clangcl/Debug --config Debug
cmake --build --preset win-x64-debug-clangcl --target test_radray_runtime --parallel 8
ctest --preset win-x64-debug-clangcl -R '^(Material|VertexInput|MeshDraw|SubmitMeshDraw|MeshPass|Scene|Frame|DeviceContext|RDG|RenderGraphFramePipeline|GeometryBufferPool|UploadArena|Backends/(Geometry|Buffer|Texture)UploadGpuTest)' --output-on-failure
```

Release 使用同名 `win-x64-release-clangcl` 预设。样例的双后端运行覆盖真实 GPU 路径；
后端回归入口见[测试](#测试)，模块接口见 [Rendering runtime](../architecture/runtime.md)。

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
显式 Update 后 Proxy 才变为 1。Frame 0 提交后执行 Scene::UpdateMaterial，Frame 1 先显式同步当前 registrations，再设置管线输入并调用 `RunFrame()`；日志验证仅绿色材质关联的一个 Batch 重建，其余九个 Batch 的驻留状态索引不变，
且该命令对应绑定池载荷的颜色常量 RGB 从 0.5 倍更新为 1.0 倍、alpha 不变。
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
透明示例只包含一个凸物体，剔除背面后只画可见表面；没有多透明物体排序或交叉透明表面的处理。
完整字段和生命周期见 [材质与 Pass 的边界](../architecture/runtime.md#材质与-pass-的边界)。
Mask、折射、透明排序、OIT、纹理绑定和静态 shader 变体尚未实现。

建议按下面顺序阅读，场景与材质准备保留中文步骤注释，帧循环的职责和生命周期见下文：

| 文件 | 阅读重点 |
|---|---|
| [sample.h](../../examples/mesh_drawing_pipeline/sample.h) / [sample.cpp](../../examples/mesh_drawing_pipeline/sample.cpp) | 准备共享资源与场景，创建 Scheduler，每帧设置管线输入并显式同步当前 registrations，再调用 RunFrame，帧间更新材质并验证局部重建，WaitIdle 后保存图片 |
| [material.h](../../modules/runtime/include/radray/runtime/material.h) / [material.cpp](../../modules/runtime/src/material.cpp) | shader/布局借用、材质性质、材质默认值、单层实例继承、类型不变的参数覆盖、Scene 拥有的参数快照 |
| [pipeline.h](../../examples/mesh_drawing_pipeline/pipeline.h) / [pipeline.cpp](../../examples/mesh_drawing_pipeline/pipeline.cpp) | Processor 筛选并复制命令颜色；MeshDrawingFramePipeline 继承 RenderGraphFramePipeline，实现 Pass 登记、帧数据提取、RDG 构图、存活 Pass 命令借用与提交/等待 |
| [mesh_pass_processor.h](../../modules/runtime/include/radray/runtime/mesh_pass_processor.h) / [mesh_pass_processor.cpp](../../modules/runtime/src/mesh_pass_processor.cpp) | 单个 Batch 的 Processor 接口、显式传入的输出 Context、共享状态和返回 void 的构建 helper；单步 AddCommand 发布完整命令。缓存 Context 写入连续命令表与 Batch 槽位；动态 Context 自动追加当帧可见命令 |
| [mesh_draw_command.h](../../modules/runtime/include/radray/runtime/mesh_draw_command.h) / [mesh_draw_command.cpp](../../modules/runtime/src/mesh_draw_command.cpp) | 64 字节 Batch 命令、VB/Payload 驻留池与自持有 PushConstants、Pass 提交参数与延迟实例流，可见命令指针与 SubmitMeshDrawCommands |
| [gpu_support.h](../../examples/mesh_drawing_pipeline/gpu_support.h) / [gpu_support.cpp](../../examples/mesh_drawing_pipeline/gpu_support.cpp) | GpuResources 准备共享 Device 几何缓冲与待上传字节，GpuFrameResources 持有每槽缓冲、参数组与 fence；附件交给 RDG 瞬态池 |
| [cube.hlsl](../../examples/mesh_drawing_pipeline/cube.hlsl) | VS 读取 80 字节 Pass UBO 和 64 字节实例矩阵；PS 读取 16 字节命令颜色，Transparent 使用颜色 alpha |

本例直接组装 Batch、假定全部候选可见，用 Scene 的持久缓存和脏追踪更新命令，按 Pass 声明的状态/深度优先键稳定排序；
不实现多线程代理同步、可见性剔除、LOD 或自动实例化。RDG 使用本项目的单队列整资源实现，Color Pass 只做纯色上色。
材质路径的 M1～M5 注释分别说明渲染资源、用户定义/实例、渲染侧参数同步、PSO 组装与 shader bindings。
材质职责与所有权见 [Runtime 材质核心](../architecture/runtime.md#材质核心)；本例同步的是 CPU 快照，没有稳定 GPU uniform buffer 绑定。
`BatchPassPolicy` 仅属于样例，通过 Batch 句柄关联记录。
候选收集不按 Pass 预先分组，Processor 内的条件才决定是否生成命令。
`MeshDrawingFramePipeline::GetMeshPassRegistrations` 登记四个 Processor。调用方每帧 SetInput，并以当前 registrations 调用 `Scene::SyncCachedMeshDrawCommands`，随后 `RunFrame()`，返回后 ClearInput；Scheduler 不同步 Scene。
Scene 为脏 Batch 创建缓存 Context 并显式传给 `AddMeshBatch`，Processor 完成 Pass 筛选后调用 runtime 的 `BuildMeshDrawCommands`，
Debug 下重新检查借用的顶点输入与共享状态，复用 Scene 入库时的 Element 校验；Release 构建信任调用方提供合法输入。
随后通过单步 `AddCommand` 输出 Batch 命令并记录 Pass 热表槽位。
绿色材质变化后用 `Scene::UpdateMaterial` 标脏，再同步时仅引用它的 Batch 重新进入四个 Processor。
样例检查未修改 Batch 的 VB/Payload 索引不变，并核对修改 Batch 的颜色常量数值；缓存稠密下标不能作为跨重建身份。

Color/Transparent Processor 在 `AddMeshBatch` 中读取 Proxy 的 BaseColor/Brightness，将 RGB 相乘并保留 alpha，
经 runtime 构建 helper 复制为绑定池自持有的 16 字节 `MaterialConstants`；Outline Processor 写入固定金色，Depth 不写颜色常量。
绑定池自持有颜色字节，源栈数据在构建后即可失效。
`GpuResources::PrepareGeometry` 创建共享的 Device VB / IB 空壳并保存 CPU 字节，Scene 可在上传前引用这些合法缓冲。
每个 GpuFrameResources 持有可复用的两组 Pass 参数组、读回缓冲和 completion fence。
实例流与两份 Pass 常量通过 RDG 动态 Upload buffer 声明，Prepare 从本槽 UploadArena 分配最终切片。
参数组绑定的页或范围变化时，在 Prepare 中 ResetBindingsForReuse、Set 并 FlushWrites；附件及 View 由每槽 RDG 瞬态池拥有。
ExtractFrameData 校验当前帧输入，不构造完整实例或常量字节数组；LocalScale 分别为 1.0 / 1.12 的常量在 Prepare 中直接生成。
`SetupGraph` 接收只读 Scene，本例使用已准备的共享几何资源声明上传、瞬态深度/颜色附件、四个 Raster Pass 与 Readback Copy。
首帧共享几何经 RDG 的 Device Staging Copy 上传，并通过 MarkOutput 保活，终态为 Vertex / Index；后续帧沿用该状态。
实例/常量来源只由消费者保活，直接填写映射切片，不录制额外 GPU Copy；上传页汇总只读用途，读回缓冲终态为 HostRead。

Compile 先裁剪，再通过 `ExtractMeshPass` 调用 `PreparePassDrawCommands` 收集存活 Pass 的缓存命令只读指针，
并把候选 Primitive 下标写入 `FirstInstance`，不访问材质参数。持久缓存的脏同步仍覆盖全部登记的 Processor。
基类 Prepare 固定并稳定排序存活列表；实例生成源收集各存活 Pass 的可见实例并集，将本帧 FirstInstance 改写为紧凑索引，
只计算和写入一次每个可见实例的矩阵，不修改持久缓存命令。屏障在 Prepare 冻结，Record 通过本槽命令缓冲消费准备结果。
`RecordPass` 只设置 viewport/scissor，将本槽 Pass 参数组和实例流通过 `MeshPassSubmitArgs` 交给列表的 `Submit(encoder, submitArgs)`，在帧内 Scene 借用有效期内直接解析缓存命令。
Depth/Color/Transparent 共用普通缩放参数组，Outline 使用放大参数组；runtime 先绑定 PSO，再在首次绑定或切换 PSO 后重绑 Pass 参数组，
最后完成几何、模板、命令参数和 DrawIndexed。相机、局部缩放或变换变化不需要重新调用 Processor，材质变化则重建关联命令的颜色副本。

每帧最后由图记录颜色读回拷贝与收尾屏障，`SubmitFrame` signal 本槽 fence 后返回；复用槽时由 Pipeline.Wait 等待该值。
每槽上传缓冲、瞬态附件池和读回缓冲独立；颜色 CopySource、深度 DepthWrite 的真实物理终态保存在池内，复用时作为首次 barrier 的 Before。
主循环只调用 RunFrame，结束时调用 WaitIdle，再从 `(frameCount - 1) % slotCount` 对应的读回缓冲保存图片。
Scheduler 的析构同样等待全部在途槽并重置命令存储，随后才销毁 Pipeline 的每槽资源和瞬态池；外部共享资源和源 Scene 晚于 Scheduler 销毁。
完整接口、裁剪与上传约束见 [RDG 与 RenderGraphFramePipeline](../architecture/runtime.md#rdg-与-rendergraphframepipeline)。
样例 Processor 检查到材质句柄失效、未同步材质、标签表身份不一致、缺失 BatchPassPolicy 或 PSO、参数提取失败时均带诊断终止程序。
runtime 构建 helper 的输入校验仅在 Debug 启用，Release 信任前置条件，不保证构建违约诊断；
未绑定材质标签表或标签不匹配仍合法跳过该 Pass。具体构建契约见 [增量持久缓存](../architecture/runtime.md#增量持久缓存)。

### 准备 shader 与构建

在已准备好 Debug/Release 依赖包的环境中执行。`RADRAY_BUILD_EXAMPLES` 默认 **ON**；开启时必须
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

直接运行 exe 前，按其配置设置当前终端的 DLL 搜索路径；下面在 Debug/Release 两组命令之间显式切换。

```powershell
. tools/dev_env.ps1 -Triplet windows-x64-clangcl -Config Debug
./build/windows-x64-clangcl/Debug/_build/Debug/radray_mesh_drawing_sample.exe --backend d3d12 --output build/mesh_drawing_pipeline/d3d12-debug.png
./build/windows-x64-clangcl/Debug/_build/Debug/radray_mesh_drawing_sample.exe --backend vulkan --output build/mesh_drawing_pipeline/vulkan-debug.png
. tools/dev_env.ps1 -Triplet windows-x64-clangcl -Config Release
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
日志应打印五个 Primitive、十个 Batch，以及每帧各 Pass 的 `对象 / Primitive / Batch / ElementMask → BatchCommand`
对应关系；BatchCommand 编号为当前 Pass 输出列表中的序号。本例每 Batch 只有一个 Element，每条命令对应一次 Draw。跳过日志按 Batch 输出一次，
不分配命令编号，并打印原因。每帧逐 Pass 打印 `Frame=<编号> Slot=<槽下标> MeshPass=<0～3>`，
四个 Pass 的 Draw 数分别为 5、6、2、2（总计 15），并打印 fence 提交/等待日志；默认槽次序为 0、1、0、1、0、1。
最后打印 PNG 路径及六帧完成日志。同一槽的四个 Pass 使用一份 D32_FLOAT_S8_UINT 附件；Depth Pass 清深度为 1、模板为 0 并保存，
Color Pass `Load` 深度和模板，以材质默认的 `LessEqual` 测试并写深度。
这样既能复用预深度，也能接纳 `ColorOnly` 对象并处理其自身和相互遮挡；如果仍使用 `Equal`，
没有预深度的表面会消失。后续 Outline/Transparent 均 `Load` 颜色、深度和模板。
RDG 将 Color、Outline、Transparent 合并到同一 RenderPass，组内省略重复的附件写屏障，每个逻辑 Pass 入口重置模板参考值。
Depth 与 Color 之间仍保留跨组 `DepthWrite → DepthWrite` barrier；组入口批量转换实例流为 Vertex、两份 Pass UBO 为 CBuffer，
其中 Outline 才首次使用的 UBO 屏障前移到合并组入口。合并条件与重排契约见 [RDG](../architecture/runtime.md#确定性重排与合并)。
录制顺序为 PSO → Pass 参数组/实例流 → 顶点缓冲 → 模板参考值 → 命令参数/颜色常量 → 索引缓冲/DrawIndexed。
检查 800×600 图片中的三个不透明物体、左侧背景色缺口、右侧物体金色轮廓，以及前方青色物体透出的绿色和背景。
读回逐行去除 padding，日志打印实际 GPU row pitch；Vulkan 使用负 viewport 高度统一输出方向。

指定后端失败时直接报错，不切换后端。退出码 `2` 表示参数或输出目录错误，`3` 表示 GPU/资源/shader
准备失败，`4` 表示材质/场景初始化、帧间材质更新或增量校验失败，`6` 表示图片读回/写入失败；Pass 登记与帧 Extract/Prepare/Record/Submit 检查到的违约带诊断终止程序，不返回退出码 `5`。可将 `--shader-dir`
设为不存在的目录检查缺失 shader 提示，将 `--output` 指向已有目录检查路径错误；不会生成伪造图片。
正常退出及帧外返回错误的路径在 GPU 完成后才读取或释放资源；帧执行中的致命错误直接终止，不承诺执行清理回调。

描边依赖的模板参考值行为由 `RenderPassTest.StencilReferenceIsDynamicAndResetsEachPass` 覆盖，
读回检查 Replace/Equal、切换 PSO 保留参考值和新 Pass 重置为 0。在已显式准备测试 shader 后运行
（Release 将 preset 改为 `win-x64-release-clangcl`）：

```powershell
cmake --build --preset win-x64-debug-clangcl --target test_radray_runtime test_radray_render --parallel 8
$env:RADRAY_TEST_REQUIRED_BACKENDS = 'd3d12,vulkan'
ctest --preset win-x64-debug-clangcl -R '^(Material|VertexInput|MeshDraw|SubmitMeshDraw|MeshPass|Scene|Frame|DeviceContext|RDG|RenderGraphFramePipeline)|StencilReference' --output-on-failure
Remove-Item Env:RADRAY_TEST_REQUIRED_BACKENDS
```
