> - 适用: 维护 shader compiler client、metadata wire、artifact decoder 或 runtime JIT
> - 权威: 本文描述当前 RadRay shader pipeline 的实现边界；第一阶段检查站见实施计划
> - 锚点: `modules/shader/include/radray/shader/shader_compiler_contract.h`, `modules/shader/include/radray/shader/shader_artifact.h`, `modules/render/include/radray/render/backend_shader_artifact.h`, `modules/render/src/backend_shader_artifact.cpp`, `modules/shader_compiler/include/radray/shader_compiler/client.h`, `modules/runtime/include/radray/runtime/shader_jit.h`, `modules/runtime/include/radray/runtime/shader_program.h`, `modules/runtime/include/radray/runtime/shader_parameters.h`, `CMakePresets.json`

# Shader pipeline

## 当前状态

HLSL 是 pass source 的唯一 authoring 输入。当前仓库已具备 source contract discovery、typed
`CompileVariant`、DXIL/SPIR-V 双 lane、compiler-free artifact decoder、target-native layout
入口和 runtime JIT 垂直切片。

正式 SDK 是独立构建并安装的 RadRay DXC fork package。`radrayshadercompiler` 只通过
`RadRayDXC::Headers` 编译、以 canonical library name 加载 `RadRayDXC::Compiler` 的 runtime，
再通过 `dxcapi_radrayext.h` 的 CLSID、ABI/schema 和 toolchain identity handshake 创建 fork
extension。`RadRayShaderCompilerClient` 测试覆盖 fork ABI/package handshake；非 fork development
mode 没有 upstream fallback，只会在 extension 能力上 fail closed。

## Source contract

根 `.hlsl` 文件定义一个 source unit：

- graphics 必须有一个 vertex entry，可以有一个 pixel entry；depth-only 不需要 pixel。
- compute 只能有一个 compute entry；一个 source unit 不能混合 graphics 与 compute。
- entry 使用标准 `[shader("vertex")]`、`[shader("pixel")]` 或 `[shader("compute")]`。
- `#pragma radray_keyword_group` 只能出现在根 `.hlsl`，不能由 `.hlsli` 隐式扩张 domain。
- `CompileVariantRequest` 的每个 keyword group 必须有一个 assignment；名称和值都由 discovery
  返回的 domain 校验。普通 `Defines` 与 keyword assignments 是两个独立的 typed channel。

`DiscoverSourceContract` 输出 kind、entry topology、keyword domain 和 `ContractHash`。caller
只把 shaderlib-root-relative 的逻辑 `SourceName` 传给 compiler；物理仓库路径不属于 source
identity。`CompileVariant` 会重新 discovery 并拒绝 `ExpectedContract` 漂移。

## Target lanes

一次 concrete request 可以请求 DXIL、SPIR-V 或两者。每个 lane 独立通过 DXC default filesystem
include handler 按调用方提供的 ordered `-I` 路径读取 include、编译 stage、生成 bytecode 和
metadata；任一 requested lane 失败，整个 batch 失败且不发布成功 lane。

DXIL 与 SPIR-V 的 binding 数字不要求相等。DXIL 使用 `register`/`space` 语义，SPIR-V 使用
`VK_BINDING(binding, set)` 展开的标准属性；`shaderlib/core/platform.hlsli` 只负责 target gate，
不分配 RadRay 自己的编号。DXIL 的 `t0` 与 `s0` 可以共享数字坐标，但属于不同 register
namespace；decoder 的 duplicate policy 与 backend lookup 都保留这个 namespace。`VK_LOCATION`
与 `VK_PUSH_CONSTANT` 遵守相同规则。

HLSL `[RootSignature(...)]` 是可选的 DXIL-only authoring policy。相关 entry 全部未声明时，DXIL
artifact 的 `RootSignature` range 为空，D3D12 根据 active binding metadata 走自己的 implicit
layout generator；DXC 不生成这条 fallback。任一相关 entry 声明后，DXC 必须输出有效的 serialized
Root Signature；graphics 的非空 stage outputs 必须逐字节相同，且 explicit signature 覆盖所有
active resources，允许保留当前 variant 未使用的合法 superset parameters/ranges/static samplers。
Malformed、跨 stage 不一致或未覆盖 active resource 都是 compile failure，不会悄悄回退到
implicit；D3D12 backend 不支持的合法 RS 形状则在 explicit layout 创建时 fail closed，同样不
回退到 implicit。

## Artifact wire

每个 lane 返回独立 bytecode 与 compiler-owned metadata blob。metadata envelope 固定 magic、
schema、target、toolchain identity、contract、bytecode/layout/gpu artifact hashes 和各 payload range；payload 记录
entry、active binding、type tree、root/push constants、可选 DXIL serialized Root Signature 与
bytecode range。Explicit range 原样保存 DXC 的 `DXC_OUT_ROOT_SIGNATURE` carrier；最终 DXIL
object 通过 `-Qstrip_rootsignature` 去掉嵌入的 `RTS0`，artifact 只保留这一份 lane-level carrier。
SPIR-V 的 Root Signature range 始终为空。

`radrayshader` 只做 wire safety 检查和 target-specific decode；它不链接 DXC，也不依赖
`radrayrender`。调用方提供独立可信的 `ExpectedGpuArtifact`，decoder 与 envelope 比较但不在
render/runtime 侧重算 hash。DXIL view 只能送入 D3D12 layout 入口，SPIR-V view 只能送入
Vulkan layout 入口；两入口先把 compiler-owned records 组装成 backend-specific layout input，
再创建原生 layout。运行时已选定 device 的 caller 使用 `CreateBackendShaderArtifact`：显式 target
必须与 device backend 一致，typed decoder 还会核对 envelope target；失败不切换 lane。该桥持有
decoded artifact bytes 与原生 layout，stage bytecode 通过 `ShaderArtifactView::FindStageBytecode`
取得。`BindingHandle` 由公共 `PipelineLayout::FindBinding` 在当前 artifact layout 上生成，内含 generation 与 DXIL
register namespace，不能跨 target、Variant 或 recompile 复用；unknown/inactive name 与跨
layout handle 写入都 fail closed，Debug 下跨 layout 使用还会触发断言。

type tree 属于所属 artifact 的 CPU upload schema。当前 v3 record 保留 scalar/vector/matrix 的
kind，并为 struct/struct-array member 携带 compiler-owned underlying `TypeIndex`；decoder 检查
range、已知 record kind、element count、offset/size/stride、type reference、父结构范围、同级
名称和父链环路的 wire 安全性。它没有独立 schema hash，也不参与 GPU artifact identity。
runtime 的 `ShaderParameterLayout` 是第一个生产消费者：它把各 cbuffer 根结构展开为扁平的
`参数名 → binding/offset/kind/size/stride/count` 索引，struct `Member` 与 struct array 通过
`TypeIndex` 递归展开。program 内重名、未知复合类型或不安全 offset 使创建失败；
`ShaderParameterStorage` 的 typed setter 在 kind/size/element 不匹配时不修改目标 bytes。

非 struct 元素的数组（`float4 Foo[4]`）是 type tree 的表达上限：`TypeIndex` 只能指向根 struct，
所以这类 record 只带 stride 与 count，元素 kind 与元素尺寸都不在 wire 里。layout 把它记为
`ShaderParameterKind::Raw`——每槽范围已知、类型未知，只接受 `SetRaw` 的原始字节，所有 typed
setter 拒绝它，而 typed 参数同样拒绝 `SetRaw`。cbuffer binding 与 cbuffer 根类型按**发射位置**
配对（`WireBindingRecord` 不带 `TypeIndex`，binding 名是变量名而根类型名是结构名，无法按名配），
依赖 compiler 按声明顺序发射两个序列；`multiple_cbuffers` fixture 双 target 钉住这条顺序。

`CreateBackendShaderArtifact` 还接收 pipeline 提供的 `ShaderLayoutPolicy`。policy 中列出的 group
把 wire cbuffer 映射为 `DynamicCBuffer`：D3D12 implicit layout 使用 root CBV，Vulkan 使用
`VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`。policy 引用不存在的 group，或与作者 serialized
Root Signature 同时出现时 fail closed；空 policy 仍生成普通 `CBuffer`。

## Runtime JIT

`radrayruntime` 在 `RADRAY_ENABLE_SHADER_JIT=ON`（默认开，依赖 compiler capability）时 PUBLIC
链接 `radrayshadercompiler`，`ShaderJit` 构造时接收并按值固定 ordered filesystem include path
数组，动态加载 canonical `dxcompiler` 名称，提交一个具体 target request，把返回 metadata 原样
交给 render 的动态 artifact 桥；桥核对 device/request/envelope target 后才进入 backend typed layout
builder。compiler
关闭时 `RADRAY_ENABLE_SHADER_JIT` 被 `cmake_dependent_option` 强制 OFF，`ShaderJit` 退化为
`IsAvailable()==false` 的桩。

JIT 在 request 不包含 target、contract drift、非法 assignment、损坏 metadata 或 toolchain
identity 不匹配时直接失败，不改请求去尝试另一 lane，也不调用 runtime reflection 或第二套
序列化格式。当前 `RadRayRuntimeShaderJit` 覆盖 D3D12/Vulkan graphics draw、compute dispatch
readback、fixture case report 和 metadata corruption negative path；case report 覆盖
no-resource、texture/sampler、static sampler、多个 DXIL root constants、SPIR-V push block、
target-specific binding 和 compute 七类场景。

`RenderSystem` 从 `ApplicationRuntimeDescriptor::ShaderSourceRoot/ShaderIncludePaths` 构造 JIT，
并按 `(逻辑 SourceName, 规范化 keyword assignments)` 缓存 `ShaderProgram`。一个 program 拥有
一个 concrete Variant 的 artifact/layout、stage shader、参数索引与 PSO map；失败结果也留在缓存中，
所以重复请求不会重新编译或刷日志。JIT 关闭时这条源码请求明确返回空，不影响 runtime 构造。

## Native PSO boundary

PSO builder 在调用 D3D12/Vulkan native pipeline API 前校验 `VertexInputState`：semantic、format、
buffer slot、attribute location、offset/stride 和重复声明必须自洽。失败不会把坏输入交给 native
PSO，也不会通过 compiler client 或 runtime reflection 补齐 vertex schema。runtime 的
`PrimitiveVertexLayout` 提供 geometry-owned stride/slot/format/offset，`ShaderProgram` 再与当前
artifact 的 `VertexInputs()` 合并；PSO key 由 material 状态、geometry layout/topology 和 pass
attachment facts 组成，不含 program 自己的 layout、bytecode 或指针。

## Build boundary

默认 `win-x64-debug` 即开启 compiler、JIT 与 tools（三者默认 ON）。隔离配置用主预设加 `-B`
与 `-D` 表达：关 compiler 的纯 runtime 构建（`-DRADRAY_BUILD_SHADER_COMPILER=OFF`，JIT/tools
随之强制 OFF）不发现 RadRay DXC package，构建树不部署 `dxcompiler` 或
`radrayshadercompiler`；tools-only 构建（`-DRADRAY_BUILD_TESTS=OFF -DRADRAY_BUILD_RENDER=OFF
-DRADRAY_BUILD_RUNTIME=OFF`）只编译 `radray_shader_compile`。

正式 compiler 配置按 Manifest `Name` 使用
`SDKs/radray_dxc/extracted`，以 `find_package(RadRayDXC CONFIG REQUIRED COMPONENTS
Headers Compiler)` 导入 fork；`tools/fetch_sdks.py restore` 负责按
`project_manifest.json` 完成下载、版本锁定和 SHA-256 校验，CMake 不再重复解析 manifest、
`.radray-sdk.json` 或 archive。所有 radray 二进制经 `radray_set_build_path` 落在
同一个 `${RADRAY_BUILD_PATH}/$<CONFIG>`，client 用裸名 `dxcompiler` 动态加载，因此运行库
部署集中在单一 `radray_dxc_runtime_deploy` custom target（把 `$<TARGET_FILE:RadRayDXC::Compiler>`
拷入公共输出目录），各消费 target 只通过 `radray_deploy_dxc_runtime` 建立构建依赖，不再各自
POST_BUILD 拷贝。
client handshake 进一步校验 RadRay extension 的 ABI、schema、toolchain identity；stock DXC
不会被当作兼容 compiler，`RADRAY_SHADER_COMPILER_FORK` 在 compiler 构建中恒定义。正式配置
不提供 `RADRAY_DXC_SDK_ROOT`/`RADRAY_DXC_FORK_PACKAGE_ROOT` 之类的开发 override。

`radray_shader_compile` 是 raw-lane 开发工具：从 shaderlib root 读取一个 `.hlsl`，将该 root
和可重复的 `--include-path` 按顺序传给 compiler，选择默认 keyword assignment，并写出 target
bytecode 与 metadata envelope；它不生成正式 manifest、
artifact index 或 publisher 输出，也不代表 stock DXC 已提供 RadRay extension ABI。工具目标
只链接 `radrayshadercompiler`，可用 map/import 检查确认没有反向引入 render/runtime/backend。

第一阶段没有正式 artifact publisher、索引、安装导出层或 fork SDK autobuild。runtime-only 的
compiler-free 验证消费版本控制的 raw golden bytecode/metadata fixture；这只证明 build-tree
decoder boundary，不代表正式离线发布链已完成。
