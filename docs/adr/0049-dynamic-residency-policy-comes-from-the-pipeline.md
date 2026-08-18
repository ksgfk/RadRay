# ADR-0049 dynamic buffer residency 由 pipeline 策略提供，per-object 数据不用 StructuredBuffer

状态: 生效
日期: 2026-08
影响: `modules/render/include/radray/render/backend_shader_artifact.h`、
`modules/render/src/shader_artifact.cpp`、内置 `ForwardPipeline` 与其执行器、
`modules/runtime/include/radray/runtime/gpu_resource.h` 的 arena 使用方式
窄化 [ADR-0043](0043-runtime-device-bridges-to-target-typed-shader-artifact.md) 的桥输入面

## 背景

per-object 数据（local-to-world 等）每 draw 都不同。朴素做法是每 draw 建一个
`ShaderParameterSet` 并写 descriptor —— 在几百个物体的场景里这是不可接受的
descriptor 写入量与分配量。

RHI 已经为此准备好了全部机制，但**没有任何生产者**：

- `ShaderParameterBindingType` 有 `DynamicCBuffer / DynamicBuffer / DynamicRWBuffer` 三个成员。
- `BindShaderParameterSet` 有 `std::span<const ShaderParameterDynamicOffset> dynamicOffsets` 形参。
- D3D12 把 dynamic 类型映射成 root descriptor（`SetGraphicsRootConstantBufferView` 等，
  连 descriptor heap 槽位都不占）。
- Vulkan 把它映射成 `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` / `STORAGE_BUFFER_DYNAMIC`
  并下发 `pDynamicOffsets`。

而 `modules/render/src/shader_artifact.cpp` 把 wire binding type code `1` 一律映射为
`ShaderParameterBindingType::CBuffer`，永远不会产出 `DynamicCBuffer`。因此这条路今天走不通。

CONTEXT.md 已经为这件事命名：**Residency** —— "一个 DXIL binding 经 descriptor table 访问，
还是作为 root descriptor 直接绑定的 layout policy"，并记录了当前状态："Implicit D3D12 Root
Signature 的当前 fallback 统一使用 descriptor table"。所以这不是新概念，而是给 implicit
fallback 的 residency 增加一个由调用方选择的维度。

另一条候选路线是 per-object StructuredBuffer：整帧一个 SB，每 primitive 一个元素，draw 时传
元素索引。它需要一个索引通道进 shader，而 ADR-0016 规定 SPIR-V 每个 source 最多一个 active
push-constant block —— 索引会占掉它；用 `SV_InstanceID` + `firstInstance` 则占掉 instancing。

## 决策

**per-object 与 per-view 数据使用 dynamic constant buffer，binding 的 residency 由 pipeline
以显式策略提供。第一期不使用 per-object StructuredBuffer。**

### 1. 数据路径

1. 一帧内所有 primitive 的 per-object 数据按 ADR-0045 逐个打包，全部落进同一个
   per-frame `DynamicCBufferArena`。`HostWriteBatch` 已把写入范围合并后一次 flush ——
   批量上传由现有机制提供，本决策不新增上传路径。
2. 每个 `(layout, flight)` 只建**一个** per-object parameter set，一次绑定整个 arena buffer，
   之后不再写它。
3. 每个 draw 只做 `BindShaderParameterSet(objectGroup, set, {{binding, byteOffset}})`。
   零 descriptor 写入、零 per-draw set 分配。
4. per-view 同理。per-material 的数值参数也走 arena + dynamic 绑定；纹理与 sampler 无法用
   dynamic offset 表达，写在 material 常驻的 set 里，仅在纹理变更时按 flight 数轮转重建。

### 2. residency 策略入口

`CreateBackendShaderArtifact` 增加 layout policy 入参，携带"哪些 group 使用 dynamic buffer
binding"。策略由 pipeline 提供，与它的 `BindingGroupPlan`（ADR-0047）同源。
`shader_artifact.cpp` 按策略把 wire type code `1` 映射为 `CBuffer` 或 `DynamicCBuffer`。

两条 fail-closed 规则：

- **作者写了 explicit `[RootSignature]` 时，策略必须使 layout 创建失败，而不是被静默忽略。**
  residency 是作者 policy（CONTEXT.md `Residency`、ADR-0035），引擎不得改写作者的 serialized
  Root Signature，也不得在 explicit 路径下假装策略生效。
- **策略引用的 group 在 artifact layout 中不存在时失败。** 不跳过、不猜测。

这加宽了 ADR-0043 那座动态桥的输入面（原本只有 `ShaderArtifactDecodeOptions`）。桥的其余
不变量不变：device/request/envelope target 三方一致才进 typed 入口，失败不尝试另一 lane，
不新增公共 layout descriptor，caller 不得构造第二份 binding layout。

## 放弃的方案及代价

- **per-object StructuredBuffer + 索引**。内存打得更紧（无 256 字节对齐浪费），且是
  instancing / GPU-driven 的最终形态，UE 的 `PrimitiveSceneData` 就是这个形状。但收益要等到
  instancing 才兑现，代价现在就付：索引通道会占掉 SPIR-V 唯一的 push block 或
  `SV_InstanceID`；HLSL 侧从 `ConstantBuffer<T>` 变成 `StructuredBuffer<T>` + 手动索引，
  这是渗透到每个 pass 的 authoring 约定，反悔成本高；还需要给 `DeviceDetail` 补
  storage buffer offset alignment（Vulkan 同时要求 `offset % minStorageBufferOffsetAlignment == 0`
  与 `offset % StructureByteStride == 0`），并给 arena 加 SRV usage。
  走 dynamic CB 时这些前置全部不需要，而将来换 SB 只是替换绑定机制与一条 HLSL 声明，
  ADR-0045 的打包器一行不动。
- **每 draw 一个 parameter set，配一个 per-flight set 池**。不需要 residency 策略，
  RHI 一行不改。代价是每 draw 一次 descriptor 写入加一次池分配，以及一个新的池化基础设施 ——
  而 dynamic 路径把这两者一并消掉。
- **render 层内置策略（例如"group index ≥ 2 即 dynamic"）**。不加参数，桥的输入面不变。
  但这把某条具体 pipeline 的 group 约定硬编码进 render 层，直接违反 ADR-0047 的
  "group 语义属于具体 pipeline"。
- **explicit `[RootSignature]` 下静默忽略策略**。调用方不用关心两条路径的差异。但那会让
  "我要求了 dynamic 却拿到 descriptor table"变成一个无声的性能与语义差异，
  而 ADR-0035 已确立 explicit 路径下不得回退到 implicit。

接受的代价：D3D12 的 CB 偏移必须 256 字节对齐，一个 64 字节的 local-to-world 会占 256 字节。
一千个物体约 256KB/帧。Vulkan 的 `maxDescriptorSetUniformBuffersDynamic` 下限为 8，
所以 dynamic CB binding 数量有限 —— view + material + object 够用，不能滥用。

## 必须保持为真

- 内置执行器的 per-draw 路径不创建 `ShaderParameterSet`，也不调用 `Set` 写 descriptor；
  只调用 `BindShaderParameterSet` 并附 dynamic offset。
- `shader_artifact.cpp` 产出 `DynamicCBuffer` 的唯一依据是调用方传入的 residency 策略；
  不存在按 group index 或 binding 名推断的内置规则。
- explicit serialized Root Signature 与非空 residency 策略并存时，layout 创建失败。
- 策略引用不存在的 group 时失败，不跳过该项。
- per-object 数据不使用 `StructuredBuffer`；`ShaderBufferBinding::StructureByteStride` 在
  per-object 与 per-view 路径上恒为 0。
- HLSL 侧 per-object/per-view 声明仍是 `ConstantBuffer<T>`，不含手动索引。
- ADR-0043 的三方 target 一致性检查不因新增策略参数而被绕过。
