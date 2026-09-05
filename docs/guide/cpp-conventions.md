> - 适用: 写这个仓库的 C++ 代码；不确定某个写法是否符合仓库惯例
> - 权威: 本文是命名、文件组织、接口风格、测试写法的唯一说明。硬规则（会被判定为缺陷的那些）在 `AGENTS.md`
> - 锚点: `.clang-format`, `cmake/Utility.cmake`

# C++ 约定

[AGENTS.md](../../AGENTS.md) 的硬规则必须遵守。本文补充命名、接口与测试的具体写法；
涉及硬约束的地方明确注明，其余惯例遵循所在文件的既有风格。

## 命名

| 类别 | 风格 | 例 |
|---|---|---|
| 类型（class / struct / enum） | PascalCase | `RenderPipeline`, `GpuSystem`, `ScopeGuard` |
| 函数与方法（含自由函数） | PascalCase | `GetWorld`, `BeginFrameRecord`, `PerspectiveLH` |
| 私有成员变量 | `_` 前缀 + camelCase | `_activePasses`, `_renderStateCreated` |
| 公开数据成员（descriptor / POD） | PascalCase | `BufferDescriptor::Usage`, `GpuMesh::DrawData::VertexBuffers` |
| `constexpr` 常量 | `k` 前缀 + PascalCase | `kShaderAssetFormatVersion`, `kMaxPointLights` |
| 宏 | ALL_CAPS | `RADRAY_ABORT`, `RADRAY_FORWARD_OBJECT_GROUP` |
| 命名空间 | 全小写 | `radray`, `radray::render`, `radray::detail` |

**没有 `m_` 前缀**，一律 `_`。公开数据成员用 PascalCase 是刻意的：它让 descriptor struct
的字段读起来像"属性"而非局部变量。

枚举成员在**新代码里 PascalCase**（`RenderQueue::Geometry`、`LightType::Point`），
但 core 的旧代码是 ALL_CAPS（`VertexDataType::FLOAT`、`VertexSemantics::POSITION`）。
**不要为统一风格去改它们**——`AGENTS.md` 禁止重命名枚举成员，`magic_enum` 与序列化数据在消费
这些名字。

文件内辅助函数有两派并存，**以文件为单位自洽**：render/shader 后端用 `static` + `_` 前缀
（`_CreateVkSwapChain`），runtime/core 用匿名 namespace + 无前缀。改哪个文件就跟那个文件。

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

`.cpp` 内部实现放匿名 namespace（runtime / core）或文件级 `static`（render / shader 后端），
见上文命名一节。

## 接口风格

**可空指针用 `Nullable<T>`，可空值用 `std::optional`。** 这条分界在仓库里执行得很干净：

```cpp
Nullable<unique_ptr<Buffer>> CreateBuffer(...);   // 指针类，可能没有
std::optional<AppFrameTarget> AcquireWindow(...); // 值类，可能没有
```

`Nullable` 的 API 是 `HasValue()` / `Get()` / `Release()` / `Unwrap()` / `operator bool`。
**没有 `Value()`**。`Unwrap()` 会在为空时抛 `NullableAccessException`——它是基础设施的误用检测，
不是业务错误处理手段。

**裸指针意味着非空**（`AGENTS.md` 硬规则）。想表达"可能为空"就用 `Nullable`。

`bool` + out 参数用在"填充一个已有对象"的场合（`GetLightRenderParameters(out)`），
不用来代替返回值。

**错误处理的主流做法是：`RADRAY_ERR_LOG` + 返回空/false。** 不可恢复的不变量违反用
`RADRAY_ABORT`。业务代码几乎不用异常——异常只出现在三类边界：第三方 C/C++ 库
（例如 libpng）、平台 API、以及需要处理可恢复异常的 C 回调。新增 `try`/`catch`/`throw`
**要先问用户**（`AGENTS.md`）。

`noexcept` 用得很密：访问器、setter、析构一律标。虚接口批量标（`rhi.h` 大部分纯虚方法）。
但**不要为了保住 `noexcept` 而加 try/catch**——那是硬规则禁止的。

## 构造与析构

**RHI 与 window 层用「接口 + 静态 Create 工厂」**，返回 `Nullable<unique_ptr/shared_ptr>`：

```cpp
static Nullable<shared_ptr<Device>> Device::Create(const DeviceDescriptor& desc);
```

**runtime 层相反**，公有构造 + `make_unique`（`GpuSystem`、`RenderSystem`、`AssetManager`、
`World`）。区别的理由是前者可能创建失败，后者不会。

**两段式析构（`Destroy()` + `DestroyImpl()`）集中在 render 后端实现类**，理由见
[RHI 所有权](../architecture/render-rhi.md)。runtime 层不用它，靠析构函数 + 成员声明顺序。

RAII 包装类的后缀是 `Scope` / `Scoped` / `Guard`，**没有 `*RAII`**：`ScopeGuard`、`TaskScope`、
`ScopedBufferMap`、`FrameUploadScope`。（`d3d12_impl.h` 内部的 `DescriptorHeapViewRAII`
是后端私有的例外。）

**成员声明顺序常常有语义**，因为析构是逆序的。这类地方都有注释标出，不要"顺手整理"。

## 测试

测试源码放 `modules/<module>/tests/test_<topic>.cpp`，目标名 = 文件名。

```cmake
radray_add_test(test_foo SOURCES test_foo.cpp LINK_LIBS radrayruntime)
```

`radray_add_test` 建独立可执行目标，链接 `GTest::gtest_main`，并用
`gtest_discover_tests(... DISCOVERY_MODE PRE_TEST)` 注册。那个 `PRE_TEST` 是硬约束，
不要改，原因与验证方法见[构建与测试](build-test.md)。

`radray_add_radray_gtest_case` 是在**已有**目标上按 `--gtest_filter` 注册单个 ctest 用例，
并自动注入 `RADRAY_PROJECT_DIR` / `RADRAY_ASSETS_DIR` 等环境变量。目前仓库里还没有调用方。

**`ctest -R` 匹配 gtest 套件名（C++ 类名），不是 cmake 目标名。** 对照表在
[build-test](build-test.md)。

**需要 GPU 的测试无条件注册，用例内 `GTEST_SKIP()`**，不做条件 CMake 注册：

```cpp
auto device = TryCreateDevice(...);
if (!device) {
    GTEST_SKIP() << "the render backend is unavailable on this machine";
}
```

这样在有设备的机器上一定会跑，在 CI 上也不会假绿。

现成的测试辅助设施：

| 设施 | 在哪 | 用途 |
|---|---|---|
| `ManualGate` | `runtime/tests/test_asset_slot.cpp` | 手工协程闸门。`async_scope::spawn` 会同步跑完只有 `co_return` 的 task，所以要挂住协程必须有它。写 `co_await gate.Wait()`，直接 `co_await gate` 会拷贝它 |
| `ProbeAsset` + `DestroyGuard` | 同上 | 不碰 GPU 的假资产，计数 `OnUnload` 与析构 |
| `Scoped*`（`ScopedDirectory` 等） | 各测试文件 | 临时目录 / 临时输入 |

需要仓库根路径的测试从 `RADRAY_PROJECT_DIR` 环境变量取，编译期有
`RADRAY_PROJECT_DIR_DEFAULT` 兜底。两者都要，因为 ctest 注册的用例走环境变量，
而直接跑可执行文件时没有。

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
