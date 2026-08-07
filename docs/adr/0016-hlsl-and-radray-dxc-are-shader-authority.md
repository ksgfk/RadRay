# ADR-0016 HLSL 与 forked RadRay DXC 是 shader 权威

状态: 生效
日期: 2026-08
影响: `shaderlib/**`、RadRay DXC fork、`modules/render`、`modules/runtime`、可选 `radrayshadercompiler`、CMake SDK 集成、shader artifact wire

## 背景

当前 shader 路径同时存在 HLSL、手写 `*.shader.json`、运行时反射和 shader C++ 数据模型。
它们分别描述资源绑定、变体域、stage 拓扑、vertex 输入和 pipeline layout，导致同一事实有多条
获取路径。C++ trace 方案可以把绑定写入 C++，但会引入另一种 shader authoring/codegen 语言，不能
复用现有 HLSL、DXIL Root Signature 和标准 SPIR-V binding 语义。

研究记录 `docs/research/dxc-embedded-metadata-vs-cpp-trace.md` 已确认：forked DXC 可以在 frontend
仍持有 AST 时解析 HLSL pragma/声明，在一个结构化扩展请求中完成 source contract discovery 与
concrete Variant 的双 target 编译，并返回 bytecode 与独立 metadata blobs。DXIL 与 SPIR-V 的
binding/layout 事实本来就不完全同构，强行合并成一份公共字节布局会丢失信息。

## 决策

### 1. Authoring 与 compiler authority

HLSL 是唯一 shader authoring 源真相，forked RadRay DXC 是唯一编译和 metadata 权威。删除 C++ trace、
手写 shader JSON、运行时反射校验、SPIRV-Cross 和独立的 source-identity/pragma 扫描器。

Pass 根 `.hlsl` 使用标准 `[shader("vertex")]`、`[shader("pixel")]`、`[shader("compute")]`；
keyword group 由根文件中的 `#pragma radray_keyword_group` 声明。调用方只提供普通 `Defines`、
`KeywordAssignments`、typed `CompilePolicy`、target mask 和可选的 expected `ContractHash`。
Variant 枚举与 cook coverage 属于调用方/资产系统，不属于 HLSL metadata。

PassName 与 AssetId 也属于调用方/资产系统。它们不得进入 RadRay DXC request、metadata 或 compiler
hash；资产索引在外部把身份映射到内容寻址的 compiler artifact。

### 2. Target 与 stage contract

同一 source/include/Defines 在 DXIL 与 SPIR-V lane 各做一次轻量 discovery。两 lane 必须拥有相同的
keyword domain、entry names、stage、graphics/compute kind 和 cardinality；`__spirv__` 可以改变
函数体、资源集合、类型和 layout，但不能改变这些拓扑事实。

graphics Pass 恰好一个 vertex entry、至多一个 pixel entry；compute Pass 恰好一个 compute entry；
同一 source unit 不得混合 graphics 与 compute。每个 concrete Variant 只拥有实际 active binding
集合，graphics 的 metadata merge 在 forked DXC 内完成，并按 stage 记录 visibility。

DXIL lane 以标准 HLSL register/space 和 `[RootSignature(...)]` 为真相。compiler 根据当前 Variant
的 active resource 并集投影精确 Root Signature；允许多个 RootConstants。SPIR-V lane 以标准
`vk::binding`/`vk::push_constant` 与 DXC 产物为真相；每个 source 最多一个 active push-constant
block。两 lane 的数字、数量与字节布局不要求相同。

DXIL Root Signature 中的 static sampler 通过 HLSL declaration identity 关联到 SPIR-V 的同一
`SamplerState` declaration，并输出 Vulkan immutable sampler metadata；不能用 binding number 相等
作为关联依据，无法唯一关联时编译失败。

### 3. Compiler result 与 artifact wire

扩展 API 只在 `dxcapi_radrayext.h` 中声明 RadRay-owned CLSID/interfaces/result；不改变 upstream
`dxcapi.h`、已有 IID/vtable、exports 或 `IDxcResult` contract。`IRadRayDxcResult` 一次返回各请求
target lane 的 bytecode、独立 canonical metadata、diagnostics 和 identity/hash。为生成 SPIR-V lane
所需的 immutable sampler metadata，compiler 可以在内部额外执行不产出 DXIL result 的 DXIL-mode
RootSignature/static-sampler analysis；该辅助 lane 不改变 result 的 requested target lane 集合。
任一 requested lane 失败，整个 batch status 为 failed，所有 target lane 都不可访问，不产生
publication 或 persisted blob；diagnostics 可以保留各 lane 的失败信息，但成功 lane 也不得单独交付。

metadata 不嵌入 DXIL/SPIR-V，也不由运行时或 cook 二次序列化。DXIL 与 SPIR-V 共享薄 envelope，
payload 分别使用 target-specific、versioned、fixed-width records；标准 serialized Root Signature
可作为 DXIL payload 原样叶子数据。不得持久化含 pointer、platform ABI 或 `pNext` 的 native structs。

metadata 必须包含运行时构造 CPU buffer 所需的完整 target-native cbuffer/struct member type tree，
但 compiler 输出的 type-tree record 不作为 `ContractHash`、`CompileInputHash`、`GpuArtifactHash` 或
任何 `CpuSchemaHash` 的独立输入；source/include bytes 仍按 `CompileInputHash` 的正常规则参与。
第一期不定义 `ArtifactContentHash`、content-address publisher 或完整 artifact integrity hash。
compiler 同时产生覆盖完整 target bytecode 的 `BytecodeHash`，以及覆盖 canonical target-native
GPU layout records 的 `PipelineLayoutHash`；二者与 `GpuArtifactHash` 使用统一的 128 位固定字节序
表示。`GpuArtifactHash` 只覆盖 bytecode 与 GPU layout metadata；runtime 不重算这些 hash，也不在
第一期以 `PipelineLayoutHash` 建立共享缓存。decoder 可以把 metadata 中的 `GpuArtifactHash`
与 caller 提供的独立可信 expected hash 做相等比较，但不把该比较扩张为完整 artifact integrity
校验。runtime 只对 type tree 做 wire bounds、record kind、offset/size、stride 和 CPU 构造安全检查，
不做第二份 schema 或 reflection 交叉校验。
PassName/AssetId 不进入 compiler hash。

### 4. Runtime ownership 与 trust boundary

`radrayrender` 拥有 bytecode view、metadata decoder、target-native binding/layout、vertex interface、
完整 CPU type tree 和 backend pipeline layout 构造；第一期 `radrayruntime` 只拥有 Variant 选择、
JIT 调度、加载/缓存及 fail-closed policy，不实现 artifact index/磁盘 artifact loader；可选
`radrayshadercompiler` 仅为 RadRay DXC SDK 的动态加载 client adapter。

compiler 输出的 target-native vertex interface 必须与 caller 提供的 `PrimitiveVertexLayout` 在
PSO builder 中组合。semantic/location、shape/format、slot/offset/stride/step mode 不兼容时，
builder 必须在创建任何 D3D12/Vulkan native PSO 前 fail closed；不得调用 runtime reflection 作为补救。

运行时完全信任 compiler artifact，不通过 DXIL/SPIR-V reflection 二次核对。缺失、损坏、schema 或
toolchain 不兼容、Variant/target 不存在时直接失败，不得 JIT fallback。纯 runtime 配置不发现或
导入 SDK，不含 compiler client、headers、CLI、import libraries、`dxcompiler`/`dxil` binaries、
HLSL source 或 SPIRV-Cross。

### 5. SDK 与第一期边界

RadRay 主工程只消费独立流水线生成的预编译 `RadRayDXC` SDK，不以 CMake 子工程构建 DXC。SDK 提供
`RadRayDXC::Headers`、`Compiler`、可选 `Validator`、可选 `CLI` imported targets；`find_package`
不指定 numeric version，正式构建只从 manifest 固定 prefix/hash 发现 package。compiler capability
打开时 CMake 按公共 build/install 规则部署 compiler binary；关闭时构建图不出现 SDK。

第一期的完成点是：compiler contract、双 target metadata、运行时 decoder/layout、D3D12/Vulkan JIT
垂直切片、旧路径删除和 compiler-free build/package gate 全部通过。compiler-free gate 只消费预生成
的 raw golden bytecode/metadata fixture，不验证 artifact index/loader。正式 cook、artifact publisher、
内容寻址、完整 AOT coverage 和生产纯 runtime 发布属于后续阶段，不阻塞第一期。

## 放弃的方案及代价

- **C++ trace 作为 shader 源真相**：需要维护第二种 authoring/codegen 语言，不能直接复用 HLSL RootSignature、标准 SPIR-V attributes 和现有 shaderlib；放弃后保留 HLSL 学习成本与 fork DXC 维护成本。
- **手写 JSON/manifest 继续作为 ABI**：会保留多条事实来源和反射核对；放弃后必须把缺失字段补到 HLSL/DXC metadata，并承担 compiler fork 的实现责任。
- **运行时反射校验或 SPIRV-Cross 重建 layout**：无法保证与编译时 concrete Variant 完全一致，且污染纯 runtime；放弃后 artifact wire/schema 兼容性必须由 SDK/metadata gate 保证。
- **DXIL/SPIR-V 一个统一 layout/字节结构**：会抹平两 target 的真实差异；放弃后 runtime 需要维护两套 target-native decoder 和 backend layout。
- **把 DXC 源码 `add_subdirectory` 到 RadRay**：DXC 会污染父工程 configure/cache/target graph，且阻塞 SDK 裁剪；放弃后需要独立 autobuild 和可验证的 CMake package。

## 必须保持为真

- `rg` 找不到 shader authoring 路径中的 C++ trace、手写 shader JSON、运行时 reflection validation 或 SPIRV-Cross。
- stock DXC 对 RadRay extension CLSID 返回不支持；RadRay client 不回退到 upstream `IDxcCompiler3`。
- 同一 concrete Variant 的每个 target lane 只包含实际 active bindings；graphics merge 在 compiler 内完成。
- DXIL 与 SPIR-V metadata payload 可不同，binding number 不被假设相等；static sampler 关联按声明 identity。
- metadata wire bytes 从 compiler 到 runtime 原样传递，不存在第二次序列化。
- runtime 仅信任 artifact metadata；artifact 异常 fail closed。
- `RADRAY_BUILD_SHADER_COMPILER=OFF` 时不发现 SDK、不构建 client、不复制/安装 compiler binary。
- 每个 pinned SDK build 通过真实 D3D12 PSO 创建与执行 smoke test；external `dxil` 不是无条件依赖。
- 第一期开工不以 cook 完成为前提；compiler-free gate 必须在无 SDK/compiler/HLSL 的进程中消费
  版本控制的 raw golden fixture，但不宣称 artifact index/loader 或生产 AOT 已完成。
