# RadRay

C++20 实时渲染器，D3D12 + Vulkan 后端。

## 文档入口

本文件也是长期文档，负责仓库级约束与导航。按任务读取，不预加载整个 docs。

| 任务 | 入口 |
|---|---|
| 定位子系统、理解当前设计 | [架构地图](docs/architecture/overview.md)，再读所属子系统文档 |
| 配置、构建、测试、恢复依赖 | [构建与测试](docs/guide/build-test.md) |
| 性能采样、运行基准、比较改动 | [性能测量](docs/guide/performance.md) |
| 编写 C++ | [C++ 约定](docs/guide/cpp-conventions.md) |
| 编写 HLSL、修改 binding | [Shader authoring](docs/guide/shader-authoring.md)、[Shader pipeline](docs/architecture/shader-pipeline.md) |
| 整理或同步文档 | 项目技能 [radray-doc](.agents/skills/radray-doc/SKILL.md) |
| 追问、推敲设计 | 项目技能 [radray-grill](.agents/skills/radray-grill/SKILL.md) |

长期知识只放在本文件、`docs/architecture/` 与 `docs/guide/`；README 是项目介绍与入口。
术语、约束和必要的设计理由就地维护在所属文档，历史由 Git 保存。
行为变化时同步更新对应长期文档。开发历史放在 `docs/history/`，仅在用户明确要求或同意保存时写入；未授权则默认不写。AI 可以说明拟记录的内容与保留价值，询问是否入库，但不必每次改动都询问；已有授权不重复确认。
已有历史记录与附件保持原样；与历史相比的变化只在当前所属文档说明。历史文件使用 `YYYY-MM-DD-主题.扩展名`，注明状态与基线，不作为当前契约；具体归档规则见[文档维护](docs/guide/documentation.md#开发历史)。
代码注释写 API 契约与局部约束，设计说明放文档；代码至多保留一处指向所属文档的文件头入口。

## 硬规则

- C++ 的 `.h` 与 `.cpp` 默认按同名文件一一对应，头文件声明的非内联函数实现在对应 `.cpp` 中；确需采用不同命名或非一一对应的文件组织时，必须先说明原因、询问用户并获得同意。
- STL 容器使用 `radray/types.h` 的别名；协程使用 `radray/coroutine.h` 的别名，不直接用 `exec::task` / `stdexec::*`。
- 可空接口指针使用 `Nullable<T>`，裸指针表示非空；Debug 判断使用 `RADRAY_IS_DEBUG`。
- 字符串格式化使用 `fmt`，先查已有 formatter；标志枚举使用 `enum_flags.h` 的 `EnumFlags<T>`、`is_flags<T>`、`format_as`。
- 性能插桩只用 `radray/profiler.h` 的 `RADRAY_PROFILE_*` 宏，不直接 include `tracy/*`；仅 render 后端为 GPU 时间戳例外。
- 不重命名已有枚举成员，它们被 `magic_enum` 与序列化消费；需要改名时新增成员并显式迁移数据。
- 新增或引入使用全局变量、全局 `static` 变量或函数内 `static` 共享状态前，必须先询问用户并获得同意，不得直接使用。
- 新增任何 `try`、`catch`、`throw` 前先征得用户同意。优先验证、`std::error_code` 或现有结果类型。
- 不为保留 `noexcept` 增加捕获；仅捕获具体、可恢复的异常，不用 `catch (...)` 把分配失败、程序错误或不变量破坏转成空值/诊断。
- 模块基础依赖：shader → core，window → core，render → core，runtime → render/core；可选 shadercompiler → shader/core。
- shader 不依赖 DXC 或 render；公共 shader/render 契约不依赖 DXC SDK 头。
- `third_party/`、`SDKs/` 是脚本填充的只读目录，不编辑。依赖分为用户手动执行的 fetch、`build_third_party.py` 和主工程 `find_package` 三阶段；配置、构建、安装不自动拉取或准备依赖。安装只导出 RadRay 自身，外部依赖由包发现提供。
- HLSL include 以 `shaderlib/` 为根，使用 `<core/math.hlsli>` 形式；当前不使用文件相对的双引号 include。
- `.hlsl` 是 entry source，`.hlsli` 是带 include guard 的库；entry 通过标准 `[shader("...")]` 声明。
- `shaderlib/core/platform.hlsli` 是唯一 target gate，`VK_BINDING(binding, set)` 直接使用 binding 数字；不新增平行 metadata 或编号包装。
- 测试放在 `modules/<module>/tests/`，使用 CMake 官方 `add_executable`、`target_link_libraries` 和 `gtest_discover_tests` 注册。同一模块的 GTest 源文件链进一个可执行文件；只有必须独占进程映像的测试（例如替换全局 `operator new`）单独成 exe。discovery 遵从 CMake 官方默认值或 `CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE` 设置。
- 正确性测试使用 Google Test（GTest）；性能测试使用 Google Benchmark，不用 `TEST` / `TEST_F` 承载基准测量。
- 禁止为了测试在生产代码中添加仅供测试使用的字段，包括统计计数、事件轨迹和观测状态；这些数据由测试 fixture/probe 自行持有和记录。
- `ctest -R` 匹配测试名中的 gtest suite，不是 CMake target。构建完成后再运行测试，不并发执行构建与测试。
