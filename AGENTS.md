# RadRay

C++20 实时渲染器，D3D12 + Vulkan 后端。

## 文档入口

本文件也是长期文档，负责仓库级约束与导航。按任务读取，不预加载整个 docs。

| 任务 | 入口 |
|---|---|
| 定位子系统、理解当前设计 | [架构地图](docs/architecture/overview.md)，再读所属子系统文档 |
| 配置、构建、测试、恢复依赖 | [构建与测试](docs/guide/build-test.md) |
| 编写 C++ | [C++ 约定](docs/guide/cpp-conventions.md) |
| 编写 HLSL、修改 binding | [Shader authoring](docs/guide/shader-authoring.md)、[Shader pipeline](docs/architecture/shader-pipeline.md) |
| 整理或同步文档 | 项目技能 [radray-doc](.agents/skills/radray-doc/SKILL.md) |
| 追问、推敲设计 | 项目技能 [radray-grill](.agents/skills/radray-grill/SKILL.md) |

长期知识只放在本文件、`docs/architecture/` 与 `docs/guide/`；README 是项目介绍与入口。
术语、约束和必要的设计理由就地维护在所属文档，历史由 Git 保存。
行为变化时同步更新对应文档；会话笔记、实施清单与交接不自动落入仓库。
代码注释写 API 契约与局部约束，设计说明放文档；代码至多保留一处指向所属文档的文件头入口。

## 硬规则

- STL 容器使用 `radray/types.h` 的别名；协程使用 `radray/coroutine.h` 的别名，不直接用 `exec::task` / `stdexec::*`。
- 可空接口指针使用 `Nullable<T>`，裸指针表示非空；Debug 判断使用 `RADRAY_IS_DEBUG`。
- 字符串格式化使用 `fmt`，先查已有 formatter；标志枚举使用 `enum_flags.h` 的 `EnumFlags<T>`、`is_flags<T>`、`format_as`。
- 不重命名已有枚举成员，它们被 `magic_enum` 与序列化消费；需要改名时新增成员并显式迁移数据。
- 新增任何 `try`、`catch`、`throw` 前先征得用户同意。优先验证、`std::error_code` 或现有结果类型。
- 不为保留 `noexcept` 增加捕获；仅捕获具体、可恢复的异常，不用 `catch (...)` 把分配失败、程序错误或不变量破坏转成空值/诊断。
- 模块基础依赖：shader → core，window → core，render → shader/core，runtime → render/window/shader/core。
- shader 不依赖 DXC 或 render/runtime；可选 shadercompiler → shader/core。runtime 仅在启用 JIT 时链接该 client，公共 shader/render/runtime 契约不依赖 DXC SDK 头。
- `third_party/`、`SDKs/` 是脚本填充的只读目录，不编辑。
- HLSL include 以 `shaderlib/` 为根，使用 `<core/math.hlsli>` 形式；当前不使用文件相对的双引号 include。
- `.hlsl` 是 entry source，`.hlsli` 是带 include guard 的库；entry 通过标准 `[shader("...")]` 声明。
- `shaderlib/core/platform.hlsli` 是唯一 target gate，`VK_BINDING(binding, set)` 直接使用 binding 数字；不新增平行 metadata 或编号包装。
- 测试放在 `modules/<module>/tests/`，使用 `radray_add_test` 或 `radray_add_radray_gtest_case` 注册；discovery 保持 `PRE_TEST`。
- `ctest -R` 匹配测试名中的 gtest suite，不是 CMake target。构建完成后再运行测试，不并发执行构建与测试。
