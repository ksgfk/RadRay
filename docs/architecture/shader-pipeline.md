> - 适用: 维护 shader compiler client、metadata wire、artifact decoder 或 runtime JIT
> - 权威: 本文描述 ADR-0051/schema 6 的目标 shader pipeline 契约；实施检查站见 `docs/todo/shader-layout-contract-correction.md`
> - 状态: 契约已接受、实现待迁移；当前 worktree 仍是 schema 5 / SDK `.radray.4`，不得据此声称 schema 6 已落地
> - 锚点: `modules/shader/include/radray/shader/shader_compiler_contract.h`, `modules/shader/include/radray/shader/shader_artifact.h`, `modules/render/include/radray/render/backend_shader_artifact.h`, `modules/render/src/backend_shader_artifact.cpp`, `modules/shader_compiler/include/radray/shader_compiler/client.h`, `modules/runtime/include/radray/runtime/shader_jit.h`, `modules/runtime/include/radray/runtime/shader_program.h`, `modules/runtime/include/radray/runtime/shader_parameters.h`, `CMakePresets.json`

# Shader pipeline

## 迁移状态

HLSL 是 pass source 的唯一 authoring 输入。当前仓库已具备 source contract discovery、typed
`CompileVariant`、DXIL/SPIR-V 双 lane、compiler-free artifact decoder、target-native layout
入口和 runtime JIT 垂直切片，但 layout 链仍有已确认的 schema 5 缺口：program cache 未覆盖
完整 CompilePolicy/layout identity；RootSignature 只桥接 Vulkan static-sampler 标志；sampler state
丢失；typed/structured/raw buffer 被折叠；group-wide `ShaderLayoutPolicy` 把 native placement 写回
logical kind；push handle 与 D3 explicit root-descriptor dynamic offset 链不完整。

以下章节是已经接受、实现必须收敛到的 schema 6 契约。迁移为原子 cutover：schema 6 decoder
拒绝 4/5，RadRay DXC package 升级到 `1.9.2607.radray.5`，extension ABI 因 interface shape 不变
仍为 3。完成前现有 schema 5 behavior 只算迁移起点，不是新的架构权威。

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
遵守相同 target-gate 规则。push declaration 则同时使用 DX `register` 和 `VK_PUSH_CONSTANT`，
不得再使用 `VK_BINDING`；DXC 对 push+binding 的组合直接报 authoring error。

每个 concrete Variant 先运行一次 compiler-owned **RootSignature policy frontend**，再进入
requested DXIL/SPIR-V lanes。frontend 与 lanes 共享同一 root source、ordered include paths、
structured `Defines`、canonical assignments 和完整 `CompilePolicy`，负责 graphics stage merge、
标准 D3 coverage、已关联 Vulkan declaration 的 visibility coverage 与 canonical declaration
identity 关联；runtime/cook 不执行第二次 DXIL analysis
或 layout link。

HLSL `[RootSignature(...)]` 是可选的跨 target base policy：

| policy item | DXIL/D3D12 lowering | SPIR-V/Vulkan lowering |
|---|---|---|
| descriptor table | serialized descriptor table | ordinary descriptor |
| root CBV | serialized root CBV | dynamic uniform buffer |
| root SRV/UAV buffer | serialized root SRV/UAV | dynamic storage buffer |
| RootConstants | serialized root constants | matching authored `VK_PUSH_CONSTANT` declaration |
| StaticSampler | full static sampler state | full-state immutable sampler record |

DXIL carrier 允许合法 stable superset；Vulkan 只发布当前 target 的 active declarations，对可关联
policy parameter 的 declaration 应用上表，target-only declaration 保留标准 Vulkan attribute 语义，
descriptor/push stage flags 取 actual active stages。parameter order、table grouping/offset、range/root/RS flags、
IA/deny flags 等 D3-only topology 不生成 Vulkan 伪字段。ordinary graphics/compute global RS 1.0/1.1
是支持范围；Local RS、directly-indexed heaps与多个 active Vulkan push blocks fail closed。

相关 entries 全部未声明时，DXIL artifact 的 serialized Root Signature range 为空，D3D12 按 active
metadata 生成 implicit descriptor tables；SPIR-V 使用 ordinary descriptors。compiler 不生成公共
默认 policy，也不因 SPIR-V-only request 发布隐藏 DXIL result。无 RS 不禁止后续 Vulkan dynamic
descriptor 或 D3 implicit root descriptor modifier。

## Artifact wire

每个 lane 返回独立 bytecode 与 compiler-owned metadata blob。schema 6 envelope 固定 magic、schema、
target、toolchain identity、contract、bytecode/base-layout/gpu artifact hashes 和各 payload range。
payload 记录 entry、active logical declarations、type tree、root/push facts、target base placement、
bytecode range，以及仅 DXIL 可有的 serialized Root Signature carrier。Explicit range 原样保存
DXC 的 `DXC_OUT_ROOT_SIGNATURE` carrier；最终 DXIL object 通过 `-Qstrip_rootsignature` 去掉嵌入
`RTS0`，artifact 只保留这一份 lane-level carrier。SPIR-V 的 carrier range 始终为空，但 payload
包含 RootSignature policy lower 后的 Vulkan records。

logical declaration record 区分 CBuffer、typed `Buffer<T>/RWBuffer<T>`、structured/read-write、
raw/read-write、sampled/storage texture 与 sampler。D3 Table/RootDescriptor 和 Vulkan
Regular/Dynamic 是 base/resolved placement，不是 logical kind；新 path 不生产
`ShaderParameterBindingType::Dynamic*` 来表达 artifact 资源类别。既有 enum identifiers 保留且
不重命名。

SPIR-V immutable sampler 引用 Vulkan-specific fixed-width full-state record，覆盖 filter、address、
mipmap/LOD、bias、anisotropy、compare、border 和 reduction；wire 不持久化 `VkSamplerCreateInfo` 或
`pNext`。push/root record 携带 canonical declaration identity，pure push shader 也能建立 name
lookup。compiler 的 `BasePipelineLayoutHash` 覆盖 canonical base semantics；Target layout modifier
不进入 artifact identity。

`radrayshader` 只做 wire safety 检查和 target-specific decode；它不链接 DXC，也不依赖
`radrayrender`。调用方提供独立可信的 `ExpectedGpuArtifact`，decoder 与 envelope 比较但不在
render/runtime 侧重算 compiler hashes。layout 链分三段且不可合并：

1. DXIL/SPIR-V target artifact decode；
2. render 用 current backend options resolve 成 owning、hashable `ResolvedD3D12Layout` 或
   `ResolvedVulkanLayout`；
3. backend 只从对应 resolved value 创建 native layout 与 command metadata。

DXIL view 不能送入 Vulkan resolver，SPIR-V view 也不能送入 D3 resolver。运行时动态桥核对
device/request/envelope target 后只选择匹配路径，失败不切 lane。resolved value 拥有所有 records、
strings、sampler recipes 与 dynamic order，不借用 artifact span；其 `ResolvedLayoutHash` 按
canonical resolved semantics 计算，不包含 modifier 输入顺序、显式默认值或 native handles。

`PipelineLayout::FindBinding` 按 canonical HLSL name 为 descriptor 与 push declaration 生成
`BindingHandle`。公共 handle 只暴露 validity/equality；generation、namespace、table index 及一个
或多个 native destinations 由 layout 内部 metadata table 解释。unknown/inactive selector、wrong
kind、duplicate modifier、wrong/cross-layout handle 等是 framework invariant（Debug assert），
不新增恢复 API。

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

## Target layout resolution

`ShaderProgramLayoutRecipe` 并列持有 D3D12 与 Vulkan 的 typed options；current backend 只消费/
哈希自己的字段。每个 Target layout modifier 以“canonical declaration name + expected logical
resource kind”精确选择当前 artifact 中的一项，不能按 group 或 binding number 扩张：

- D3D12 Implicit 仅允许 count=1、原生合法 buffer 在 descriptor table 与 root descriptor 间切换；
  Explicit serialized RS 是最终权威，不能改写。
- Vulkan 仅允许 uniform/storage buffer 在 Regular 与 Dynamic descriptor 间切换，或为 sampler
  指定完整 immutable state（base 已有时整体替换）。

modifier 不能改变 kind、count、group/binding、visibility 或 push range。missing/inactive selector、
kind mismatch 和 duplicate target 都是 framework 构造错误；device limit/feature、sampler capability
和 native create 失败才是 layout creation diagnostic。unsupported immutable sampler 不得 silent
downgrade。

`ResolvedVulkanLayout` 明确保有 set 0..max set（含 empty holes）、排序后的 descriptors、immutable
sampler recipes、唯一 active push block、actual stage flags 与 Vulkan dynamic-offset packing order。
native 创建顺序是 samplers -> descriptor set layouts -> `VkPipelineLayout`，backend layout 保持
sampler/set-layout 引用。`ResolvedD3D12Layout` 保存 Explicit carrier 或 Implicit canonical topology、
Table/RootDescriptor destinations、RootConstants/static samplers 与 visibility fan-out。不存在公共
native layout descriptor 或 global native layout cache。

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

`RenderSystem` 从 `ApplicationRuntimeDescriptor::ShaderSourceRoot/ShaderIncludePaths` 构造 JIT，并只
提供 `GetOrCreateShaderProgram(const ShaderProgramRequest&)`。request 显式携带 source compile input
（logical source 与 structured `Defines`）、keyword assignments、完整 `CompilePolicy` 与
`ShaderProgramLayoutRecipe`；discovery 与 compile 从同一 request 获得完整 compile inputs，不能
一侧使用 default policy。

compiler artifact key 覆盖 source input/identity、structured `Defines`、canonical assignments、完整 policy、target 和
toolchain，不含 layout recipe。program/layout key 由 artifact identity 加 current backend 的
`ResolvedLayoutHash` 组成；非 current backend recipe 字段、modifier 原始顺序和 native handles
不进入 key。失败结果按同一完整 key 缓存。一个 program 仍拥有一个 concrete Variant 的 artifact、
resolved/native layout、stage shaders、参数索引与 PSO map；不增加 global native layout cache。
JIT 关闭时这条源码请求明确返回空，不影响 runtime 构造。

command binding 延续薄公共操作，但 identity 来自 resolved layout：
`ShaderParameterDynamicOffset` 携带 `BindingHandle + Offset`。D3 root destination 提交 base GPU VA +
offset；Vulkan 按 resolved dynamic order 打包 `pDynamicOffsets`，不依赖 caller 顺序或裸 binding
number。push 写入为 `SetPushConstants(BindingHandle, bytes)`，只写 `[0,size)` prefix，要求非零、
不超过 resolved size 且 4-byte aligned，remainder 不变，不提供 destination offset。

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
