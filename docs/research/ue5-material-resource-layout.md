> - 适用: 对照 UE5.7.4 材质表达式、uniform expression、shader reflection 与 D3D12/Vulkan pipeline layout
> - 权威: 本文是一次针对 Unreal Engine 源码的只读研究记录，不改变 RadRay 的运行时契约
> - 锚点: `F:\cpp\UnrealEngine\Engine\Source\Runtime\Engine\Private\Materials\MaterialExpressions.cpp`, `F:\cpp\UnrealEngine\Engine\Source\Runtime\Engine\Private\Materials\MaterialCachedData.cpp`, `F:\cpp\UnrealEngine\Engine\Source\Runtime\Engine\Private\Materials\MaterialIRModuleBuilder.cpp`, `F:\cpp\UnrealEngine\Engine\Source\Runtime\Engine\Private\Materials\MaterialIRValueAnalyzer.cpp`, `F:\cpp\UnrealEngine\Engine\Source\Runtime\Engine\Private\Materials\MaterialIRToHLSLTranslator.cpp`, `F:\cpp\UnrealEngine\Engine\Source\Runtime\Engine\Private\Materials\MaterialUniformExpressions.cpp`, `F:\cpp\UnrealEngine\Engine\Source\Runtime\D3D12RHI\Private\D3D12Util.cpp`, `F:\cpp\UnrealEngine\Engine\Source\Runtime\D3D12RHI\Private\D3D12RootSignature.cpp`, `F:\cpp\UnrealEngine\Engine\Source\Runtime\VulkanRHI\Private\VulkanDescriptorSets.cpp`, `F:\cpp\UnrealEngine\Engine\Source\Runtime\VulkanRHI\Private\VulkanLayout.cpp`, `F:\cpp\UnrealEngine\Engine\Source\Runtime\VulkanRHI\Private\VulkanPipeline.cpp`

# UE 材质资源与 layout 研究

## 研究范围

快照为 Unreal Engine `5.7.4`，commit `260bb2e1c5610b31c63a36206eedd289409c5f11`，分支
`release`。研究只读了 `F:\cpp\UnrealEngine`，没有修改或构建 UE；本文件是
`F:\cpp\RadRay` 中唯一新增的研究 Markdown。

问题是：纯 RGB 参数、纹理参数，以及由 Static Switch 选择纹理的材质，最终是否拥有不同的
shader resource 集合和 native binding layout。

## 先分三层

“材质参数存在”不等于“shader 使用了资源”，也不等于“native pipeline layout 有一个槽”。
需要分开看：

1. **参数契约**：材质编辑器/实例公开的 scalar、vector、texture、static switch 名称和值。
2. **有效编译资源集合**：表达式图中从材质输出可达、经过编译和分析后留下的 numeric uniform、
   texture/sampler、SRV/UAV、uniform buffer 等。
3. **native layout**：DXIL/SPIR-V 反射及 shader resource counts/header 被后端量化后形成的
   D3D12 root signature、Vulkan descriptor set layout 和 pipeline layout。

因此，“参数在 MaterialInstance 中存在”本身不能证明它会占用 shader binding。

## 表达式到资源集合

### Legacy translator

`UMaterialExpressionVectorParameter::Compile` 在
`Engine/Source/Runtime/Engine/Private/Materials/MaterialExpressions.cpp:7986-8000`：普通路径
走 `FMaterialCompiler::VectorParameter`，启用 Custom Primitive Data 时走
`CustomPrimitiveData`；两者都不是 texture expression。`FMaterialCompiler` 的接口在
`Engine/Source/Runtime/Engine/Public/MaterialCompiler.h:298-307` 区分
`TextureSample` 与 `TextureParameter`，在 `:472-473` 区分 `StaticBool` 与
`StaticBoolParameter`。

`FHLSLMaterialTranslator::AccessUniformExpression` 位于
`Engine/Source/Runtime/Engine/Private/Materials/HLSLMaterialTranslator.cpp:4286`：

- numeric uniform 读取 `Material.PreshaderBuffer[...]`；
- texture code chunk 必须对应 `FMaterialUniformExpressionTexture` 或 external texture expression；
- translator 在 `:2371-2381` 把实际收集到的 uniform texture expressions 写入
  `FMaterialCompilationOutput::UniformExpressionSet.UniformTextureParameters`。

所以一个只提供 RGB 值的 Vector Parameter 是 Material numeric uniform，不是纹理资源。UE
内部仍以 float4 形状表示 vector parameter；`UMaterialExpressionVectorParameter::Build`
在 IR 路径的 `MaterialExpressionsIR.cpp:396-405` 也创建一个 vector 值并提供 XYZ、单分量和
完整 vector 输出。

### Material IR

IR builder 在 `MaterialIRModuleBuilder.cpp:86-125` 初始化 `FMaterialIRValueAnalyzer`，将
`FStaticParameterSet` 交给 emitter。参数 emission 在
`MaterialIREmitter.cpp:1131-1176`：

- Scalar -> float scalar uniform；
- Vector/DoubleVector -> 4-component numeric uniform；
- Texture/Font -> texture uniform parameter，并带 sampler type；
- StaticSwitch -> 在 emitter 中解析为 constant bool。

IR analyzer 只从各 entry point 的 outputs 反向遍历实际依赖，见
`MaterialIRModuleBuilder.cpp:511-617`。随后 `Step_LinkInstructions` 只把从 outputs 可达的
instruction 放入 entry-point blocks，见 `MaterialIRModuleBuilder.cpp:619-695`；IR 到 HLSL
只 lower 这些已 link 的 outputs/instructions，见 `MaterialIRToHLSLTranslator.cpp:398-450`。
纹理 uniform 只有在 analyzer 访问到时才通过 `MaterialIRValueAnalyzer.cpp:175-219` 注册进
`UniformExpressionSet`。

这使得“被构造过的 MIR value”与“有效资源集合”仍然不同：未被材质输出使用的值不会因为
仅存在于模块中就自动成为 shader resource。IR 的 texture lowering 还会在
`MaterialIRToHLSLTranslator.cpp:1948-1952` 断言 texture uniform 已由 analysis 注册，说明
active analysis 是 HLSL resource reference 的前置条件。

### `ReferencedTextures` 不是 binding 集合

UE 另有一个图级/缓存级对象索引 `MaterialCachedData::ReferencedTextures`，声明在
`Engine/Source/Runtime/Engine/Public/MaterialCachedData.h:337-339`。缓存构建会遍历表达式的
referenced texture 并 `AddUnique`，见 `MaterialCachedData.cpp:532-552`。因此 Static Switch
两侧即使只有一侧 active，另一侧的 texture object 仍可能出现在这个数组中。

这个数组的用途是让编译器和 material 数据通过稳定的 texture index 找到源对象；legacy
translator 使用 `Material->GetReferencedTextures()` 取得对象，见
`HLSLMaterialTranslator.cpp:8906-8916`，IR emitter 也把 source texture 转成该数组的 index，
见 `MaterialIREmitter.cpp:1246-1257`。它不是 `UniformExpressionSet` 的 texture list，不能
把“图级 ReferencedTextures 包含 A、B”解释成“shader 同时有 A、B 两个 texture binding”。

## 三个具体案例

| 材质图 | 有效编译资源 | bindful 下的典型 layout 影响 |
|---|---|---|
| 纯 RGB / Vector Parameter | numeric uniform；使用时占 Material numeric/preshader 数据，不产生 texture uniform expression | 不新增 texture sample 的 SRV；Material UB metadata 仍包含通用 wrap/clamp sampler 成员，实际 stage binding 还要看生成 HLSL 是否引用它们 |
| Texture Sample Parameter | 一条 texture uniform expression；普通纹理通常需要 texture resource + sampler | 通常新增一对 SRV/texture 与 sampler 绑定；具体 native register/set 由 shader reflection/header 决定 |
| Static Switch 选择纹理 A 或 B | 静态值为真时只保留 A，假时只保留 B；两边纹理不应同时成为 active binding，虽然图级 `ReferencedTextures` 仍可能含 A、B | 若两边资源数量/类型/stage 使用相同，Material UB 与 bindful layout 通常相同并可复用；若形状不同，Vulkan layout 会随 bindings 改变，D3D12 仍先经过 counts 量化，可能复用 |

### Static Switch 的关键差异

Legacy 路径中 `UMaterialExpressionStaticSwitch::Compile` 位于
`MaterialExpressions.cpp:8755-8780`：runtime `MCT_Bool` 会在 `:8763-8766` 编译两侧并调用
`DynamicBranch`；否则 `GetStaticBoolValue` 在 `:8768-8779` 成功后只编译选中的输入。

对应的 `FHLSLMaterialTranslator::DynamicBranch` 在
`HLSLMaterialTranslator.cpp:6830`：条件为动态值时 A、B 两侧都被保留在生成表达式中，
因此两侧的资源都可能进入有效集合。`GetStaticBoolValue` 在 `:9372` 只接受
`MCT_StaticBool`，普通 runtime bool 不能伪装成 static switch。

IR 路径的 `UMaterialExpressionStaticSwitch::Build` 在
`MaterialExpressionsIR.cpp:471-477` 先将条件转为 constant bool，再只调用选中输入。
`UMaterialExpressionStaticSwitchParameter::Build` 在 `:479-489` 有一个有意的细节：它会先
读取 A、B 两侧，以便两侧都缺输入时仍向用户报错，但输出只返回选中的 value。之后
`MaterialIRModuleBuilder::Step_AnalyzeIRGraph`、`Step_LinkInstructions` 和 HLSL lowering 都
从 entry-point outputs 追踪 active uses；未选 value 不会成为 active texture resource，虽然
其对象仍可能已在图级 `ReferencedTextures` 中。

结论：Static Switch 是编译期 permutation 选择；dynamic branch 才是同一个 shader 中保留
两侧资源的路径。

## Material uniform expression 与运行时 fallback

`FUniformExpressionSet::CreateBufferStruct` 位于
`Engine/Source/Runtime/Engine/Private/Materials/MaterialUniformExpressions.cpp:393-705`。
它为 Material uniform buffer 生成 constant data 和 resource metadata。`FillUniformBuffer`
在 `:1030-1400` 之后按有效 expression 集合填充运行时资源。

这里要区分两个事实：`UniformTextureParameters` 的 2D/Cube/Array/Volume 成员按实际数量
生成 texture + sampler metadata，见 `:523-600`；但 Material UB 还会无条件追加
`Wrap_WorldGroupSettings` 与 `Clamp_WorldGroupSettings` 两个通用 sampler metadata，见
`:680-684`。因此“纯 RGB 不产生 texture uniform binding”是表达式/active texture contract
的结论，不应扩大成“Material UB metadata 绝不含 sampler”。

纹理参数的查找链是：

- `FUniformExpressionSet::GetTextureValue`，`MaterialUniformExpressions.cpp:876-891`，先处理
  editor transient override，再从 `FMaterialRenderContext` 查询参数值；
- `FMaterialTextureParameterInfo::GetGameThreadTextureValue`，`:2078-2084`，没有 instance
  override 时回退到 Material 的 indexed referenced texture；
- `FMaterialRenderProxy::GetTextureValue`，`MaterialRenderProxy.cpp:218-226`，只是从 proxy
  的参数值中取出 texture；实际父材质/实例链由具体 proxy/interface 实现继续解析。

编译期默认 texture 不是 layout 改变，而是参数表达式的默认对象：

- 2D parameter 的 `SetDefaultTexture` 使用
  `/Engine/EngineResources/DefaultTexture.DefaultTexture`，见
  `MaterialExpressions.cpp:4397-4400`；
- Cube parameter 的 `SetDefaultTexture` 使用
  `/Engine/EngineResources/DefaultTextureCube.DefaultTextureCube`，见
  `MaterialExpressions.cpp:4479-4482`；
- parameter compile 在 `MaterialExpressions.cpp:4007-4036` 先验证 texture 和 sampler type，
  再调用 `CompileTextureSample`。

运行时若选中的 texture 没有有效 resource、TextureReferenceRHI 或类型不匹配，仍然填充同一
个已编译的 resource slot，而不是删掉 slot：

- Standard2D -> `GWhiteTexture`，`MaterialUniformExpressions.cpp:1305-1398`；
- Cube -> `GWhiteTextureCube`，`:1401-1439`；
- 2D array / cube array -> black array fallback，`:1442-1517`；
- Volume -> `GBlackVolumeTexture`，`:1520-1557`；
- Sparse volume -> 先用 black volume/uint volume 资源初始化，再用有效 render resources
  覆盖，`:1561-1597`；
- external texture lookup 失败 -> `GWhiteTexture`，`:1600-1631`。

普通 2D 的 sampler 也可能由 `SamplerSource` 换成 world wrap/clamp sampler，见
`MaterialUniformExpressions.cpp:1363-1379`。这改变绑定到 slot 的 sampler value，不改变
shader 的 resource shape。

一个需要调用方注意的 API 事实是 `UMaterialInstance::SetTextureParameterValueInternal`
在 `MaterialInstance.cpp:4244-4281` 只有 `Value` 非空时才写入 `ParameterValue` 并发送更新。
传 `nullptr` 不会把已有非空纹理按普通 setter 逻辑清掉；清除/恢复应走专门的 override 或
参数链机制。即使参数解析最终返回空，填充阶段仍会用上述 typed fallback 保证 native slot
有有效 RHI 资源。

## 从编译输出到 native layout

中间链路是：

```text
material expression graph
  -> legacy HLSL translator or Material IR
  -> FMaterialCompilationOutput
  -> FUniformExpressionSet / Material uniform buffer metadata
  -> shader compiler reflection/header
  -> backend resource counts or descriptor bindings
  -> native pipeline layout
```

`FShaderParameterMap` 在 `Engine/Source/Runtime/RenderCore/Public/ShaderCore.h:322-348` 保存
编译器发现的参数分配。`FShaderCompilerResourceTable` 在
`Engine/Source/Runtime/RenderCore/Public/ShaderCompilerCore.h:120-151` 保存 texture/SRV/sampler
映射。`BuildResourceTableMapping` 在
`Engine/Source/Developer/ShaderCompilerCommon/Private/ShaderCompilerCommon.cpp:126-164`
只把同时出现在 resource table 和 parameter map 中的资源建立有效映射。因此 metadata 中
存在一个 resource member，不等于每个 shader stage 的最终 reflection 都有该 binding。

DXC 反射在 `Engine/Source/Developer/Windows/ShaderFormatD3D/Private/D3DShaderCompilerDXC.cpp:1126-1160`
提取 resource counts；`FShaderCodePackedResourceCounts` 位于
`Engine/Source/Runtime/RenderCore/Public/ShaderCore.h:760-782`，记录 SRV、sampler、CBV、UAV
数量以及 bindless/root-constant flags。

## D3D12

当前 Windows 配置在
`Engine/Source/Runtime/D3D12RHI/Private/Windows/WindowsD3D12RHIDefinitions.h:27` 将
`USE_STATIC_ROOT_SIGNATURE` 定义为 `0`。默认 root signature 因而按 bound shader state 的
实际 resource counts 量化。

`D3D12Util.cpp:1212-1227` 调用
`FD3D12QuantizedBoundShaderState::InitShaderRegisterCounts`；具体量化在
`D3D12Util.cpp:1136-1178`：Tier 1/2 会按 power-of-two round-up，Tier 3 以最大表大小表示
“有该类资源”。
`D3D12RootSignature.cpp:162-347` 再按各 shader stage 的 counts 创建 descriptor ranges：

- `ShaderResourceCount > 0` 才创建 SRV table；
- 超过 root CBV 数量的 constant buffers 才进入 CBV table；
- `SamplerCount > 0` 才创建 sampler table；
- `UnorderedAccessCount > 0` 才创建 UAV table。

所以在当前 bindful 默认下，纯 RGB 参数和“选择后只剩 numeric branch”的 shader 不会因为
材质参数名字而新增 texture sample 的 descriptor table；纹理 sample 进入有效 DXIL 资源后
才会增加对应 SRV/sampler counts。但两份不同的原始 resource 集合如果量化后的
`FShaderRegisterCounts`、stage visibility、flags 等相同，仍可以复用同一个 root signature。
`FD3D12QuantizedBoundShaderState` 的比较字段见 `D3D12Util.h:117-157`，root signature manager
按 QBSS 缓存/复用见 `D3D12RootSignature.cpp:841-866`。这正是“resource 集合不同”不能直接
推出“D3D12 root signature 必然不同”的原因。

Bindless shader 会通过 `FD3D12ShaderData::UsesBindlessResources/Samplers` 设置
`bUseDirectlyIndexedResourceHeap` 和 `bUseDirectlyIndexedSamplerHeap`，见
`D3D12Util.cpp:1196-1208`。在当前 `USE_STATIC_ROOT_SIGNATURE == 0` 配置下，bindless
shader 仍经过 dynamic QBSS/root-signature 路径，见 `D3D12Util.cpp:1363-1418`，但资源/采样
访问使用直接索引的全局 descriptor heaps；开启 static root signature 时则选择全局的
`StaticBindless*RootSignature`，见 `:1334-1349`。无论哪种配置，bindless 不是按每个纹理
对象生成一套材质专属 texture slots；静态 BSS 路径还要求不混用 bindless 与 bindful shader，
见 `:1290-1328`。

## Vulkan

Vulkan shader header 在
`Engine/Source/Runtime/VulkanRHI/Public/VulkanShaderResources.h:19-39` 保存：

- `Bindings`：binding index 对应的 `VkDescriptorType`；
- `UniformBufferInfos`：uniform buffer layout hash、resource presence 等信息；
- `PackedGlobalsSize` 与绑定的 uniform buffer 元数据。

bindful descriptor set layout 由
`VulkanDescriptorSets.cpp:14-97` 的 `FinalizeBindings` 根据各 shader header 的 bindings
生成。随后 `VulkanLayout::Compile` 在
`Engine/Source/Runtime/VulkanRHI/Private/VulkanLayout.cpp:24-45` 将 descriptor set handles
传给 `vkCreatePipelineLayout`。

Vulkan layout cache 的 key 是 descriptor set layout 形状、stage info、bind point 等，而不是
texture object identity：`VulkanDescriptorSets.h:117-153` 定义比较，
`VulkanPipeline.cpp:2196-2210` 按 `FVulkanDescriptorSetsLayoutInfo` 查找/复用 layout。因此
两个 static permutation 只换了不同纹理对象、但 active resource 的 descriptor type/count 和
stage 使用相同，通常共享同一个 Vulkan descriptor/pipeline layout；RGB vs texture 或资源
类型/stage 使用改变时，bindings/hash 才会改变。

graphics pipeline 在 `VulkanPipeline.cpp:2426-2465` 统计所有 active shader 的
`UsesBindless()`，禁止 graphics pipeline 混用 bindless 和非 bindless shader。compute pipeline
在 `:2742-2770` 做同样的 descriptor layout/header/layout 连接。

Vulkan bindless 不按每个材质重新创建同形状的普通 descriptor set：
`VulkanLayout.cpp:30-34` 直接使用 bindless descriptor manager 的 pipeline layout。
`VulkanCommon.h:21-26` 说明默认按资源类型预留 9 个 descriptor set；各类 set index 在
`:102-135` 的 `VulkanBindless::EDescriptorSets` 中定义。材质资源的变化进入 bindless
descriptor 数据，而不是把 material parameter name 变成新的 per-material descriptor set
layout。

## 不用“Unity same / UE different”概括

“Unity same / UE different”把三个不同问题压成了一个二分法，结论不严谨。正确比较必须
分别问：材质图契约是否包含 texture 参数；active 编译图是否产生 texture/sampler resource；
后端是否按资源形状精确建 layout、按量化 key 复用，或直接使用 bindless global layout。
在 UE 内部，三个答案也可能分别是“有/无”“有/相同数量”“layout 相同/不同/复用”，不能
仅凭两个材质引用了不同纹理对象就作出 layout 结论。

## 对 RadRay 绑定设计的含义

UE 的结论不是“所有 material parameter 都有固定 slot”，而是：

1. Numeric parameter 只需要 numeric uniform contract；纯 RGB 不需要 texture uniform/SRV contract，
   但不能把 Material UB 中的通用 sampler metadata 误读成 active texture sample binding。
2. Texture sample expression 需要 texture/sampler contract；空值只替换 slot 的运行时资源。
3. Static Switch 纹理分支会让不同 static permutation 产生不同的有效资源集合，但只有资源
   数量/类型/stage 使用改变时才必然改变 layout；同形状 permutation 可以共享 layout。
4. Dynamic branch 会把两侧资源一起留在同一 shader 的有效集合中。
5. D3D12 bindful 由反射 counts 量化后按 QBSS 复用 root signature；Vulkan bindful 按精确
   descriptor bindings/hash 复用 layout；bindless 资源访问使用全局 descriptor layout/heaps，
   但 D3D12 当前 dynamic-root 配置仍有量化 root signature 这一层。

这与 RadRay 当前 `docs/architecture/shader-pipeline.md:44-59` 的 manifest ABI 模型形成
明确设计选择：如果 RadRay 保持“variant/target 无关的 SharedPipelineLayout”，Static Switch
的不同纹理资源集合不能直接照搬 UE 的“每个 permutation 反射后缩小 layout”，必须在 trace
期生成固定 superset layout，或者把 layout identity 纳入 variant；同时应允许同形状 variant
共享 layout。两者都应在实现前写入
RadRay 自己的 binding ABI，而不能让运行时 texture override 决定 layout。

## 结论

对同一材质族：

- 纯 RGB 与 active Texture Sample 的编译后有效资源集合不同：前者是 numeric uniform，后者
  至少有 texture/sampler resource contract；图级 `ReferencedTextures` 不能代替这个判断。
- Static Switch 的 A/B 纹理分支不是运行时“把纹理换掉”；静态值改变会选择不同编译图，
  选中分支决定有效资源集合。两边若资源形状相同，Material UB 和 native layout 通常可相同；
  运行时空纹理只触发 typed fallback，不改变 layout。
- Legacy translator 与 Material IR 在这个问题上的核心语义一致；IR 的两侧预取只是为了
  诊断输入错误，最终 analysis/link/HLSL 都从 material output 的可达图出发。
- D3D12 bindful 依据 DXIL resource counts 量化并缓存 root signature；Vulkan bindful 依据
  shader header bindings 精确构建并缓存 descriptor set/pipeline layout；两者都不以
  MaterialInstance 参数数组的全量内容直接决定 layout。
