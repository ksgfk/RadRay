> - 适用: 写这个仓库的 C++ 代码；不确定某个写法是否符合仓库惯例
> - 权威: 本文是命名、文件组织、接口风格、测试文件组织与注册的唯一说明；测试取舍、用例形态与 TDD 流程在 testing。硬规则（会被判定为缺陷的那些）在 `AGENTS.md`
> - 锚点: `.clang-format`, `cmake/Utility.cmake`

# C++ 约定

[AGENTS.md](../../AGENTS.md) 的硬规则必须遵守。本文补充命名、接口与测试的具体写法；
涉及硬约束的地方明确注明，其余惯例遵循所在文件的既有风格。

## 命名

| 类别 | 风格 | 例 |
|---|---|---|
| 类型（class / struct / enum） | PascalCase | `Device`, `TaskScope`, `ScopeGuard` |
| 函数与方法（含自由函数） | PascalCase | `CreateBuffer`, `BeginRenderPass`, `PerspectiveLH` |
| 私有成员变量 | `_` 前缀 + camelCase | `_activePasses`, `_renderStateCreated` |
| 公开数据成员（descriptor / POD） | PascalCase | `BufferDescriptor::Usage`, `Viewport::Height` |
| `constexpr` 常量 | `k` 前缀 + PascalCase | `kShaderAssetFormatVersion`, `kMaxPointLights` |
| 宏 | ALL_CAPS | `RADRAY_ABORT`, `RADRAY_FORWARD_OBJECT_GROUP` |
| 命名空间 | 全小写 | `radray`, `radray::render`, `radray::detail` |

**没有 `m_` 前缀**，一律 `_`。公开数据成员用 PascalCase 是刻意的：它让 descriptor struct
的字段读起来像"属性"而非局部变量。

枚举成员在**新代码里 PascalCase**（`AssetState::Ready`、`LightType::Point`），
部分已有枚举使用 ALL_CAPS（例如 `TextureFormat::UNKNOWN`）。
**不要为统一风格去改它们**——`AGENTS.md` 禁止重命名枚举成员，`magic_enum` 与序列化数据在消费
这些名字。

文件内辅助函数有两派并存，**以文件为单位自洽**：render/shader 后端用 `static` + `_` 前缀
（`_CreateVkSwapChain`），core 用匿名 namespace + 无前缀。改哪个文件就跟那个文件。

## 文件组织

头文件一律 `#pragma once`，没有 include guard。

include 顺序没有工具强制（`.clang-format` 里 `SortIncludes: false`），惯例是：

```cpp
// foo.cpp
#include <radray/foo.h>      // 自身头文件第一

#include <algorithm>         // 标准库
#include <optional>

#include <radray/logger.h>   // 其余 radray 头
#include <radray/types.h>
```

平台相关的 include 用 `#if defined(RADRAY_PLATFORM_WINDOWS)` 包在末尾。

`.cpp` 内部实现放匿名 namespace（core）或文件级 `static`（render / shader 后端），
见上文命名一节。

## 接口风格

整数上下界统一使用 `<limits>` 中的 `std::numeric_limits<T>::min()` / `max()`，使用处直接包含 `<limits>`；
不使用 `<cstdint>` / `<stdint.h>` 的整数边界宏。

**可空指针用 `Nullable<T>`，可空值用 `std::optional`。** 这条分界在仓库里执行得很干净：

```cpp
Nullable<unique_ptr<Buffer>> CreateBuffer(...);   // 指针类，可能没有
std::optional<uint64_t> FindOffset(...);          // 值类，可能没有（示意）
```

`Nullable` 的 API 是 `HasValue()` / `Get()` / `Release()` / `Unwrap()` / `operator bool`。
**没有 `Value()`**。`Unwrap()` 会在为空时抛 `NullableAccessException`——它是基础设施的误用检测，
不是业务错误处理手段。

**裸指针意味着非空**（`AGENTS.md` 硬规则）。想表达"可能为空"就用 `Nullable`。

`bool` + out 参数用在"填充一个已有对象"的场合（例如 `JsonDeserializer<T>::Read(value, output)`），
不用来代替返回值。

**错误处理的主流做法是：`RADRAY_ERR_LOG` + 返回空/false。** 不可恢复的不变量违反用
`RADRAY_ABORT`。业务代码几乎不用异常——异常只出现在三类边界：第三方 C/C++ 库
（例如 libpng）、平台 API、以及需要处理可恢复异常的 C 回调。新增 `try`/`catch`/`throw`
**要先问用户**（`AGENTS.md`）。

`noexcept` 用得很密：访问器、setter、析构一律标。虚接口批量标（`rhi.h` 大部分纯虚方法）。
但**不要为了保住 `noexcept` 而加 try/catch**——那是硬规则禁止的。

## 构造与析构

用于继承的基类必须声明虚析构函数，派生类显式声明析构时标记 `override`，避免通过基类销毁时遗漏派生资源。

**RHI 与 window 层用「接口 + 静态 Create 工厂」**，返回 `Nullable<unique_ptr/shared_ptr>`：

```cpp
static Nullable<shared_ptr<Device>> Device::Create(const DeviceDescriptor& desc);
```

**两段式析构（`Destroy()` + `DestroyImpl()`）集中在 render 后端实现类**，理由见
[RHI 所有权](../architecture/render-rhi.md)。普通资源包装使用析构函数与成员声明顺序。

RAII 包装类的后缀是 `Scope` / `Scoped` / `Guard`，**没有 `*RAII`**：`ScopeGuard`、`TaskScope`、
`ScopedBufferMap`。（`d3d12_impl.h` 内部的 `DescriptorHeapViewRAII`
是后端私有的例外。）

**成员声明顺序常常有语义**，因为析构是逆序的。这类地方都有注释标出，不要"顺手整理"。

## 每帧热路径

每帧按 primitive / draw 执行的函数（snapshot 变换、culling、renderer list 处理、后端 encoder 绑定）
要同时对 Release 与 Debug 负责：Debug 默认 `/Od /Ob0`，Eigen 表达式模板、`std::span`、小容器的
每个算子都是一次真实调用，Debug 帧率主要由这些路径决定。约定：

- 每 primitive 一次的 bounds/剔除数值代码优先使用直接分量访问（当前入口为 `raster_scene.cpp`、`render_scene.cpp`）；
  既有矩阵运算可继续使用 Eigen，是否展开由测量决定。Eigen 是列主序，`(r, c)` 在 `data()[c * 4 + r]`。
- 不在每 draw 路径上构造 `string`、`fmt::format`、按名字查找绑定；启动或首次遇到 program 时解析成
  绑定句柄或索引再复用。
- 以 snapshot 索引为键的每帧查找用稠密槽位表而不是 `unordered_map`。
- 后端 encoder 保留 PSO 重复绑定检查；VB、IB 与 shader 参数组每次合法绑定都下发原生命令，
  不保存去重缓存。具体行为见 [RHI 与后端](../architecture/render-rhi.md)。

性能改动使用适用的基准或 Tracy 数据验证，不凭直觉声称收益。

## 测试

测试源码放 `modules/<module>/tests/test_<topic>.cpp`。同一模块的这些源文件链进一个可执行文件
（`test_radray_core`、`test_radray_render`、`test_radray_shader_compiler`），
`gtest_main` 和被测库只链接一次。CTest 仍按每个 `TEST()` 启动进程。只有不能和其它用例共享进程映像的测试另开 exe：
目前是 `test_inline_vector`，它替换全局 `operator new` / `operator delete`。`EXPECT_DEATH` / `EXPECT_EXIT`
已经在子进程里跑断言，不因此再拆可执行文件。

测什么、怎样组织用例以及 TDD 流程见[编写测试](testing.md)。测试观测遵守 [AGENTS.md](../../AGENTS.md)
的字段限制，优先通过实际输出和对象状态验证，不增加专用计数接口；运行时性能插桩沿用 `RADRAY_PROFILE_*`。

```cmake
add_executable(test_radray_core test_foo.cpp test_bar.cpp)
target_link_libraries(test_radray_core PRIVATE radraycore GTest::gtest_main)
radray_default_compile_flags(test_radray_core)
radray_optimize_flags_binary(test_radray_core)
radray_set_build_path(test_radray_core)
radray_load_mimalloc_early(test_radray_core)
gtest_discover_tests(test_radray_core)
```

链接 core 的可执行文件都调用 `radray_load_mimalloc_early`，原因见
[Windows 动态 CRT 与 mimalloc](build-test.md#windows-动态-crt-与-mimalloc)；不链接 core 的 `test_inline_vector` 不调用。

顶层开启测试时加载 CMake `GoogleTest` 模块，各模块直接调用官方接口注册。
`gtest_discover_tests` 枚举可执行文件中的用例；发现模式见[构建与测试](build-test.md#测试)。
通过 `PROPERTIES` 设置测试环境变量、超时或串行运行要求；需要划分不同属性的用例组时，
可使用互不重叠的 `TEST_FILTER` 分别发现。依赖 DXC 的测试通过启动环境查找 SDK，不复制 DLL；
构建期 discovery 与测试执行的环境配置见[开发时加载运行库](build-test.md#开发时加载运行库)。

**`ctest -R` 匹配 gtest 套件名（C++ 类名），不是 cmake 目标名。** 对照表在
[build-test](build-test.md)。

需要 GPU 的测试使用公共 fixture，跳过与失败的规则见[编写测试](testing.md#gpu-测试)。

现成的测试辅助设施：

| 设施 | 在哪 | 用途 |
|---|---|---|
| `ScopedTempDirectory`、`ScopedTempFile` | 各测试文件的匿名命名空间 | 临时目录 / 临时文件，析构时删除 |
| `GpuTest`、`BackendTest`、`LoadShader`、`ReadBuffer`/`ReadTexture` 等 | `modules/render/tests/gpu_test_fixture.h` | 后端参数化、设备创建、字节码加载、上传与读回 |
| `FakeDevice`、`FakeGpu`、`RecordingEncoder` 等 | `modules/runtime/tests/test_fakes.h` | runtime 测试的 CPU fake，不需要 GPU |
| `DiscoverAndCompile`、`DecodeLane` 等 | `modules/shader_compiler/tests/compiler_test_utils.h` | 编译 shaderlib 源并解码各 target 的 artifact |

需要仓库根路径的测试使用编译期定义 `RADRAY_PROJECT_DIR`，目前只有 shader_compiler 测试与 fixture 生成器使用。
本地测试 shader 的目录由编译期定义 `RADRAY_TEST_SHADER_DIR` 提供。

## 格式化

`.clang-format` 在仓库根，Cpp 段基于 Google 改：

| 设置 | 值 |
|---|---|
| `IndentWidth` / `TabWidth` | 4，`UseTab: Never` |
| `ColumnLimit` | **0（无列宽限制）** |
| `BreakBeforeBraces` | `Attach` |
| `PointerAlignment` | `Left`（写 `Type* name`） |
| `AccessModifierOffset` | -4 |
| `SortIncludes` | false |
| `AlwaysBreakTemplateDeclarations` | Yes |

没有 `.clang-tidy`，没有 `.editorconfig`。

## 注释

`AGENTS.md` 已定基调：**代码注释只承载 API 契约，设计理由归 `docs/`**。落到实践：

- 写"调用方必须做什么"、"这里为什么不能改成显然的写法"——保留。
- 必要的设计理由与代价写进所属 architecture/guide 页面；代码文件只在文件头保留至多一个文档入口。
- 用 `【】` 标出真正会导致出错的约束，让它在扫读时跳出来。
- 改了某个文档描述的行为，同一次提交里改那个文档。
