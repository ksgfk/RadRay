> - 适用: 核对 `dynamic_cast`、`typeid`、`std::type_index` 及编译器 RTTI 开关的标准与工具链依据
> - 权威: 2026-09-05 的一次官方来源快照，不定义 RadRay 当前契约；当前契约见 architecture 文档与 ADR-0055
> - 锚点: `modules/core/include/radray/runtime_type.h`, `modules/core/CMakeLists.txt`, `modules/runtime/include/radray/runtime/asset_manager.h`, `modules/runtime/include/radray/runtime/asset_database.h`, `modules/runtime/include/radray/runtime/game_framework/actor.h`, `modules/runtime/include/radray/runtime/service_registry.h`

# C++ RTTI 运行时对象查询证据

## 调查范围与来源版本

本报告只回答四个问题：标准指针 `dynamic_cast` 能表达哪些对象关系，`typeid` 何时得到最派生
动态类型，`std::type_index` 是否适合作为进程内容器键，以及 RadRay 支持的前端怎样显式开启 RTTI。

- WG21 editor draft 仓库 `cplusplus/draft`：2026-09-05 查询 `main` 为 commit
  [`51de607bd0264b2e9fb9a853a93d83b7390ca040`](https://github.com/cplusplus/draft/tree/51de607bd0264b2e9fb9a853a93d83b7390ca040)。下文段落号使用同日可读的
  [eel.is draft `dynamic_cast`](https://eel.is/c++draft/expr.dynamic.cast)、
  [`typeid`](https://eel.is/c++draft/expr.typeid) 与
  [`type_index`](https://eel.is/c++draft/type.index) 页面；该渲染快照链接到 commit
  `c7015b485cc3db8efaa9dfb9ff0809c5394a4ed1` 的 editor source。
- Microsoft C++ 文档：`MicrosoftDocs/cpp-docs` 2026-09-05 `main` 为 commit
  [`6e1f010ece41dfca93d4aecce54004837f9311a5`](https://github.com/MicrosoftDocs/cpp-docs/blob/6e1f010ece41dfca93d4aecce54004837f9311a5/docs/build/reference/gr-enable-run-time-type-information.md)；
  [/GR 页面](https://learn.microsoft.com/en-us/cpp/build/reference/gr-enable-run-time-type-information?view=msvc-170)
  标注最后更新 2021-08-03。
- Clang 文档：2026-09-05 访问的
  [Clang 24.0.0git command-line reference](https://clang.llvm.org/docs/ClangCommandLineReference.html#cmdoption-clang-frtti)。
  本次 Windows 验证实际使用 ClangCL 22.1.3、MSVC-compatible frontend 19.51。

## 标准机制能覆盖目标关系

[`[expr.dynamic.cast]`](https://eel.is/c++draft/expr.dynamic.cast) 要求目标是完整类类型的指针或
引用；普通运行期检查要求 source 是多态类型。其运行期规则在最派生对象中寻找公开、无歧义且
唯一的目标子对象，因此同一机制覆盖向下转换、基类查询、接口横向转换、虚继承以及多继承下的
正确子对象地址调整。指针形式转换失败时结果是目标类型的 null pointer；引用形式失败会抛出
`std::bad_cast`。所以 RadRay 的可空查询采用指针形式，并以 `Nullable` 暴露结果。

这也说明人工 GUID 继承图不是等价替代品：GUID 相等或图可达只能回答声明关系，不能独自给出
最派生对象中目标子对象的调整后地址。

## 精确动态类型必须在非空对象上查询

[`[expr.typeid]`](https://eel.is/c++draft/expr.typeid) 规定：对多态类 glvalue 使用 `typeid` 时，结果
表示它所指最派生对象的动态类型。类型操作数必须完整；对形如 `typeid(*p)` 的空指针求值会抛出
`std::bad_typeid`。因此 RadRay 先通过 `Nullable` 确认对象存在，再对 `*pointer` 使用 `typeid`；
不通过新 catch 路径把空对象转换为普通失败。

## `std::type_index` 是进程内服务键

[`[type.index]`](https://eel.is/c++draft/type.index) 明确把 `std::type_index` 定义为 `type_info` 的
轻量包装，可作为有序或无序关联容器的索引，并提供相应 hash。它适合 `ServiceRegistry` 的进程内
精确静态类型键，但标准没有把名字或 hash 规定为跨构建持久标识。因此服务表使用
`std::type_index(typeid(T))`，稳定协议仍单独使用 GUID，二者不建立全局映射。

## 构建开关必须沿静态库边界一致

Microsoft 的 [/GR 文档](https://learn.microsoft.com/en-us/cpp/build/reference/gr-enable-run-time-type-information?view=msvc-170)
说明 `/GR` 生成运行期类型检查信息、定义 `_CPPRTTI`，而 `/GR-` 关闭 RTTI；使用
`dynamic_cast` 或 `typeid` 通常需要该选项。Clang 24.0.0git 的命令行参考列出
`-frtti` / `-fno-rtti` 开关。

RadRay 因而把 `/GR`（MSVC 与 ClangCL）或 `-frtti`（其他前端）放在 `radraycore` 的 PUBLIC
CMake usage requirement 上。本次生成的 ClangCL Debug 工程在 `radraycore` 和最终
`test_runtime_type` 目标上都产生 `RuntimeTypeInfo=true`；测试同时检查 `_CPPRTTI` 或
`__cpp_rtti`。这不是性能优化提示，而是所有产生和消费多态对象的目标必须一致遵守的 ABI 前提。

上段记录重构初版的构建验证。随后 [ADR-0056](../adr/0056-rtti-belongs-to-project-compile-defaults.md)
将选项归属调整为 `radray_default_compile_flags` 对自有 target 的 PRIVATE 设置；core PUBLIC
传播不再是当前配置。标准转换语义与跨静态库验证范围不变。

## 对 RadRay 设计的边界

- RTTI 只负责活对象的 C++ 关系和进程内类型身份，不替代资产 `AssetId`、manifest type 字符串、
  shader wire 标识或其他持久协议。
- 正确性来自标准转换语义和一致构建开关，不假设 LTO 会消除 `dynamic_cast`。
- 可查询目标可以是无 GUID 的完整接口类；创建资产、创建组件仍由领域基类约束。
- 本报告没有论证跨动态库插件 ABI。当前验证边界是 RadRay 静态库与最终 executable；若将来新增
  独立编译插件，需要单独固定编译器、标准库、visibility 和加载边界。
