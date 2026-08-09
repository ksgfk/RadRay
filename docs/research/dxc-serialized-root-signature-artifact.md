> - 适用: 评估把 HLSL `[RootSignature]` 的 DXC serialized Root Signature 直接交给 D3D12、定义 DXIL artifact 形状与 Root Signature 去重策略
> - 权威: 本文是基于 RadRay `e182294887960396388d41c3799150ec1e3e8e9e`、RadRay DXC fork `cfeae8de35483aca208d7eb6073b16d9b43c1337` 与 DXC `v1.9.2607` 的只读研究记录；它不改变当前 RadRay 契约
> - 锚点: `modules/shader/include/radray/shader/shader_compiler_contract.h`, `modules/shader/include/radray/shader/shader_artifact.h`, `modules/render/src/shader_artifact.cpp`, `modules/render/src/d3d12/d3d12_impl.cpp`, `docs/adr/0016-hlsl-and-radray-dxc-are-shader-authority.md`, `F:\cpp\DirectXShaderCompiler\include\dxc\dxcapi.h`, `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcompilerobj.cpp`, `F:\cpp\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcradray.cpp`, `F:\cpp\DirectXShaderCompiler\lib\DxilContainer\DxilContainerAssembler.cpp`, `F:\cpp\DirectXShaderCompiler\tools\clang\unittests\HLSLExec\ShaderOpTest.cpp`

# DXC serialized Root Signature 进入 RadRay DXIL artifact 的可行性

研究日期：2026-08-09。范围是普通 graphics/compute shader 的 global Root Signature，不讨论
DXR library 的 local/global Root Signature subobject。DXC fork checkout 有一处既存的
`DXIsenseTest.cpp` 未提交修改，与本研究无关；本文依据上述 HEAD 和未受该修改影响的 compiler、
container、RadRay extension 源码。

> 产品决策补记（2026-08-09）：后续对齐确认 `[RootSignature]` 可选。本文关于 Explicit RS 的
> `DXC_OUT_ROOT_SIGNATURE`、D3D12 direct consumption 与内容去重证据仍适用；本文曾推荐“缺失 RS
> 时 fail closed 或由 compiler 生成”的部分不再是当前计划。缺失 RS 时 artifact range 为空，由
> D3D12 RHI 根据 active binding metadata生成 Implicit D3D12 Root Signature；DXC 不实现 fallback
> generator。后续又确认本次去重只覆盖 artifact内的 stage-output coalescing；本文建议的
> `DeviceD3D12` native cache与未来 package content table均不进入当前计划，由上层另行处理。当前方案
> 最终 Explicit artifact会启用 `-Qstrip_rootsignature`，只保留lane-level standalone output；实施时
> 先保留 embedded `RTS0`完成GPU parity gate。当前方案见
> `docs/todo/dxc-root-signature-artifact-and-fallback.md`。

## 结论

**方案可行，且应采用。** DXC 已经把 `[RootSignature]` 编译为标准 serialized Root Signature，
同时把 `RTS0` 嵌入 DXIL shader container，并通过 `DXC_OUT_ROOT_SIGNATURE` 返回只含 Root
Signature 的独立 container。D3D12 官方明确允许把含 Root Signature 的 compiled shader blob
直接传给 `ID3D12Device::CreateRootSignature`；因此 RadRay D3D12 后端不需要把 compiler records
重新组装为 `D3D12_VERSIONED_ROOT_SIGNATURE_DESC`，也不需要再次序列化。

推荐的持久化形状不是“任取一个 stage 的完整 DXIL 当 Root Signature”，而是：

```text
DXIL lane artifact
  stage DXIL blobs[]                  // 可保留 RTS0；成熟后可 -Qstrip_rootsignature
  canonical RootSignature blob       // 原样保存 DXC_OUT_ROOT_SIGNATURE，一份/lane
  active binding/type/vertex records // 名称、CPU upload schema 与 shader interface
  hashes/toolchain/schema
```

D3D12 后端把 canonical blob 原样交给 `CreateRootSignature`，再用
`D3D12CreateVersionedRootSignatureDeserializer` **只读解析同一 blob**，建立命令提交需要的 CPU
binding plan。解析不是重新发明或重新序列化 Root Signature；Root Signature blob 仍是唯一 D3D12
layout 真相。

Root Signature 去重分两层：当前没有正式 cook/publisher 时，artifact 先保留 lane-local 一份，
`DeviceD3D12` 按 serialized blob 内容缓存 `ID3D12RootSignature`；未来 publisher 再把相同 blob
外提为 package-level content-addressed table。缓存只能共享 native Root Signature，不能直接共享
整个 RadRay `PipelineLayout`，因为相同 Root Signature 可以服务于 binding 名称不同的 shader，
而名称、`BindingHandle` generation 和 CPU type tree 都是 artifact-local 状态。

## 当前 RadRay 边界与实际缺口

当前架构已经完成了正确的模块分层：`radrayshader` 持有 compiler/render wire 与 decoder，
`radrayshadercompiler` 是可选 fork client，`radrayrender` 不依赖 DXC；DXIL 与 SPIR-V 使用不同的
target-native view。这个分层无需因 serialized Root Signature 改变。

但当前实现尚未兑现 ADR-0016 中“standard serialized Root Signature 可作为 DXIL payload 原样
叶子数据”的决定：

- `WireMetadataEnvelope` 只有 entry、binding、type、root constant、vertex input 和 bytecode range，
  没有 Root Signature range。
- fork 的 `CollectDxilModule` 已读取 `DxilModule::GetSerializedRootSignature()`，却只保存 128-bit
  digest、root constants、root binding 坐标和 static-sampler 名称；serialized bytes 随后被丢弃。
- `CompileStage` 只读取 `DXC_OUT_OBJECT`，没有读取已经存在的 `DXC_OUT_ROOT_SIGNATURE`。
- `modules/render/src/shader_artifact.cpp` 把 compiler records 压成公共 parameter-set 形状；
  `DeviceD3D12::CreateRootSignatureInternal` 再固定生成“每个 space 一个 resource table + 一个 sampler
  table”，最后调用 `D3D12SerializeVersionedRootSignature`。

这条重建路径会丢失或改写 `[RootSignature]` 中的真实策略，包括：

- descriptor table 与 root CBV/SRV/UAV 的 residency 差异；
- root parameter 顺序、一个 table 内的 range 顺序与显式 descriptor offset；
- descriptor range/root descriptor 的 1.1 flags；
- Root Signature flags 与精确 shader visibility；
- static sampler 的 filter、address、LOD、comparison、border color 等完整状态。

当前 wire 的 immutable sampler 只有一个 flag，render 侧据此构造默认 `SamplerDescriptor`；所以它只能
表达“这是 static/immutable sampler”，不能保真作者在 Root Signature 中写下的 sampler state。
直接消费 compiler blob 不只是省一次序列化，也修复了 D3D12 policy 被 backend 默认值覆盖的问题。

## DXC 与 D3D12 的一手证据

### DXC 已产生所需数据

`dxcapi.h:726-738` 把 `DXC_OUT_OBJECT` 定义为 shader/library object，把
`DXC_OUT_ROOT_SIGNATURE` 定义为 serialized Root Signature output。
`dxcompilerobj.cpp:1022-1146` 在 DXIL module 完成后同时组装 shader object 与独立 Root Signature
output。`DxilContainerAssembler.cpp:1986-2041` 从 module 取出 serialized bytes，把它写进独立
Root Signature container，并在未指定 `-Qstrip_rootsignature` 时把同一个 `RTS0` part 写进 shader
container。

RadRay fork 的 observer 调用发生在上述 container assembly 之前：
`dxcompilerobj.cpp:1064-1070` 把最终 module 交给 observer；当前
`dxcradray.cpp:1579-1593` 已能读取、反序列化并校验同一份 bytes。因此 extension 无需新增 HLSL
parser，只需在 stage result 中保留 `DXC_OUT_ROOT_SIGNATURE`，在 lane merge 时选择/核对 canonical
blob，再把它写入 metadata range。

### D3D12 可以直接消费

Microsoft 的 [Specifying Root Signatures in HLSL](https://learn.microsoft.com/en-us/windows/win32/direct3d12/specifying-root-signatures-in-hlsl)
说明 compiler 会验证 Root Signature blob，并把它与 shader bytecode 一起嵌入 shader blob。
[D3D12SerializeVersionedRootSignature](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12serializeversionedrootsignature)
进一步明确：shader 内已有 Root Signature 时，可把 compiled shader blob 直接传给
`ID3D12Device::CreateRootSignature`。

DXC 自己也这样测试：`ShaderOpTest.cpp:900-911` 在没有单独创建 Root Signature 时，把刚编译的
完整 shader blob 直接传给 `CreateRootSignature`。所以 runtime 接受的不是 RadRay 私有格式，
而是 D3D12/DXIL 已有的标准 container。

### 本地探针

用 fork `build_radray/Release/bin/dxc.exe` 编译
`modules/render/tests/data/shader_sources/multiple_root_constants.hlsl` 的 `VSMain/vs_6_0`：

| output | bytes | SHA-256 |
|---|---:|---|
| embedded-RS DXIL | 2,992 | `2266C44D573B35C0CFDF7EF55ECB119D52F5E83B6E459193E06DDFFAC9236356` |
| `-Frs` standalone RS | 116 | `970974E7FCFE4FF63C11DAE9869659DF067810C049D0A61543A6BD65EA5E7FB5` |
| `-Qstrip_rootsignature` DXIL | 2,908 | `FF3E51035ADC51B867E780284A088C240CAC98B06823E3419579E64D76AF33A5` |
| stripped compile 的 `-Frs` | 116 | `970974E7FCFE4FF63C11DAE9869659DF067810C049D0A61543A6BD65EA5E7FB5` |

独立 RS 在 strip 前后逐字节相同；说明可以让 stage DXIL 去掉重复的 `RTS0`，同时保留一份稳定的
lane-level compiler output。`-extractrootsignature` 从 shader 后处理得到的 container 与 `-Frs`
只在 DXBC container digest 字段不同（前者为零），`RTS0` payload 相同。因此 artifact 必须指定
唯一 canonical producer；不要混用不同提取流程的整个 wrapper bytes 作为语义 identity。

## 推荐的 artifact 与 compiler contract

### 1. 一次 lane 只发布一份 canonical Root Signature

每个 DXIL stage compile 都尝试取得 `DXC_OUT_ROOT_SIGNATURE`：

- graphics：零个或多个 stage 可以携带 RS，但所有非空 `RTS0` payload 必须逐字节相同；只比较
  128-bit hash 不足以作为 compiler correctness gate。lane 最终发布一份。
- compute：发布 compute entry 的一份。
- fork 继续用全部 active stage resource union 校验 canonical RS；一个 active declaration 必须能按
  register class/register/space 和 visibility 唯一映射到允许的 Root Signature location。
- 如果维持当前“exact-permutation layout”决策，则最终发布的 blob 必须就是 exact Root Signature。
  当前 fork 实际做的是拒绝含 inactive resource 的 declared RS，并没有实现文档所说的自动
  projection；若未来要允许 declared superset 再投影，projection、重新序列化、替换各 stage
  `RTS0` 与 standalone output 必须全部在 compiler 内完成。

若 `[RootSignature]` 是 DXIL residency/static-sampler policy 的权威，DXIL lane 不应在缺失 RS 时由
backend 静默合成另一份。推荐 fail closed；无资源 pass 也显式声明 empty RS。若产品确实需要默认
布局，应由 fork 生成并发布一份标记为 generated 的 canonical RS，而不是让 runtime 猜。

### 2. Wire 与 hash

metadata envelope 增加 DXIL-only `RootSignature` range；SPIR-V 必须为空。该 range 保存完整的
canonical `DXC_OUT_ROOT_SIGNATURE` blob，而不是含 pointer 的 D3D12 struct，也不是只保存
`RTS0` hash。binary shape 改变要求 metadata schema、toolchain/package identity 与 goldens 断代。

建议同时定义 `RootSignatureDigest`，或把 DXIL `PipelineLayoutHash` 明确定义为 canonical Root
Signature bytes 的 digest。Root Signature bytes 必须进入 `GpuArtifactHash`；若 stage DXIL 暂时仍
内嵌 `RTS0`，也要显式作为 layout input，不能只依赖“它碰巧也在 bytecode hash 里”。

当前 `PipelineLayoutHash` 不适合直接拿来去重 native Root Signature：它没有覆盖 root parameter
topology/static-sampler state，却包含 binding name、vertex input 等不属于 D3D12 Root Signature 的
数据。相同 RS、不同 HLSL 名称应能共享 native object；不同 residency/flags/static sampler 则绝不能
因为 binding 坐标相同而误共享。

### 3. 是否 strip stage 内的 `RTS0`

分两步更稳妥：

1. 先保留 stage 内嵌 `RTS0`，新增 lane-level canonical blob并切换 backend 直读；用真实 D3D12
   PSO/draw tests 证明 bytecode、standalone RS 和 CPU binding plan 一致。
2. 再给 stage compile 加 `-Qstrip_rootsignature`，只保留 lane-level 一份，减少 graphics 多 stage
   和 metadata 内的重复。PSO 始终显式传入由 canonical blob 创建的 root signature。

strip 是空间优化，不应与首次 correctness cutover 绑成一个不可拆分变更。

## D3D12 backend 不能只改 `CreateRootSignature`

`CreateRootSignature(blob)` 只产生 native object；命令提交仍必须知道每个 HLSL binding 对应哪个
root parameter。当前 backend 自己生成 descriptor table，所以顺手知道 parameter index 和 table
offset；删除重建后，必须从 canonical blob 恢复这份 CPU plan。

推荐在 `CreatePipelineLayout(DxilShaderArtifactView)` 中：

1. 校验 DXIL artifact schema、toolchain、Root Signature range 与 hash。
2. 用完整 canonical blob调用 `ID3D12Device::CreateRootSignature`。
3. 用 `D3D12CreateVersionedRootSignatureDeserializer` 读取同一 blob，不改变版本、不重新序列化。
4. 将 active binding records 与 deserialized parameters/ranges 匹配，记录：
   root parameter index、descriptor heap kind、table offset、root descriptor kind、root constants range
   和 static-sampler residency。
5. `ShaderParameterSet`/push-constant 提交只消费这份 plan；不再假设 constants 排在前面，也不再
   假设每个 register space 恰好两个 descriptor tables。

这里需要明确 RadRay 支持的 Root Signature 子集。标准 D3D12 允许多个 tables、显式/append offset、
不同 space 的 ranges，甚至让相同 register 在不相交 visibility 下出现于多个 parameters。当前
`GroupIndex == register space` 和单目标 `BindingHandle` 未必能无损表达所有合法 RS。首期建议由 fork
fail closed 地限制为：每个 active HLSL declaration 在 graphics union 中恰好映射到一个 residency
位置；同一 declaration 不允许按 stage 同时落入不同 root parameter/table/static policy。否则 backend
必须让一个 handle fan-out 到多个 root locations。

这种限制是 RadRay authoring contract，不是 backend 重建 Root Signature 的理由。即使受限，native
object 和 parameter order/flags/sampler state 仍应使用 compiler blob原样创建。

### Root Signature version

serialized blob 自带 1.0/1.1 version。当前 D3D12 backend 已直接拒绝不支持 1.1 的 device，所以原样
消费 1.1 blob不会扩大现有硬件要求。若未来支持仅 1.0 的环境，官方
[GetRootSignatureDescAtVersion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12versionedrootsignaturedeserializer-getrootsignaturedescatversion)
可以把 1.1 降为 1.0，但会丢失 1.1 flags。更干净的 artifact policy 是 cook 两个明确版本或提升
最低要求；不要在普通 runtime 路径悄悄降级并产生第二种未入 hash 的 RS。

## Root Signature 去重

### Runtime native-object cache

在每个 `DeviceD3D12` 内建立 cache：

```text
key:   {node mask, canonical RootSignature digest, byte length}
guard: digest 命中后对 canonical blob 做逐字节比较
value: ComPtr<ID3D12RootSignature>
```

Root Signature object 属于 device/node，不能跨 device 全局共享。首期可让 cache 持有到 device
销毁；若以后需要驱逐，再给 cache entry 加显式引用计数。不能只以 128-bit digest 命中即返回，必须
处理理论 hash collision。

`RootSigD3D12`/`PipelineLayout` wrapper 仍应每个 artifact 独立存在，持有：

- cache 返回的共享 `ComPtr<ID3D12RootSignature>`；
- artifact-local binding names、CPU binding plan 与 `BindingHandle` generation；
- root constants 与 parameter-set 提交信息。

因此两个 shader 即使 RS bytes 相同，也只共享 native object；它们不会错误共享 binding name 或
layout generation。PSO cache key 则引用同一个 Root Signature content identity。

### Cook/package 去重

当前正式 artifact publisher/index 尚未实现，不建议为了第一次 cutover 让 raw lane wire 引入跨文件
引用。先容忍“每个 DXIL lane 一份小 RS blob”，runtime cache 已能避免重复 native creation。

publisher 落地后，可建立 package-level Root Signature table：以 canonical compiler output 的内容
digest 寻址，shader artifact 只保存 RootSignatureId/range。发布时仍要 byte-compare 防碰撞，并把
table 与 shader artifact 的完整性关系纳入外层 index。不要让 runtime 在磁盘上扫描所有 shader 来
临时建立隐式依赖。

exact-permutation contract 会自然限制去重率：只有 active layout 完全相同的 variants 才共享。
若未来为了减少 root-signature switching 选择 stable-superset layout，那是新的 layout/variant ADR，
不能通过缓存层把两个不同 serialized RS 强行视为相同。parameter order、flags 或 static sampler state
不同，即使 shader resource 坐标相同，也必须视为不同 RS。

## Vulkan 同步边界

serialized Root Signature 只解决 D3D12 native layout 的保真与创建；它不能直接交给 Vulkan。
fork 仍需按 HLSL declaration identity 把 Root Signature policy投影到 SPIR-V lane：

- static sampler：SPIR-V metadata 必须带完整 immutable sampler state，而不只是当前的 immutable
  bit；D3D sampler register 与 Vulkan set/binding 不能作为关联依据。
- root descriptor：若 RadRay 将其映射为 Vulkan dynamic uniform/storage descriptor，metadata wire
  必须明确携带 residency；当前 binding kind 只会生成非 dynamic 类型，尚未兑现这条桥接。
- RootConstants/push constant：继续承认两 target 的数量、坐标与字节布局可不同；只校验项目定义的
  declaration-level invariant，不把 D3D serialized RS 当 Vulkan pipeline layout。

所以推荐的数据权威关系是：

```text
HLSL declaration identity + RootSignature policy
  -> DXIL: canonical serialized RS 原样消费
  -> SPIR-V: compiler-owned descriptor/push/immutable-sampler records
```

两条 lane 都由 fork 同次 typed Variant operation 发布，但 backend 不互相反射或转换。

## 风险与验收门槛

实施前应固定以下 tests：

- graphics 的 VS/PS 有相同 RS、只有一个 stage 携带 RS、以及非空 RS bytes 不同三种情况；最后一种
  fail closed，前两种产生一份 canonical lane blob。
- descriptor table、root CBV/SRV/UAV、多个 RootConstants、显式 table offset、1.1 flags、精确 static
  sampler state 和 compute RS。
- backend 不调用 `D3D12SerializeVersionedRootSignature`，直接用 artifact blob成功创建 RS/PSO并完成
  draw/dispatch/readback。
- deserialized CPU plan 的 root parameter index/table offset 与实际 command writes 一致；不能只验证
  `CreateRootSignature` 成功。
- 相同 RS、不同 shader/binding name：native object cache 命中，但 wrapper generation 与 name lookup
  独立；不同 parameter order/flags/static sampler 不命中。
- 保留内嵌 `RTS0` 与 strip 后两种 DXIL 都通过真实 D3D12 gate，之后再决定是否正式 strip。
- SPIR-V immutable sampler保留 filter/address/LOD/comparison/border；root-descriptor policy 的 Vulkan
  映射有独立 consumer test。
- metadata range corruption、Root Signature 与 `GpuArtifactHash` 错配、unsupported version、cache hash
  collision bucket 都 fail closed。

## 最终判断

| 问题 | 判断 |
|---|---|
| DXC 是否已经序列化 `[RootSignature]` | 是，既嵌入 DXIL `RTS0`，又提供 `DXC_OUT_ROOT_SIGNATURE` |
| D3D12 能否直接创建 | 是，可直接传 compiled shader 或 standalone RS container |
| RadRay 是否应继续 backend 重建 | 否；重建会丢 residency、顺序、flags 与 sampler state |
| 是否只需给 wire 加一个 range | 否；还必须从同一 blob建立 CPU binding plan并约束可表达的 RS 子集 |
| RS 如何去重 | lane 内一份；device 内按 canonical bytes缓存 native object；publisher 后续做 package-level 内容寻址 |
| 是否共享整个 `PipelineLayout` | 否，只共享 `ID3D12RootSignature`；名称、handle generation、CPU plan 保持 artifact-local |
| 是否能替代 Vulkan metadata | 否，Vulkan 仍消费 target-specific records与 declaration-identity bridge |

综合评估为 **Go**。建议把它作为 DXIL artifact schema 的一次明确断代实施：先保留 embedded
`RTS0` 完成 direct-consumption cutover和 CPU plan验证，再做 strip 与 package-level dedup。最大风险
不是 D3D12 API 或 DXC carrier，而是当前 RHI parameter-set 假设无法表达任意合法 Root Signature；
这个边界应由 compiler fail-closed subset或显式 RHI 扩展解决，不能通过 backend 再造一份 RS 掩盖。

## Primary sources

- [Microsoft: Specifying Root Signatures in HLSL](https://learn.microsoft.com/en-us/windows/win32/direct3d12/specifying-root-signatures-in-hlsl)
- [Microsoft: Creating a Root Signature](https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-a-root-signature)
- [Microsoft: ID3D12Device::CreateRootSignature](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createrootsignature)
- [Microsoft: D3D12SerializeVersionedRootSignature](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12serializeversionedrootsignature)
- [Microsoft: D3D12CreateVersionedRootSignatureDeserializer](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createversionedrootsignaturedeserializer)
- [Microsoft: Root Signature Version 1.1](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-version-1-1)
- [DirectXShaderCompiler `dxcapi.h`, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/include/dxc/dxcapi.h)
- [DirectXShaderCompiler `dxcompilerobj.cpp`, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/tools/clang/tools/dxcompiler/dxcompilerobj.cpp)
- [DirectXShaderCompiler `DxilContainerAssembler.cpp`, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/lib/DxilContainer/DxilContainerAssembler.cpp)
