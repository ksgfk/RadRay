> - 适用: 评估 RadRay runtime JIT 的 include source 传递、shaderlib include root 与 RadRay DXC fork 的职责边界
> - 权威: 本文是基于指定 RadRay 工作树与 DirectXShaderCompiler 源码快照的一次性研究记录，不改变当前 shader contract
> - 锚点: `modules/shader/include/radray/shader/shader_compiler_contract.h`, `modules/shader/src/shader_compiler_contract.cpp`, `modules/shader_compiler/src/client.cpp`, `modules/runtime/include/radray/runtime/shader_jit.h`, `docs/architecture/shader-pipeline.md`, `docs/architecture/shaderlib.md`, `F:\cpp\DirectXShaderCompiler\include\dxc\dxcapi.h`, `F:\cpp\DirectXShaderCompiler\include\dxc\dxcapi_radrayext.h`, `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcradray.cpp`, `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxclibrary.cpp`, `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcompilerobj.cpp`

# `IncludeSource` 与 DXC include 边界研究

## 版本与范围

RadRay 工作树基线为 `815a13addd68a693a52fc3802b525e1db39fc960`，调查时工作树包含本样例的未提交改动。
指定的 DirectXShaderCompiler checkout 为 `ac485a95ef24e9ee3cc167bc632fed8e8e940156`，提交信息为
`Rename RadRay DXC package identity`，时间为 `2026-08-08T14:21:44+08:00`。调查只读了上述
RadRay 文档、RadRay shader wire/client 实现、RadRay DXC extension ABI，以及 DXC 原生
`IDxcIncludeHandler`、`-I` 和编译调用链。

## 结论

`vector<IncludeSource>` 不是 DXC 原生编译器“不能使用 include directory”的证明。普通 DXC
支持两种相关机制：命令行 `-I`，以及 `IDxcIncludeHandler::LoadSource`；默认 include handler
会从 filesystem 读取候选文件。

但 RadRay 使用的不是普通 `IDxcCompiler3` 入口，而是 fork 自有的
`IRadRayDxcCompiler::CompileVariant`。这个 ABI 只接收一个不透明的
`RadRayDxcBlobView` wire request，没有物理 include root、工作目录或 callback 参数。
wire request 内部明确包含 root source 和 include name/content 列表。fork 收到请求后自行
按逻辑名称展开 include，再把展开后的单一 source buffer 传给普通 DXC，并传入空的
`IDxcIncludeHandler`。

因此准确的因果关系是：

```text
DXC 原生能力              可以从目录或 IncludeHandler 读文件
        |
RadRay CompileVariant ABI  只接受可序列化的 source/content wire request
        |
RadRay DXC fork            自己按 IncludeSource 做 typed expansion
        |
普通 IDxcCompiler3         收到已展开 source，IncludeHandler = nullptr
```

`IncludeSource` 是 RadRay 为了让一次 JIT request 成为自包含、可验证、可复现的编译输入而
选择的 transport representation；它不是 DXC API 的硬性限制。

## RadRay 侧证据

### request 没有物理 include path

`shader::IncludeSource` 只有 `LogicalName` 与 `Content`；`CompileVariantRequest` 只有
`SourceName`、`RootSource`、`Includes`，没有 `IncludeRoot` 或 filesystem callback：

- `modules/shader/include/radray/shader/shader_compiler_contract.h:248-275`

`CanonicalizeCompileVariantRequest` 只验证 source/include 是 root-relative logical name；
`EncodeCanonicalCompileVariantRequest` 随后把 source、每个 include 的 logical name 和 bytes
一起编码进 wire request：

- `modules/shader/src/shader_compiler_contract.cpp:88-100`
- `modules/shader/src/shader_compiler_contract.cpp:156-175`

`radrayshadercompiler` client 只做 canonicalize、encode，然后调用 fork 的
`CompileVariant`，没有向 DXC 传入物理路径：

- `modules/shader_compiler/src/client.cpp:265-282`

这也解释了文档中“物理仓库路径不属于 source identity”的约束；`shaderlib` 是逻辑 include
根，不是传入 wire ABI 的绝对路径：

- `docs/architecture/shader-pipeline.md:31-38`
- `docs/architecture/shaderlib.md:7-10`

### fork 自己展开 include

RadRay fork 在 `dxcradray.cpp` 中定义了与 wire request 对应的内部 `IncludeSource`，读取
include count、logical name 和 bytes，并拒绝非法逻辑名：

- `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcradray.cpp:208-216`
- `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcradray.cpp:257-298`

`ExpandSourceInternal` 会扫描 active conditional block 中的 `#include`，按逻辑名在
`request.Includes` 中查找 bytes，递归展开，并按 target 分别执行：

- `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcradray.cpp:1753-1848`
- `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcradray.cpp:2533-2543`

随后 `CompileStage` 将展开后的字符串放进 `DxcBuffer`，调用 `IDxcCompiler3::Compile` 时
第四个参数为 `nullptr`，即没有使用 DXC 的 include handler：

- `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcradray.cpp:1871-1913`

因此，在当前 fork 实现中，直接把 `shaderlib` 作为 `-I` 传给底层 DXC 并不能替代
`IncludeSource`：fork 在调用底层 DXC 前已经要求 include 展开完成，且底层调用根本没有
include handler。

### include 内容参与 compile input identity

fork 只把实际展开到的 `opened` include 写入 `EncodeCompileInput`，再计算 compile input hash：

- `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcradray.cpp:2024-2060`
- `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcradray.cpp:2537-2565`

这使得同一个逻辑 source 在 shaderlib 内容发生变化时得到不同的 compile input identity，且
不会把开发机绝对路径、当前工作目录或机器上的目录布局作为 ABI 输入。

## DXC 原生侧证据

DXC 的公开接口明确提供了 filesystem include handler：

- `F:\cpp\DirectXShaderCompiler\include\dxc\dxcapi.h:224-242` 定义
  `IDxcIncludeHandler::LoadSource`，并说明默认实现从 filesystem 读取 include 文件。
- `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxclibrary.cpp:39-68`
  的 `DxcIncludeHandlerForFS::LoadSource` 调用 `DxcCreateBlobFromFile`。
- `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxclibrary.cpp:311-319`
  创建默认 filesystem handler；`DxcLibrary::CreateIncludeHandler` 在
  `:575-576` 转发到它。

DXC 的 `-I` 也是真实的 include directory 机制：`dxcompilerobj.cpp` 把每个 `OPT_I` 参数
加入 `clang::HeaderSearchOptions` 的 angled include search path：

- `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcompilerobj.cpp:1491-1505`

所以“DXC 可以直接使用 `shaderlib` 路径”在普通 DXC 调用模型中成立；它只不适用于当前
RadRay fork 的 typed `CompileVariant` ABI，因为该 ABI 在更上层已经选择了 content-closed
include expansion。

## 为什么 contract discovery 不需要 IncludeSource

当前 `DiscoverSourceContract` 只发现 root source 的 entry topology 和 keyword domain，fork
也只从 discovery request 读取 source name、root source 和 target。include 中不能声明
keyword group，entry topology 由 root `.hlsl` 定义，因此 discovery 阶段无需展开 shaderlib：

- `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcradray.cpp:2460-2490`
- `docs/architecture/shader-pipeline.md:22-33`

真正的 `CompileVariant` 才需要 `IncludeSource`，因为它必须生成目标代码，并且要让 active
include 内容进入 compile input hash。

## 对样例实现的判断

当前样例中的 `IncludeCollector` 是对底层契约的适配，不是 DXC 本身的必要步骤：

- `examples/example_lambert_sphere/example_lambert_sphere.cpp:121-210`
- `examples/example_lambert_sphere/example_lambert_sphere.cpp:383-410`

它把物理 `shaderlib` 目录转换成 RadRay fork 所需的逻辑文件表。这个转换应该是 runtime/JIT
的可复用 source resolver 责任；example 不应重复实现 include 解析、安全检查和文件读取。

不过，resolver 的输出最终仍然必须是 `vector<IncludeSource>`，除非同时改变 RadRay fork ABI
和其 `CompileVariant` 实现，使其重新接受物理 include root 或 `IDxcIncludeHandler`。仅在样例
里增加 `-I shaderlib` 不会改变当前 fork 的行为。

## 最终判断

- “为什么需要 `vector<IncludeSource>`？”因为 RadRay 的 fork 编译 ABI 是 content-closed
  request，fork 自己依赖这些 bytes 做 typed expansion 和 compile identity，而不是因为 DXC
  不支持 include path。
- “为什么需要用户自己整理？”当前 `ShaderJit` API 暴露的是底层 request，没有暴露通用
  `shaderlib` resolver；这是 RadRay runtime API 的缺口，不是 shader authoring 的合理负担。
- 推荐保持 `IncludeSource` 作为 compiler wire 层的内部输入，同时把物理 root 到 logical
  source map 的实现下沉到 runtime/JIT 的通用 resolver。不要把绝对路径直接写进 source identity
  或 wire contract；如要恢复物理路径加载，必须重新设计 fork ABI、include expansion 和
  compile input identity。
