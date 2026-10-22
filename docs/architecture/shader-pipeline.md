> - 适用: 维护 shader compiler client、metadata wire、artifact decoder 或 shader compile 工具
> - 权威: 本文描述 schema 8 当前 shader pipeline 契约、编译边界与设计理由；HLSL 写法见 authoring 指南
> - 锚点: `modules/shader/include/radray/shader/shader_compiler_contract.h`, `modules/shader/include/radray/shader/shader_artifact.h`, `modules/shader_compiler/include/radray/shader_compiler/client.h`, `tools/CMakeLists.txt`, `CMakePresets.json`

# Shader pipeline

## 当前边界

HLSL 是 compiler 的 pass source authoring 输入。shader 模块定义 wire contract 与 decoder，compiler 模块
通过 RadRay DXC fork 生成独立 target lanes。render 是独立的薄 RHI，只依赖 core，接收原始 stage 字节码
和调用方显式定义的绑定布局。shader metadata 描述编译结果，可供上层工具使用，不再决定 RHI 布局。
布局与参数 ABI、变体之间的稳定槽位、原生兼容性及共享策略由调用方负责，见 [RHI 绑定模型](render-rhi.md#绑定模型)。

以下章节既是接受的 schema 8 契约，也是当前实现。cutover 是原子的：schema 8 decoder 拒绝 4/5/6/7，
RadRay DXC package 为 `1.9.2609.radray.1`（基于 upstream `v1.9.2609`），extension ABI 为 5，
toolchain identity 为 `0x0000000001090214`。`project_manifest.json` 按 `EnforceHash` 固定正式发布归档的 hash。

正式 SDK 是独立构建并安装的 RadRay DXC fork package。`radrayshadercompiler` 只通过
`RadRayDXC::Headers` 编译、以 canonical library name 加载 `RadRayDXC::Compiler` 的 runtime，
再通过 `dxcapi_radrayext.h` 的 CLSID、ABI/schema 和 toolchain identity handshake 创建 fork
extension。`RadRayShaderCompilerClient` 测试覆盖 fork ABI/package handshake；非 fork development
mode 没有 upstream fallback，只会在 extension 能力上 fail closed。

## 诊断结果

请求规范化、请求编码、source discovery、contract/artifact 解码和 variant 编译统一返回
`shader::DiagnosticResult<T>`，定义在 `modules/shader/include/radray/shader/diagnostic_result.h`。
`Value` 是 `std::optional<T>`，`Succeeded()` 只检查它是否存在；`Diagnostics` 是拥有内容的单个
字符串。失败不返回部分结果，且必须提供非空诊断；成功可以携带 warning。调用方检查成功后从
`Value` 读取或移动产物，直接展示 `Diagnostics`，不依赖诊断文本做错误分类。

`DiscoveryResult` 是 `DiagnosticResult<ShaderContract>` 的别名，`CompileVariantResult` 是
`DiagnosticResult<vector<CompileTargetLane>>` 的别名。编译状态枚举、诊断数字编号和解码错误输出
参数已退出公共 C++ 接口；DXC 的 HRESULT 与 extension status 只在 client 适配边界读取，SDK ABI
和 artifact wire 不变。多个诊断按原顺序换行拼接，保留 SDK 提供的正文、文件名和行号；client
补充操作/source 上下文。SDK 未发布的 warning 不由客户端补造，读取 SDK 诊断失败也视为操作失败。

解码器在首次失败处返回检查项及安全可读的记录索引或布局数值。typed decoder 和 render artifact
包装层传递原始诊断，不以空值或笼统错误覆盖；底层 native 创建的详细日志仍遵循 RHI 的诊断规则。
这些构造诊断、分配产物的接口不声明 `noexcept`，不捕获分配异常来伪装业务错误。

## Source contract

Pass 是同一 source unit 的完整 graphics 或 compute 程序，Stage 是其中的可编程阶段。
Variant 是一组 target-independent keyword assignments；Compiled Variant artifact 则属于
一个具体 target。资产层的 PassName/AssetId 由调用方映射到 artifact，不进入 compiler contract。

根 `.hlsl` 文件定义一个 source unit：

- graphics 必须有一个 vertex entry，可以有一个 pixel entry；depth-only 不需要 pixel。
- compute 只能有一个 compute entry；一个 source unit 不能混合 graphics 与 compute。
- entry 使用标准 `[shader("vertex")]`、`[shader("pixel")]` 或 `[shader("compute")]`。
- `#pragma radray_keyword_group` 只能出现在根 `.hlsl`，不能由 `.hlsli` 隐式扩张 domain。
- `CompileVariantRequest` 的每个 keyword group 必须有一个 assignment；名称和值都由 discovery
  返回的 domain 校验。普通 `Defines` 与 keyword assignments 是两个独立的 typed channel。

`DiscoverSourceContract` 输出 kind、entry topology、keyword domain 和 `ContractHash`。caller
把相对调用方 source root 的逻辑 `SourceName` 传给 compiler；它可与 `shaderlib/` include root 不同，
不使用物理绝对路径作 source identity。`CompileVariant` 会重新 discovery 并拒绝 `ExpectedContract` 漂移。

entry topology 在合法 keyword domain 内保持不变；keyword 可以改变资源使用和函数体，不能
让 entry 声明只在部分 Variant 出现。discovery 与 compile 共享 Defines、CompilePolicy 和
include 配置，语义由 Clang/DXC frontend 校验，不再建立独立的 source/metadata scanner。

Root source 以调用方提供的内存 bytes 编译，`SourceName` 是诊断与 virtual main-file context，
不是从磁盘重读 root 的位置。共享 include 才由每次 invocation 的 DXC default handler 按
有序路径搜索；同名 include 由先命中的目录提供。路径列表独立于 shader request/identity，
ABI 借用显式长度的 UTF-8 view，允许空列表，拒绝空 path 或嵌入 NUL。
caller/build system 在一次 discovery、compile 或 multi-target batch 读取期间保持 include tree
不变；编译器不建立跨调用 snapshot。include 内容变化后的缓存失效由调用方管理。

## Target lanes

一次 concrete request 可以请求 DXIL、SPIR-V 或两者。每个 lane 独立通过 DXC default filesystem
include handler 按调用方提供的 ordered `-I` 路径读取 include、编译 stage、生成 bytecode 和
metadata；任一 requested lane 失败，整个 batch 失败且不发布成功 lane。

正式 SPIR-V 路径以 Vulkan 1.2 为目标，不传 `-fspv-reflect`。资源、类型与顶点输入语义等
metadata 由 fork 的编译器内部 observer 独立收集，不依赖 GPU 字节码中的 GOOGLE 反射装饰。
RHI 不启用 `VK_GOOGLE_hlsl_functionality1` 或 `VK_GOOGLE_user_type`；提交的 shader 不得声明
对应 `SPV_GOOGLE_*` 扩展。旧版或外部产物应重新编译且不使用 `-fspv-reflect`，或在离线提取
所需 metadata 后用 `spirv-opt --strip-nonsemantic` 清理，再按 Vulkan 1.2 校验并重新计算产物哈希。
该清理也会移除其他非语义调试信息，不能无条件用于需要这些信息的调试产物。
RHI 原样提交字节码，不做运行时 shader 改写；当前工具链与 wire ABI 不因这项设备策略改变。

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
identity 关联；artifact 消费方不执行第二次 DXIL analysis
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
Implicit D3 table 拓扑由 resolved bindings 的 heap 类型和原生 visibility 确定性派生；显式 carrier
仍是原生 Root Signature 的唯一权威。分组、CPU mirror 与发布规则见 [RHI 与后端](render-rhi.md#描述符分配)，
不新增 artifact wire 字段或另一套 resolved layout hash。

## Artifact wire

每个 lane 返回独立 bytecode 与 compiler-owned metadata blob。schema 8 envelope 固定 magic、schema、
target、toolchain identity、contract、bytecode/base-layout/gpu artifact hashes 和各 payload range。
payload 记录 entry、active logical declarations、type tree、root/push facts、target base placement、
bytecode range，以及仅 DXIL 可有的 serialized Root Signature carrier。Explicit range 原样保存
DXC 的 `DXC_OUT_ROOT_SIGNATURE` carrier；最终 DXIL object 通过 `-Qstrip_rootsignature` 去掉嵌入
`RTS0`，artifact 只保留这一份 lane-level carrier。SPIR-V 的 carrier range 始终为空，但 payload
包含 RootSignature policy lower 后的 Vulkan records。

logical declaration record 区分 CBuffer、typed `Buffer<T>/RWBuffer<T>`、structured/read-write、
raw/read-write、sampled/storage texture 与 sampler。D3 Table/RootDescriptor 和 Vulkan
Regular/Dynamic 是编译结果的 base placement，不是 logical kind。每个 active CBuffer 还携带
lane-local `TypeIndex`，显式指向当前 artifact type table 的顶层 payload `Struct`；多个 declaration
可以共享同一 root，一个被 declaration 拥有的 root 也可以被另一 root 嵌套引用。非 CBuffer binding
必须写 `kShaderNoType`。root constant 在 lane 发布 live payload tree 时携带同类 owner；仅由 policy
稳定发布而没有 live payload tree 时允许 sentinel。DXIL/SPIR-V 的 index 数值不要求相同。

declaration owner 属于 CPU payload schema，不参与 `BasePipelineLayoutHash`、`GpuArtifactHash` 。SPIR-V immutable sampler 引用 Vulkan-specific fixed-width full-state record，
覆盖 filter、address、mipmap/LOD、bias、anisotropy、compare、border 和 reduction；wire 不持久化
`VkSamplerCreateInfo` 或 `pNext`。push/root record 携带 canonical declaration identity，pure push
shader 也能建立 name lookup。compiler 的 `BasePipelineLayoutHash` 覆盖 canonical base semantics；
调用方的 RHI 显式布局不进入 artifact identity。

`radrayshader` 只做 wire safety 检查和 target-specific decode；它不链接 DXC 或 render。
调用方提供可信的 `ExpectedGpuArtifact`，decoder 与 envelope 比较。需要 metadata 的上层自行消费 decoder；
RHI 不接收这个 envelope，不进行 artifact hash、名称或 carrier 校验。

编译返回的 `CompileTargetLane::Stages` 包含各 stage 的独立 bytecode 与 entry name；调用方把匹配后端的
原始 bytes 交给 `Device::CreateShader(ShaderDescriptor)`，在 PSO 的 `ShaderEntry` 中选择 entry。
创建 shader 不生成布局。`BindingGroupLayoutDescriptor` 和 `PipelineLayoutDescriptor` 由调用方另外提供，
即使某变体裁掉资源也不会缩减这些描述。原先 render artifact bridge、target layout resolver/modifier、
`FindBinding(name)` 与布局身份 handle 已移除。

type tree 属于所属 artifact 的 CPU upload schema。`WireTypeRecord` 保留 scalar/vector/matrix 的
kind，并为 struct/struct-array member 携带 compiler-owned underlying `TypeIndex`；decoder 检查
range、已知 record kind、element count、offset/size/stride、type reference、父结构范围、同级
名称和父链环路。declaration owner 在 type tree 校验成功后单独校验：CBuffer 必须指向有效顶层
`Struct`，非 CBuffer 必须为 sentinel；concrete root-constant owner 同样必须指向有效顶层
`Struct`，且 payload size 不得超过可写 root range。type tree 没有独立 schema hash，也不参与
GPU artifact identity。

DXIL 仅对有父结构、没有 underlying `TypeIndex` 的 Float scalar 与 float3 vector 叶子成员允许尾部填充：
两者均要求 `RowCount=1`、`Flags=0`、`ElementCount=1`；scalar 为 `ColumnCount=1`、`Stride=4`，
float3 为 `ColumnCount=3`、`Stride=12`。
固定 schema 8 编译器为两个分别位于 `packoffset(c0)` 与 `packoffset(c1)` 的 `float3`
发布首成员 size/stride 16/12；位于 c0 与 c2 时为 32/12。`Size` 包含到下一字段的占位间隙，
`Stride` 是有效载荷大小。scalar 位于 c1.y、下一个 float3 位于 c2 时，首字段 size/stride 为 12/4。
decoder 保留原始记录，要求 size 不小于 stride。
成员占位范围不能越过父结构或与同父成员重叠；其他 scalar、matrix、其他 vector、聚合及根记录
仍要求 size 等于 stride，数组仍要求 size 等于 stride 乘元素数。SPIR-V 不使用这个例外。
matrix 保持严格校验；合法 float4x4 形状增加 16 字节占位的负例覆盖该边界。
真实 DXC 及损坏元数据回归见
`RadRayDxcMetadata.PaddedDxilCbufferLeavesDecodeWithoutChangingCompilerMetadata`，目标隔离见
`RadRayShaderArtifact.LeafPaddingIsDxilOnly`。render 不再通过 metadata 构建常量数据，GPU 数据布局由调用方保证。

schema 8 起每条 record 还携带 `ShaderScalarKind`（float/sint/uint/bool）与 `RowCount`/`ColumnCount`，
`Flags` bit 0 表示 HLSL `row_major`，其余位保留为零。非 struct 元素的数组同样带上元素的标量种类
与分量数，所以 `float4 Foo[4]` 在 wire 上是完整类型而不是不透明字节区间。这些事实存在的唯一原因
是让 AOT 生成器能离线复现 GPU 布局；运行时不用它们做类型门闩。

同一 shared struct 在 DXIL 与 SPIR-V lane 上发布的 offset/size/stride/标量种类/矩阵形状必须逐项
相等，否则整个 batch 返回无 `Value` 的失败结果，并在诊断里点名该 struct，而不是发布
其中一个 lane 的答案——由有争议布局生成的 POD 会静默与 GPU 不符。当前已知的分歧构造有三类，
需要跨 lane 一致布局的 cbuffer 必须避开：非方阵（`float3x4` 两 lane 的 rows/cols 与 matrix stride 相反）、
cbuffer 里的 `bool`（DXIL 报 Bool 且 size 补到寄存器边界，SPIR-V 报 uint 且 size 4）、以及元素
大小不是 16 字节整数倍的 struct 数组（DXIL 报紧排 stride，SPIR-V 报 cbuffer 规则的 stride）。
`RadRayDxcMetadata.CBufferTypePayloadPinsScalarKindShapeAndOffsets` 钉住两 lane 一致的部分，
`RadRayDxcMetadata.CrossLaneTypePayloadDisagreementFailsClosed` 钉住分歧必须 fail closed。

运行时不再构建按名参数布局或提供参数存储与 setter；调用方负责按实际 GPU 布局准备常量数据。

## 显式 RHI 布局边界

编译器继续发布当前 schema 8 的 RootSignature policy、placement、immutable sampler 和 push facts，
它们是 shader 工具层信息。RHI 不根据这些记录自动创建、修饰或缓存布局，也不要求应用链接 shader 模块。
上层可以选择人工定义 ABI，或在自己的工具流程中读取反射信息并生成描述；生成结果仍由上层维护和校验。

完整布局直接拥有原生资源，复用通过调用方共享对象实现，没有设备内的布局去重表。
描述符分配器与 sampler cache 继续保留。详细所有权与成本见
[RHI 原生布局直接所有权](render-rhi.md#原生布局直接所有权与显式共享)。

## Native PSO boundary

开启 RHI 校验时，包装层在调用 D3D12/Vulkan native pipeline API 前校验 `VertexInputState`：semantic、format、
buffer slot、attribute location、offset/stride 和重复声明必须自洽。失败不会把坏输入交给 native
PSO，也不会通过 compiler client 或 runtime reflection 补齐 vertex schema。
调用方直接提供 geometry 的 stride/slot/format/offset 与 shader semantic/location 组成的 RHI VertexInputState。
原生 shader entry、pipeline layout 与资源寿命由调用方管理，compiler client 不负责创建 PSO。
render 测试覆盖手写显式布局和双后端真实 compute PSO、dispatch/读回；wire/contract 测试位于 shader 模块。
测试入口与 compiler-free fixture 边界见[构建与测试](../guide/build-test.md#测试)。

## Build boundary

默认 `win-x64-debug` 开启 compiler 和 shader tools。通过独立 `-B` 目录和
`-DRADRAY_BUILD_SHADER=OFF -DRADRAY_BUILD_SHADER_COMPILER=OFF -DRADRAY_BUILD_SHADER_TOOLS=OFF`
验证完全独立的 render，不查找 DXC 包。只关闭 compiler 时 shader decoder 仍可保留；已开启的 tools 必须显式关闭。
compiler 要求 shader，tools 要求 compiler，非法开关组合配置失败。
仅构建工具可关闭 tests/window/render/benchmarks，再构建 `radray_shader_compile` target。
runtime/JIT/examples 已退出当前构建，不再提供相关选项。

compiler 配置通过 `find_package(RadRayDXC CONFIG REQUIRED GLOBAL COMPONENTS Headers Compiler)`
查找 fork 包，核对完整版本标识 `1.9.2609.radray.1` 和 Compiler/Validator 运行库。仓库 preset 设置依赖三元组后，
主工程将 `SDKs/radray_dxc/extracted` 作为查找提示；也可使用 `RADRAY_DXC_ROOT` 显式指定包。
`tools/fetch_sdks.py restore` 负责 manifest 的版本锁定与 SHA-256 校验；主工程不自动恢复或构建依赖。

client 默认用裸名 `dxcompiler` 动态加载，先尝试 exe 同目录，再走系统动态库搜索路径。
开发时，CLI、测试与 fixture generator 通过启动环境查找 SDK，不复制 DXC DLL；Windows preset
负责构建与测试的 `PATH`，IDE 和手动运行入口见[开发时加载 DXC](../guide/build-test.md#开发时加载-dxc)。

client handshake 校验 RadRay extension 的 ABI、schema、toolchain identity；stock DXC
不会被当作兼容 compiler，`RADRAY_SHADER_COMPILER_FORK` 在 compiler 构建中恒定义。
子工程默认开启 compiler、关闭 shader CLI；功能开关和宿主示例见
[作为子工程引用](../guide/build-test.md#作为子工程引用)。

`radray_shader_compile` 是 raw-lane 开发工具：从 shaderlib root 读取一个 `.hlsl`，将该 root
和可重复的 `--include-path` 按顺序传给 compiler，选择默认 keyword assignment，并写出 target
bytecode 与 metadata envelope；它不生成正式 manifest、
artifact index 或 publisher 输出，也不代表 stock DXC 已提供 RadRay extension ABI。工具目标
只链接 `radrayshadercompiler`，可用 map/import 检查确认没有反向引入 render/backend。

旧 Forward cbuffer 与 ImGui/blit artifact 的专用生成器和 custom targets 已随渲染框架移除；
历史方案见[设计快照](../history/2026-09-14-render-framework-design.md)。raw shader compile CLI 继续保留。

RadRay 安装包导出 `RadRay::shadercompiler` 与可选 `RadRay::shader_compile`，不加载优化探测。
普通安装不复制 SDK；安装后的 CLI 通过外部运行库搜索路径加载 compiler。
可独立运行的完整 SDK 或工具发布包由打包步骤收集 DXC 运行库，日常构建不承担该职责。
调用方显式传入 `RadRay_SHADERLIB_DIR`。包组件和安装布局见
[安装与包消费](../guide/build-test.md#安装与包消费)。

当前没有正式 shader artifact publisher 或索引；fork SDK autobuild 只发布 compiler package。
shader 测试读取版本控制的 metadata golden；render 测试只读取独立的 raw stage 字节码。
`modules/shader_compiler/tests/shader_fixture_generator.cpp` 属于 compiler 侧手动 `EXCLUDE_FROM_ALL` 目标，
生成器从 shader/compiler 测试 source 更新这些 fixture。普通 render 构建、测试和安装不调用生成器。
这些 fixture 不代表产品 artifact 发布链。
