> - 适用: 维护 shader compiler client、metadata wire、artifact decoder 或 shader compile 工具
> - 权威: 本文描述 schema 10 当前 shader pipeline 契约、编译边界与设计理由；HLSL 写法见 authoring 指南
> - 锚点: `modules/shader/include/radray/shader/shader_compiler_contract.h`, `modules/shader/src/shader_compiler_contract.cpp`, `modules/shader/include/radray/shader/shader_artifact.h`, `modules/shader/include/radray/shader/client.h`, `tools/CMakeLists.txt`, `CMakePresets.json`

# Shader pipeline

## 当前边界

HLSL 是 compiler client 的 pass source authoring 输入。统一 shader 模块通过 RadRay DXC fork 生成独立
target lanes，将已解码 artifact 合并为拥有型布局。runtime 的 ShaderAsset 接收手写描述或离线布局索引，
创建当前后端的 Shader；加载阶段不调用 DXC。RHI 本身不消费 metadata。
布局与参数 ABI、稳定槽位、原生兼容性和共享策略由调用方负责，见 [RHI 绑定模型](render-rhi.md#绑定模型)。

当前 metadata schema 为 10，decoder 拒绝其他 schema 版本。
RadRay DXC package 为 `1.9.2609.radray.4`（基于 upstream `v1.9.2609`），extension ABI 为 5，
wire 的 `kShaderToolchainIdentity` 为 `0x0000000001090216`。`project_manifest.json` 按 `EnforceHash` 固定正式发布归档的 hash。

正式 SDK 是独立构建并安装的 RadRay DXC fork package。
`radrayshader` / `RadRay::shader` 统一提供编译契约、DXC client、artifact 解码、类型树、布局和家族反射，
公共 API 位于 `radray::shader`。唯一自有直接依赖是纯头文件 `RadRay::gpu`，通过它使用 core。
DXC SDK 只用于私有实现编译，构造 client 时动态加载；解码、反射和 runtime 资产加载不调用 DXC。
`RadRay::gpu_impl` 接收原始 stage 字节码与显式布局，不依赖 shader。runtime 依赖 shader 与 gpu_impl，
负责 GPU 资产与布局注册表。布局 ABI、稳定槽位和共享策略仍由调用方负责。

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

解码器在首次失败处返回检查项及安全可读的记录索引或布局数值；compiler client 保留 decoder 诊断并补充调用上下文。
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

编译契约中的 `ContractHash`、`BytecodeHash`、`BasePipelineLayoutHash` 和 `GpuArtifactHash` 都是 core 类型 `radray::Hash128` 的别名。
`<radray/shader/shader_compiler_contract.h>` 提供 discovery 请求编码 `EncodeSourceContractRequest` 和结果解码 `DecodeWireShaderContract`。
解码时按 kind、keyword group 名称和值、entry 名称及 stage 的顺序重算契约摘要。
每段文本以字节 `0xff` 结束，stage 占一个字节。core 的通用 `HashData128` 使用 XXH3-128，
也返回 `Hash128`，原始字节可从 `Bytes` 读取。[core 哈希接口](../../modules/core/include/radray/hash.h)

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

相关 entries 全部未声明时，DXIL artifact 的 serialized Root Signature range 为空，metadata 记录普通
descriptor placement；SPIR-V 同样使用 ordinary descriptors。compiler 不生成公共默认 policy，
也不因 SPIR-V-only request 发布隐藏 DXIL result。
RHI 不消费 metadata 或 serialized carrier；D3D12 Root Signature 与 Vulkan pipeline layout 都由调用方的显式描述创建。
组存储、CPU mirror 与发布规则见 [RHI 与后端](render-rhi.md#绑定模型)。

## Artifact wire

每个 lane 返回独立 bytecode 与 compiler-owned metadata blob。schema 10 envelope 固定 magic、schema、
target、toolchain identity、contract、bytecode/base-layout/gpu artifact hashes 和各 payload range。
payload 记录 entry、active logical declarations、type tree、root/push facts、target base placement、
bytecode range，以及仅 DXIL 可有的 serialized Root Signature carrier。Explicit range 原样保存
DXC 的 `DXC_OUT_ROOT_SIGNATURE` carrier；最终 DXIL object 通过 `-Qstrip_rootsignature` 去掉嵌入
`RTS0`，artifact 只保留这一份 lane-level carrier。SPIR-V 的 carrier range 始终为空，但 payload
包含 RootSignature policy lower 后的 Vulkan records。

Envelope 为 152 字节；entry、binding、type、push/root、vertex input、sampler 记录分别为
20、36、44、32、20、20 字节。`<radray/shader/shader_compiler_contract.h>` 声明 envelope 和这六类记录的编解码接口，
实现按固定偏移以小端顺序读写字段。解码后的 `ShaderArtifact*Record` 是普通逻辑记录，
其 `sizeof` 不代表 wire stride；`GetShaderMetadataRecordSize` 返回对应序列化尺寸。
记录编解码接口检查位宽与保留位，失败保留输出；完整 artifact decoder 另行验证枚举、引用和布局语义。

名称按完整 UTF-8 字节串去重，以字节序排序后写入名称池；记录仍保存 32 位 offset/length。
偏移、大小、数量、索引均为 32 位，采样器四个浮点字段保留 IEEE 754 位模式。
各压缩字的位范围从最低位开始，其余位必须为零：

| 记录 | 压缩字在记录内的偏移 | 有效位 |
|---|---:|---|
| Entry | 8 | Stage 0–7 |
| Binding | 16 | Type 0–3，Stages 4–6，Placement 7–8 |
| Type | 12 | Kind 0–2，ElementKind 3–5，ScalarKind 6–8，Rows 9–11，Columns 12–14，RowMajor 15 |
| Push/root | 24 | Stages 0–2，原生 SPIR-V push 标志 3 |
| Vertex input | 16 | ComponentType 0–1，ComponentCount 2–4 |
| Sampler | 0 | Mag/Min/Mip filter 0/1/2，U/V/W address 3–5/6–8/9–11，anisotropy 12，compare enable 13，compare op 14–16，border 17–19，reduction 20–21，unnormalized coordinates 22 |

Entry/Binding/VertexInput 的逻辑 Flags 当前必须为零。Sampler 压缩字后依次存放
MipLodBias、MaxAnisotropy、MinLod、MaxLod；不做量化。

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

接口哈希使用独立版本 1 的 `RadRay.GPU.Interface` 编码。entry、binding、push 按名称排序，
vertex input 按 location/semantic/index 排序；字符串带长度，列表带数量，整数与浮点位模式按小端编码。
RootSignature policy identity、entry 名称/阶段、资源名称/位置/种类/数量/阶段/placement、
被引用的 immutable sampler 完整状态、push 范围/阶段/标志及顶点签名参与编码。
名称的物理 offset、记录内保留位、类型名称与类型表、TypeIndex 和 BufferByteSize 均不参与。
`BasePipelineLayoutDigest` 对此编码求摘要；`GpuArtifactHash` 还包括实际 stage 字节码。
`BytecodeDigest` 继续对 stage 顺序拼接的原始字节码使用既有算法；CPU 描述变化若同时改变编译器输出字节码，
GPU artifact identity 仍随实际字节码变化。修改 wire schema 需重生成 artifact 与可信 manifest。

artifact decoder 只做 wire safety 检查和 target-specific decode，不调用 DXC 或 gpu_impl。
调用方提供可信的 `ExpectedGpuArtifact`，decoder 与 envelope 比较。需要 metadata 的上层自行消费 decoder；
RHI 不接收这个 envelope，不进行 artifact hash、名称或 carrier 校验。

编译返回的 `CompileTargetLane::Stages` 包含各 stage 的独立 bytecode 与 entry name；调用方把匹配后端的
原始 bytes 交给 `Device::CreateShader(ShaderDescriptor)`，在 PSO 的 `ShaderEntry` 中选择 entry。
创建 shader 不生成布局。`BindingGroupLayoutDescriptor` 和 `PipelineLayoutDescriptor` 由调用方另外提供，
即使某变体裁掉资源也不会缩减这些描述。gpu_impl 不提供 artifact bridge、target layout resolver/modifier、
按名称的绑定查询或布局身份 handle；shader decoder 的 `FindBinding(name)` 仅查询 metadata。

type tree 属于 artifact 的 CPU upload schema。类型记录保留 `Kind`、`ElementKind`、
`ScalarKind`、HLSL 逻辑 `RowCount/ColumnCount`、主序、父索引及 underlying `TypeIndex`。
数组容器与元素类型分离，长度为 1 的数组和矩阵数组均保留数组身份。
`Size`、`ElementSize`、数组及矩阵步长均显式存储，解码不推算被省略的跨度。

存储跨度包含内部空隙，不含最后一个有效字节之后的额外分配填充：

| 字段 | 契约 |
|---|---|
| `Size` | 成员从起点到最后一个有效字节末尾的跨度 |
| `ElementSize` | 单元素跨度；非数组等于 Size |
| `Stride` | 数组元素起点间距；非数组为 0 |
| `MatrixStride` | 矩阵相邻存储向量起点间距；非矩阵为 0 |
| `BufferByteSize` | CBuffer 安全存储大小，至少覆盖原生报告及成员，按 16 字节向上取整 |

矩阵跨度为 `(MajorCount - 1) * MatrixStride + MinorCount * 4`；数组跨度为
`(ElementCount - 1) * Stride + ElementSize`；结构体跨度为成员 `Offset + Size` 的最大值。
`float3x3 Matrices[2]` 的常见布局为 ElementSize=44、Stride=48、MatrixStride=16、Size=92，
仅含该字段的 CBuffer 大小为 96。GPU 分配仍需遵守设备对齐；PushConstant 保留原生范围。
DXIL 标量、向量不再用下一字段偏移差作为大小。

支持 32 位 float/sint/uint 标量与向量、2–4 行列的 float 矩阵（两种主序、方阵及非方阵）、
嵌套结构体和一维定长数组。native 16/64 位、多维数组及运行时长度常量数组不在支持范围内。
`bool` 的 DXIL Bool 与 SPIR-V uint 差异仍拒绝；矩阵后接标量等真实偏移差异也会使双端编译失败。
SPIR-V 的形状转换回 HLSL 逻辑形状，偏移和步长保留实际后端布局；反射器不推测矩阵步长。

Decoder 校验跨度公式、整数溢出、父子关系、同级名称和范围，以及沿 TypeIndex 的循环。
CBuffer owner 必须是有效根 Struct，PushConstant 的可选类型不能超过其范围；非 CBuffer 不携带类型块。
类型树没有独立 schema hash，也不参与 GPU artifact identity。

## Shader 布局

`ShaderPipelineLayoutDesc` 描述资源布局。`ShaderVariantInterfaceDesc` 描述一个变体，其中 DXIL 和 SPIR-V 各有可选的
`ShaderVariantTargetInterfaceDesc`。`ShaderFamilyLayoutDesc` 持有共享资源布局和变体列表。绑定与 push constant 使用 RHI 描述，
拥有型类型表使用 `ShaderTypeRecord`，顶点签名使用 `ShaderVertexComponentType`。

原先包含 `shader_type_tree.h` 的调用方改用 `<radray/shader/shader_layout.h>`。该头文件同时提供 `ValidateShaderTypeRecords`
和 `FindShaderTypeMember`。成员路径支持 `a.b[i].c`，查询结果包含绝对偏移、数组和矩阵步长及 padding 信息。
原 C++ 类型 `ShaderProgramInterfaceDesc`、`ShaderTargetProgramInterfaceDesc` 分别迁移到 `ShaderVariantInterfaceDesc`、
`ShaderVariantTargetInterfaceDesc`。变体校验入口为 `ValidateShaderVariantInterfaceDesc`，变体 Binary/JSON 的读写函数名也将
`ShaderProgramInterface` 替换为 `ShaderVariantInterface`。
独立变体接口的 v1 JSON 读写均使用 `"Kind": "ShaderVariantInterface"`。使用旧标记 `"ShaderProgramInterface"` 的文件需将顶层 `Kind` 改为新值后才能读取。

三类布局描述保留版本 1 的 JSON/Binary 读写，失败不改变输出；二进制显式编码小端字段。
原有顶点类型编号 Float=0、Sint=1、Uint=2 和 JSON 的 Sint/Uint 名称保持不变，读写时显式映射到 shader 枚举。
校验覆盖名称/ID 唯一、原生映射冲突、数量、溢出、类型引用和跨度及变体引用。
artifact 解码和布局读取共享类型树跨度、成员范围、循环引用及顶点签名校验。解码器另行验证名称池范围和保留位。
原先使用 `shader::ShaderBindingKind` 的调用方统一改用 `gpu::BindingKind`。读取 metadata 时，用
`<radray/shader/shader_compiler_contract.h>` 中的 `DecodeBindingKind(record.Type)` 替代 `TryMapBindingKind`。
构造 metadata 记录时，将同一头文件中 `EncodeBindingKind(kind)` 返回的编号写入 `Type`。
调用方需要使用 `gpu::BindingKind::Texture` 等枚举成员时，包含 `<radray/gpu/types.h>`。
两个函数共用 schema 10 的编号表，遇到未知值均返回 `std::nullopt`。布局 v1 二进制单独使用 GPU 枚举值。
原先调用 shader 侧寄存器类别、root descriptor 或可写性 helper 的代码，在转换成功后分别调用
`gpu::GetBindingRegisterClass`、`gpu::CanBeRootDescriptor`、`gpu::IsWritableBinding`。
纹理、常量缓冲和 typed buffer 分类直接比较转换后的 `gpu::BindingKind`。
离线布局检查每个已提供的后端映射。RHI 对当前后端使用同一组设备无关规则，再检查设备能力。
绑定和 push constant 共用全局名称空间；结构体成员仅在所属结构内唯一。
类型跨度见 [Shader pipeline](shader-pipeline.md#artifact-wire)。

## 家族反射

家族反射属于 `RadRay::shader`。`ReflectShaderVariant` 和
`ReflectShaderFamily` 接收已解码的强类型 DXIL/SPIR-V artifact，不调用或链接 DXC，返回
`DiagnosticResult<ShaderFamilyLayoutDesc>`。输出拥有字符串和类型表，不借用输入 artifact。

家族先汇总所有变体，默认按 Vulkan set 分组；无 Vulkan 映射时按 D3D12 space 分组。
策略可显式指定 GroupSlot、组名、BindingId 和 PushId；显式 ID 先占用，其余按名称排序取最小可用 ID。
输入变体顺序不影响结果；跨资产版本的永久稳定 ID 必须显式指定。
同名资源要求 Kind、Count、原生位置、Placement、immutable sampler 和类型结构兼容，Stages 取并集。
类型按成员名称及结构递归比较并重写所有索引，根名称不构成 ABI 身份，未使用根不导出。

配对 artifact 默认拒绝单侧资源；`AllowTargetOnlyBindings` 显式允许后保留该侧映射和活跃引用。
DXIL 静态 sampler 必须有配对 SPIR-V metadata 提供完整状态。共享布局是整个家族的资源超集，
每变体、每后端单独保存顶点输入与活跃 Binding/PushConstant 引用；活跃信息不裁剪布局，
也不免除 RHI 参数集完整性要求。runtime 的创建、查询和参数写入契约见 [Shader 资产、布局与缓存](runtime.md#shader-资产布局与缓存)。

## 显式 RHI 布局边界

编译器继续发布当前 schema 10 的 RootSignature policy、placement、immutable sampler 和 push facts，
它们是 shader 工具层信息。RHI 不根据这些记录自动创建、修饰或缓存布局，也不要求应用链接 shader 模块。
上层可以选择人工定义 ABI，或在自己的工具流程中读取反射信息并生成描述；生成结果仍由上层维护和校验。

完整布局直接拥有原生资源，复用通过调用方共享对象实现，没有设备内的布局去重表。
描述符分配器与 sampler cache 继续保留。详细所有权与成本见
[RHI 原生布局直接所有权](render-rhi.md#原生布局直接所有权与显式共享)。

## Native PSO boundary

开启 RHI 校验时，包装层在调用 D3D12/Vulkan native pipeline API 前校验 `VertexInputState`：semantic、format、
buffer slot、attribute location、offset/stride 和重复声明必须自洽。失败不会把坏输入交给 native
PSO。调用方可以直接提供 RHI 状态，也可先通过 runtime 的 `ResolveVertexInputState`
按 semantic 配对几何与后端签名，解析后的状态仍经过相同 RHI 校验。
原生 shader entry、pipeline layout 与资源寿命由调用方管理，compiler client 不负责创建 PSO。
gpu_impl 测试覆盖手写显式布局和双后端真实 compute PSO、dispatch/读回；wire/contract 测试位于 shader 模块。
测试入口与 compiler-free fixture 边界见[构建与测试](../guide/build-test.md#测试)。

## Build boundary

默认构建统一 shader 及两个 CLI；编译能力没有独立关闭开关。`RADRAY_BUILD_SHADER=ON` 时需要 DXC SDK。
`RADRAY_BUILD_SHADER_TOOLS` 和 `RADRAY_BUILD_SHADER_REFLECTOR_TOOLS` 分别控制 compile/reflect CLI，均依赖 shader。
关闭 gpu_impl、runtime、window、benchmarks、examples 可单独构建 shader 和 CLI，不构建 GPU 后端。
以 `-DRADRAY_BUILD_SHADER=OFF -DRADRAY_BUILD_EXAMPLES=OFF` 验证独立 gpu_impl；runtime 和 CLI 自动关闭，不查找 DXC。
runtime 要求 gpu_impl 与 shader；examples 要求 runtime。gpu 随 core 提供，没有独立构建开关。

compiler 配置通过 `find_package(RadRayDXC CONFIG REQUIRED GLOBAL COMPONENTS Headers Compiler)`
查找 fork 包，核对完整版本标识 `1.9.2609.radray.4` 和 Compiler/Validator 运行库。仓库 preset 设置依赖三元组后，
主工程将 `SDKs/radray_dxc/extracted` 作为查找提示；也可使用 `RADRAY_DXC_ROOT` 显式指定包。
`tools/fetch_sdks.py restore` 负责 manifest 的版本锁定与 SHA-256 校验；主工程不自动恢复或构建依赖。

client 默认用裸名 `dxcompiler` 动态加载，先尝试 exe 同目录，再走系统动态库搜索路径。
开发时，CLI、测试与 fixture generator 通过启动环境查找 SDK，不复制 DXC DLL；Windows preset
负责构建与测试的 `PATH`，IDE 和手动运行入口见[开发时加载运行库](../guide/build-test.md#开发时加载运行库)。

client handshake 校验 RadRay extension 的 ABI、schema、工具链主次版本 1.9 及非零工具链身份；
它不将整个 128 位身份与固定值比较。artifact decoder 只在调用方传入非零 `ExpectedToolchainIdentity` 时
比较 wire 的 64 位工具链身份，该选项默认 0。stock DXC
不会被当作兼容 compiler，`RADRAY_SHADER_COMPILER_FORK` 在 compiler 构建中恒定义。
子工程默认开启 shader、关闭 shader CLI；功能开关和宿主示例见
[作为子工程引用](../guide/build-test.md#作为子工程引用)。

`radray_shader_compile` 链接 shader，接受重复 `--keyword NAME=VALUE`；名称、值和重复项
均校验，未指定组仍取声明中的首值。`--manifest <file>` 可输出版本 1 的编译结果 manifest，
其中包含默认变体名 Default、相对 artifact 路径、GPU artifact hash 与 toolchain identity。

`radray_shader_reflect --manifest <file> --output <prefix>` 链接 shader。manifest 形态为：

```json
{
  "version": 1,
  "variants": [{
    "name": "Default",
    "dxil": {"path": "shader.dxil.bin", "gpuArtifactHash": "32位十六进制字符串", "toolchainIdentity": 17367573},
    "spirv": {"path": "shader.spirv.bin", "gpuArtifactHash": "32位十六进制字符串", "toolchainIdentity": 17367573}
  }],
  "policy": {
    "allowTargetOnlyBindings": false,
    "bindings": [{"name": "Pass", "groupSlot": 0, "bindingId": 0, "groupName": "Pass"}],
    "pushConstants": [{"name": "MaterialPush", "pushId": 0}]
  }
}
```

后端字段和 policy 可省略，至少提供一个后端；hash 必须来自独立可信的编译结果，不能从待验证
artifact 自抄。所有路径相对 manifest。工具先读取、解码和反射全部变体，再输出
`.layout.json`、`.layout.bin`、`.<变体序号>.<dxil|spirv>.<stage>.bin`，最后发布 `.index.json`。
索引记录变体、后端、入口、阶段和文件；写入前移除旧成功索引，失败返回非零且不发布成功索引。
编译结果 manifest 同样在全部 lane 写入成功后发布。可将多次编译的 variants 汇入一个 manifest 反射家族。

RadRay 安装包导出 `RadRay::gpu`、`RadRay::gpu_impl`、`RadRay::shader` 与各自可选 CLI。
shader 组件只加载 gpu/core，两个 CLI 组件加载 shader；DXC 头是构建阶段依赖，安装消费不查找 DXC SDK。
只有实际调用编译能力时才需要外部 DXC 运行库搜索路径；普通构建和安装均不复制 DXC DLL。
包配置不执行优化探测，独立 SDK 发布包由打包步骤收集运行库。
调用方显式传入 `RadRay_SHADERLIB_DIR`。包组件和安装布局见
[安装与包消费](../guide/build-test.md#安装与包消费)。

反射 CLI 发布本地布局和索引；fork SDK autobuild 只发布 compiler package。
shader 测试读取本地生成的 artifact，对 metadata 做结构化断言，不比较 golden hash；gpu_impl 测试只读取
独立的 raw stage 字节码。生成的 `.bin` 不入库，统一由 `tools/compile_test_shaders.py` 显式准备到
被忽略的 `build/test_shaders/`（CMake 缓存变量 `RADRAY_TEST_SHADER_DIR` 可改写）。脚本组合 compiler 侧
`EXCLUDE_FROM_ALL` 的 `radray_shader_fixture_generator`（`shader_artifacts/`）与
`tools/compile_render_fixtures.py` 的 raw DXC 配方（`shader_bytecode/`）；普通测试构建只验证
源指纹和产物，不构建/调用生成器。shader 关闭的 gpu_impl 消费者需预先准备夹具，但仍不查找或加载 DXC。
生成依赖、增量校验与干净检出步骤见[本地测试 shader](../guide/build-test.md#本地测试-shader)。
这些 fixture 不代表产品 artifact 发布链，也不参与产品安装。
