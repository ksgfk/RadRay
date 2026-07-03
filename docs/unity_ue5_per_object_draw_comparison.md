# Unity SRP 与 UE5 逐物体绘制对比

本文记录 Unity SRP/URP 与 UE5 在“逐物体绘制”上的共同结构和差异。这里的“逐物体绘制”不是指某个 pass 在 CPU 上遍历所有对象,而是指一个 render pass 最终发出 mesh draw 时,如何决定 draw list、如何选择 shader/material、如何绑定 VB/IB、逐物体数据、材质数据和光照相关数据。

关键结论:

```text
Unity SRP:
  C# RenderPass 构造 RendererList 描述符
  native renderer backend 展开 Renderer/Material/Mesh 并发出 draw

UE5:
  FPrimitiveSceneProxy 输出 FMeshBatch
  FMeshPassProcessor 把 FMeshBatch 显式编译成 FMeshDrawCommand
  FMeshDrawCommand 直接携带接近 RHI draw 所需的数据
```

两者都把“pass 要画哪些 primitive”和“一个 primitive 最终如何绑定 draw 数据”分层。区别在于 Unity 把低级逐物体绑定隐藏在 native engine 内,而 UE5 把大部分 draw command 构建逻辑暴露在 Renderer C++ 源码里。

---

## 1. Unity SRP 的逐物体绘制路径

URP object pass 的核心路径是:

```text
ScriptableRenderPass
  -> FilteringSettings
  -> DrawingSettings
  -> RenderStateBlock
  -> RendererListParams(CullingResults, DrawingSettings, FilteringSettings)
  -> RenderGraph.CreateRendererList
  -> cmd.DrawRendererList(rendererList)
  -> Unity native backend 逐 renderer 发出 draw
```

以 `DrawObjectsPass` 为例:

- `Init()` 写入 `ShaderTagId` 列表,默认包含 `SRPDefaultUnlit`、`UniversalForward`、`UniversalForwardOnly`。
- `FilteringSettings` 指定 render queue、layer mask、batch layer 等过滤条件。
- `RenderingUtils.CreateDrawingSettings(...)` 指定 shader tag、排序、`perObjectData`、`mainLightIndex`、instancing 等。
- `CreateRendererListWithRenderStateBlock(...)` 把 `CullingResults`、`DrawingSettings`、`FilteringSettings`、`RenderStateBlock` 合成 `RendererListParams`。
- Execute 阶段设置少量 pass global/keyword 后调用 `cmd.DrawRendererList(rendererList)`。

Unity C# SRP 层到这里就结束。`DrawRendererList` 之后的工作在 Unity native engine 中完成:

```text
RendererList 内的 renderer
  -> 找 renderer 的 mesh/submesh
  -> 找 renderer 的 material
  -> 按 DrawingSettings.shaderTagId 选择 shader pass
  -> 绑定 VB/IB
  -> 绑定 UnityPerMaterial
  -> 按 DrawingSettings.perObjectData 填 UnityPerDraw
  -> 发出底层 draw
```

所以 Unity pass 可以不知道 VB/IB/material 细节,不是因为这些数据不需要,而是因为这些数据归 `Renderer`、`Mesh`、`Material` 和 native backend 管理。

---

## 2. UE5 的逐物体绘制路径

UE5 的路径更显式:

```text
UPrimitiveComponent
  -> CreateSceneProxy()
  -> FPrimitiveSceneProxy
  -> FPrimitiveSceneInfo
  -> static path: DrawStaticElements(PDI) 生成并缓存 FStaticMeshBatch
  -> dynamic path: GetDynamicMeshElements(...) 每帧生成 FMeshBatch
  -> visibility/relevance 阶段按 view/pass 收集可见 mesh
  -> FSceneRenderer::SetupMeshPass
  -> FPassProcessorManager::CreateMeshPassProcessor
  -> FMeshPassProcessor::AddMeshBatch
  -> FMeshPassProcessor::BuildMeshDrawCommands
  -> FMeshDrawCommand
  -> FParallelMeshDrawCommandPass::DispatchDraw / Draw
  -> FMeshDrawCommand::SubmitDraw
  -> RHI DrawIndexedPrimitive / DrawPrimitive
```

重要结构:

| UE5 结构 | 作用 |
|----------|------|
| `FPrimitiveSceneProxy` | game/component 数据在 render thread 的镜像,负责产出 mesh 描述 |
| `FPrimitiveSceneInfo` | renderer 内部 primitive 状态,一对一关联 `FPrimitiveSceneProxy` |
| `FMeshBatch` | 一个 mesh section/material 的 draw 描述,还不是最终 RHI command |
| `FMeshBatchElement` | index buffer、first index、primitive count、instance count、primitive UB 等 element 级数据 |
| `FStaticMeshBatchRelevance` | static mesh 在各 pass 的相关性和缓存信息 |
| `FViewCommands` | 某个 view 下,各 mesh pass 的可见 draw command/build request |
| `FMeshPassProcessor` | 把 `FMeshBatch` 转换成 `FMeshDrawCommand` 的 pass-specific builder |
| `FMeshDrawCommand` | 接近 RHI 的 draw command,包含 PSO、shader binding、vertex streams、index buffer、draw args |
| `FVisibleMeshDrawCommand` | per-view 可见 draw command payload,包含排序、primitive id、culling payload |

`FMeshBatch` 是 UE5 的关键中间层。它已经明确携带:

```text
VertexFactory
MaterialRenderProxy
LCI(light cache interface)
Elements[]
  IndexBuffer
  FirstIndex
  NumPrimitives
  NumInstances
  BaseVertexIndex
  MinVertexIndex / MaxVertexIndex
  PrimitiveUniformBuffer / PrimitiveUniformBufferResource
  IndirectArgsBuffer
LODIndex
MeshIdInPrimitive
CastShadow
bUseForMaterial
bUseForDepthPass
bUseAsOccluder
DepthPriorityGroup
Type(primitive topology)
```

这意味着 UE5 的 pass processor 不需要再回到 `UStaticMeshComponent` 或 `USkeletalMeshComponent` 查数据。proxy 已经把渲染线程需要的几何、材质、element 参数压成 `FMeshBatch`。

---

## 3. Static mesh 与 dynamic mesh 两条入口

UE5 把 primitive 的 mesh 来源分为 static path 和 dynamic path。

### 3.1 Static path

static path 用于可以在 primitive 加入 scene 时缓存的 mesh:

```text
FPrimitiveSceneInfo::AddStaticMeshes
  -> Proxy->DrawStaticElements(&BatchingSPDI)
  -> PDI->DrawMesh(FMeshBatch)
  -> FStaticMeshBatch 存入 PrimitiveSceneInfo->StaticMeshes
  -> Scene->StaticMeshes 增加引用
  -> CacheMeshDrawCommands
```

`DrawStaticElements` 只在 scene proxy 创建后调用一次。静态 mesh 的 `FMeshBatch` 和部分 pass 的 `FMeshDrawCommand` 可以缓存,后续每帧只根据 view visibility/relevance 把缓存 command 引到 `FViewCommands`。

### 3.2 Dynamic path

dynamic path 用于需要每帧、每 view family 生成的 mesh:

```text
visibility 阶段发现 primitive dynamic relevant
  -> Proxy->GetDynamicMeshElements(Views, ViewFamily, VisibilityMap, Collector)
  -> Collector.AddMesh(ViewIndex, MeshBatch)
  -> View.DynamicMeshElements
  -> ComputeDynamicMeshRelevance
  -> SetupMeshPass 时为对应 pass 现场 BuildMeshDrawCommands
```

典型动态来源包括 skeletal mesh 的某些路径、粒子、debug/editor mesh、view-dependent mesh、不能缓存 shader binding 的 mesh 等。

### 3.3 两条路径进入同一个 pass processor

无论 static 还是 dynamic,最终都会进入:

```text
FMeshPassProcessor::AddMeshBatch(const FMeshBatch&, BatchElementMask, PrimitiveSceneProxy, StaticMeshId)
```

差别是:

- static mesh 可能已经有 cached `FMeshDrawCommand`,view 阶段只追加 `FVisibleMeshDrawCommand`;
- dynamic mesh 每帧通过 pass processor 生成 `FMeshDrawCommand`;
- static mesh 若不支持缓存,也会作为 dynamic build request 在本帧生成 command。

---

## 4. UE5 pass 如何筛选 primitive

Unity 的 pass 筛选主要通过 `FilteringSettings + DrawingSettings.shaderTagId + CullingResults`。UE5 的筛选分散在三个阶段:

```text
1. view visibility/relevance
   - primitive 是否对 view 可见
   - primitive view relevance: static/dynamic/relevant pass flags

2. mesh relevance/pass relevance
   - FStaticMeshBatchRelevance
   - View.DynamicMeshElementsPassRelevance
   - FMeshPassMask

3. pass processor 内部 AddMeshBatch/TryAddMeshBatch
   - pass-specific material/domain/blend/shadow/depth 条件
   - 选择 shader permutation / lightmap policy / render state
```

例子:

- Base pass 先看 `MeshBatch.bUseForMaterial`,再检查 material blend/domain、`PrimitiveSceneProxy->ShouldRenderInMainPass()`、是否属于默认 opaque pass、translucency pass 类型等。
- Depth pass 先看 `MeshBatch.bUseForDepthPass`,再排除 translucent,检查 `ShouldRenderInDepthPass()`、material domain、opaque/default pass 条件、occluder flag、early-z 策略、velocity 相关策略。
- Shadow pass 会看 `CastShadow`、shadow relevance、shadow subject/caster 关系,并构建 shadow depth pass command。

这与 Unity 的关系可以这样看:

| 概念 | Unity SRP | UE5 |
|------|-----------|-----|
| 候选 primitive 集 | `CullingResults` | view visibility map / primitive relevance |
| pass 过滤 | `FilteringSettings` | pass relevance + `FMeshBatch` flags + pass processor 条件 |
| shader pass 匹配 | `ShaderTagId(LightMode)` | `EMeshPass` 对应的 shader class/permutation 选择 |
| render state override | `RenderStateBlock` | `FMeshPassProcessorRenderState` + PSO state |
| 排序 | `SortingSettings/SortingCriteria` | `FMeshDrawCommandSortKey` + `FCompareFMeshDrawCommands` |

UE5 没有像 Unity 那样用字符串 `LightMode` 在 runtime C# 层匹配 shader pass。UE5 的 pass processor 是编译期/类型化的:BasePass、DepthPass、ShadowDepth、Velocity 等都有对应 processor 和 shader class 组合。material、vertex factory、feature level、lightmap policy 等共同决定最终 shader permutation。

---

## 5. UE5 pass 如何把 FMeshBatch 编译成 FMeshDrawCommand

`FMeshPassProcessor::BuildMeshDrawCommands` 是 UE5 逐物体绑定的核心。

它做的事情可以分成六步:

### 5.1 从 MeshBatch 取几何入口

```text
VertexFactory = MeshBatch.VertexFactory
PrimitiveSceneInfo = PrimitiveSceneProxy->GetPrimitiveSceneInfo()
PrimitiveType = MeshBatch.Type
```

`VertexFactory` 是 UE5 的几何输入抽象。它决定:

- vertex declaration;
- vertex streams;
- 是否支持 position-only stream;
- primitive id stream index;
- shader parameter element binding;
- GPUScene/instance 数据如何被 shader 读取。

这相当于 Unity native backend 根据 Mesh layout/vertex attributes 创建 input layout 和 VB binding。

### 5.2 构建最小图形管线状态

pass processor 准备 `FGraphicsMinimalPipelineStateInitializer`:

```text
PrimitiveType
BoundShaderState(vertex declaration + shaders)
RasterizerState(fill/cull)
BlendState
DepthStencilState
DrawShadingRate / VRS
ImmutableSamplerState
```

Base pass、depth pass、shadow pass 会各自计算自己的 render state。例如 depth pass 可能选择 position-only vertex shader 和 null/简化 pixel shader,base pass 会根据 material、decal、stencil、translucency pass 设置 depth/stencil/blend。

### 5.3 绑定 pass/material shader 参数

`BuildMeshDrawCommands` 先对共享 command 初始化 shader bindings:

```text
SharedMeshDrawCommand.InitializeShaderBindings(MeshProcessorShaders)
PassShaders.VertexShader->GetShaderBindings(...)
PassShaders.PixelShader->GetShaderBindings(...)
```

`FMeshMaterialShader::GetShaderBindings` 会调用 material shader 的 binding 逻辑,并额外绑定 fade/dither uniform buffer。材质参数、纹理、sampler 等由 `FMaterialRenderProxy/FMaterial` 和 uniform expression 系统提供。

### 5.4 绑定 element/VF/primitive 数据

对 `MeshBatch.Elements[]` 中每个有效 element:

```text
FMeshMaterialShader::GetElementShaderBindings(...)
  -> VertexFactoryType->GetShaderParameterElementShaderBindings(...)
  -> 绑定 VF element 参数
  -> 绑定 PrimitiveUniformShaderParameters 或使用 GPUScene primitive id
```

如果平台和 vertex factory 使用 GPUScene,shader 通常通过 `PrimitiveId` 到 scene buffer 取 primitive data。否则绑定 `FPrimitiveUniformShaderParameters` uniform buffer。

这对应 Unity 的 `UnityPerDraw`,但 UE5 更显式地区分两种路径:

```text
legacy / non-GPUScene:
  draw command 绑定 Primitive uniform buffer

GPUScene:
  draw command 绑定/传递 PrimitiveId
  shader 用 PrimitiveId 读取 PrimitiveSceneData SRV
```

### 5.5 写入 draw 参数

`FMeshDrawCommand::SetDrawParametersAndFinalize` 从 `FMeshBatchElement` 写入:

```text
IndexBuffer
FirstIndex
NumPrimitives
NumInstances
BaseVertexIndex
NumVertices
IndirectArgsBuffer / IndirectArgsOffset
```

这一步之后,`FMeshDrawCommand` 已经具备发出 RHI draw 的主要参数。

### 5.6 生成可见 command payload

`DrawListContext->FinalizeCommand(...)` 会生成或记录:

```text
FVisibleMeshDrawCommand
  MeshDrawCommand*
  PrimitiveIdInfo
  StateBucketId
  MeshFillMode
  MeshCullMode
  SortKey
  CullingPayload
  Flags
```

`FVisibleMeshDrawCommand` 是 view/pass 维度的轻量 payload。真正 draw 所需的重数据放在 `FMeshDrawCommand` 里。

---

## 6. UE5 最终如何提交 draw

`FParallelMeshDrawCommandPass` 负责一个 mesh pass 的 setup/draw。

setup 阶段:

```text
DispatchPassSetup
  -> GenerateDynamicMeshDrawCommands
  -> ApplyViewOverridesToMeshDrawCommands
  -> Update sort keys
  -> Sort(FCompareFMeshDrawCommands)
  -> InstanceCullingContext.SetupDrawCommands
```

draw 阶段:

```text
DispatchDraw / Draw
  -> InstanceCullingContext.SubmitDrawCommands
  -> SubmitMeshDrawCommandsRange
  -> FMeshDrawCommand::SubmitDraw
```

`FMeshDrawCommand::SubmitDrawBegin` 做:

```text
SetGraphicsPipelineState
SetStencilRef
SetStreamSource(vertex streams)
Set primitive id stream / dynamic primitive id offset
ShaderBindings.SetOnCommandList
```

`FMeshDrawCommand::SubmitDrawEnd` 做:

```text
SetUniformBufferDynamicOffset(如果使用 batched primitive slot)
SetShaderRootConstants(如果有)
DrawIndexedPrimitive / DrawIndexedPrimitiveIndirect
DrawPrimitive / DrawPrimitiveIndirect
```

这就是 UE5 与 Unity 最大的可见差异:Unity C# 只看到 `DrawRendererList`,UE5 Renderer C++ 能看到从 `FMeshBatch` 到 `RHICommandList.DrawIndexedPrimitive` 的完整链路。

---

## 7. 逐物体数据:UnityPerDraw vs UE5 PrimitiveSceneData

### 7.1 Unity

Unity 的 per-object 数据由 `DrawingSettings.perObjectData` 决定。URP 常见配置包含:

```text
Lightmaps
LightProbe
OcclusionProbe
ShadowMask
ReflectionProbes
LightData
LightIndices
MotionVectors
```

shader 侧固定 ABI 是 `UnityPerDraw`,包含:

```text
unity_ObjectToWorld
unity_WorldToObject
unity_LODFade
unity_RenderingLayer
unity_PackedLightIndices
unity_LightData
unity_ProbesOcclusion
reflection probe data
lightmap ST
SH
renderer bounds
previous matrices
motion vector params
sprite params
```

pass 只声明需要哪些 `PerObjectData`,native backend 决定填哪些字段。

### 7.2 UE5

UE5 的 primitive 数据来自:

```text
FPrimitiveSceneProxy
  -> BuildUniformShaderParameters
  -> FPrimitiveUniformShaderParameters
  -> FPrimitiveSceneShaderData / GPUScene upload
```

`FPrimitiveUniformShaderParameters` / GPUScene primitive data 包含:

```text
LocalToRelativeWorld
RelativeWorldToLocal
PreviousLocalToRelativeWorld
PreviousRelativeWorldToLocal
ObjectWorldPosition
ActorWorldPosition
ObjectBounds / LocalBounds / PreSkinnedLocalBounds
LightmapUVIndex
LightmapDataIndex
InstanceSceneDataOffset
NumInstanceSceneDataEntries
LightingChannelMask / Flags
CustomPrimitiveData
Nanite resource ids
custom depth stencil
visibility flags
```

shader 侧通过 `GetPrimitiveData(PrimitiveId)` 或 primitive uniform buffer 访问这些数据。UE5 的 material shader 模板大量依赖 `MaterialParameters.PrimitiveId` 和 `GetPrimitiveData(...)`。

### 7.3 对比

| 数据类别 | Unity | UE5 |
|----------|-------|-----|
| transform | `unity_ObjectToWorld` / `unity_WorldToObject` | `LocalToRelativeWorld` / `RelativeWorldToLocal` |
| previous transform | `unity_MatrixPreviousM` / `unity_MatrixPreviousMI` | `PreviousLocalToRelativeWorld` / `PreviousRelativeWorldToLocal` |
| lightmap | `unity_LightmapST` | `LightmapUVIndex` / `LightmapDataIndex` + lightmap policy |
| probe/SH | `unity_SH*` / probe fields | precomputed lighting buffers, ILC/VLM/lightmap policy |
| per-object light index | `unity_PackedLightIndices` / `unity_LightData` | deferred 下通常不是逐物体 direct light list; forward/cluster/shadow pass 有各自路径 |
| custom data | MaterialPropertyBlock / instancing / renderer data | `CustomPrimitiveData` |
| 绑定方式 | native 按 `PerObjectData` 填 `UnityPerDraw` | GPUScene SRV by `PrimitiveId`,或 primitive uniform buffer |

---

## 8. 材质数据:UnityPerMaterial vs UE5 FMaterialRenderProxy

Unity SRP Batcher 要求 material cbuffer layout 稳定。URP Lit shader 中 `UnityPerMaterial` 包含 `_BaseMap_ST`、`_BaseColor`、`_Cutoff`、`_Smoothness`、`_Metallic` 等。pass 不关心这些字段,只通过 shader tag 选择 material 的某个 pass。

UE5 里材质绑定由 `FMaterialRenderProxy` / `FMaterial` / `FMaterialShader` 负责:

```text
FMeshPassProcessor::AddMeshBatch
  -> MeshBatch.MaterialRenderProxy
  -> MaterialRenderProxy->GetMaterialNoFallback(...)
  -> 选择 shader permutation
  -> FMaterialShader::GetShaderBindings(...)
  -> 写入 FMeshDrawShaderBindings
```

UE5 的 material shader 编译组合更类型化:

```text
Material
VertexFactoryType
FeatureLevel / ShaderPlatform
MeshPass
LightMapPolicy
Permutation flags
```

这与 Unity 的 `ShaderTagId + keyword variant + material cbuffer` 方向相似,但 UE5 的 pass processor 在 C++ 层显式参与 shader 选择和 binding 生成。

---

## 9. 光源数据在逐物体绘制中的关系

### 9.1 Unity

URP forward object pass 会通过 `DrawingSettings` 写入:

```text
mainLightIndex = lightData.mainLightIndex
perObjectData 包含 LightData / LightIndices
```

当不是 Forward+ 且存在 additional lights 时,URP 会请求 `PerObjectData.LightIndices`,shader 通过 `unity_PackedLightIndices` / `unity_LightData` 获取逐物体 additional light 索引/数量。也就是说 Unity forward 路径可以把“这个 object 受哪些 additional light 影响”作为 per-object 数据交给 draw。

Forward+ / clustered 路径则更偏向屏幕/cluster light list,不再依赖传统 per-object light indices。

Deferred lighting pass 通常不是逐物体 mesh draw,而是从 GBuffer 和 light list/volume/fullscreen pass 做 lighting。

### 9.2 UE5

UE5 deferred base pass 不会为每个物体绑定“影响它的动态光源列表”。base pass 主要输出 GBuffer,材质和 primitive 数据中会包含 lighting channel、static lighting/lightmap/volumetric lightmap 相关信息。动态 direct lighting 多在后续 light pass、tiled/clustered/volume/shadow 等阶段处理。

UE5 forward shading 或特定 pass 会有自己的 light uniform/cluster 数据路径,但它不是通过一个统一的 `PerObjectData.LightIndices` flag 暴露给 pass 的。

Shadow pass 则是另一类关系:pass 不是给物体绑定光源列表,而是由某个 light/shadow view 决定 shadow caster set,再用 shadow depth mesh processor 把 caster 的 mesh batch 编译成 shadow depth draw command。

### 9.3 结论

Unity URP forward 的逐物体 draw 更明显地暴露“per-object light indices”概念。UE5 deferred 主路径更倾向:

```text
base pass:
  object/material -> GBuffer

lighting pass:
  light data + screen/depth/GBuffer -> lighting

shadow pass:
  light/shadow view -> caster mesh draw commands
```

所以不能简单认为“每个 UE5 object draw 都绑定自己的 light list”。在 deferred 主路径里,动态光照通常不在 base pass 的逐物体 binding 中解决。

---

## 10. Render pass 与最终绑定数据的关系

### 10.1 Unity 的关系

Unity pass 通过描述符间接影响最终 binding:

| Pass 声明 | 对最终 draw 的影响 |
|-----------|-------------------|
| `ShaderTagId` list | 选择 material shader pass |
| `FilteringSettings` | 决定哪些 renderer 进入 list |
| `SortingSettings` | 决定 draw 顺序 |
| `RenderStateBlock` | 覆盖 depth/stencil/blend/raster state |
| `perObjectData` | 决定 `UnityPerDraw` 中哪些数据需要填 |
| `mainLightIndex` | forward shader 主光索引 |
| `enableInstancing` | native backend 是否走 instancing/batching |
| `overrideMaterial` / `overrideShader` | 替换 material 或 shader |

pass 不直接持有 VB/IB/material,而是影响 native backend 如何从 renderer/material/mesh 中取数据。

### 10.2 UE5 的关系

UE5 pass processor 直接参与最终 binding:

| Pass processor 决策 | 对最终 draw 的影响 |
|---------------------|-------------------|
| `ShouldDraw` / `AddMeshBatch` 条件 | 决定 mesh batch 是否进入该 pass |
| shader class/permutation | 选择 VS/PS/GS 等 |
| `FMeshPassProcessorRenderState` | 决定 blend/depth/stencil/stencil ref |
| mesh fill/cull mode | 决定 rasterizer state |
| `EMeshPassFeatures` | 决定 vertex input stream,如 position-only |
| lightmap policy | 决定 base pass shader permutation 和 shader element data |
| `ShaderElementData` | 传递 fade/dither/lightmap/self-shadow 等 element 数据 |
| `BuildMeshDrawCommands` | 写入 shader bindings、vertex streams、primitive id、draw args |

UE5 pass processor 虽然也不需要知道 `UStaticMesh` 的 UObject 细节,但它已经知道 `FMeshBatch` 和 `FMeshBatchElement` 的渲染细节,并负责把它们固化成 RHI draw command。

---

## 11. 一张总对比表

| 维度 | Unity SRP/URP | UE5 |
|------|---------------|-----|
| pass 对象 | `ScriptableRenderPass` | mesh pass + `FMeshPassProcessor` |
| draw list 描述 | `RendererListParams` | `FViewCommands` / `FParallelMeshDrawCommandPass` |
| 候选集 | `CullingResults` | visibility map + primitive relevance |
| primitive render mirror | engine native `Renderer` | `FPrimitiveSceneProxy` + `FPrimitiveSceneInfo` |
| mesh 中间描述 | native 内部 Renderer/Mesh/Submesh | `FMeshBatch` / `FMeshBatchElement` |
| pass 筛选 | `FilteringSettings` + shader tag | relevance + mesh flags + pass processor |
| shader pass 选择 | `ShaderTagId("UniversalForward")` 等 | `EMeshPass` + C++ shader class/permutation |
| material 数据 | `UnityPerMaterial` | `FMaterialRenderProxy` / uniform expressions / shader bindings |
| object 数据 | `UnityPerDraw` | `FPrimitiveUniformShaderParameters` / GPUScene `PrimitiveSceneData` |
| VB/IB 绑定 | native backend 黑盒 | `FMeshDrawCommand.VertexStreams` / `IndexBuffer` 显式可见 |
| draw command | native internal | `FMeshDrawCommand` |
| RHI draw | native internal | `DrawIndexedPrimitive` / `DrawPrimitive` 可见 |
| 静态 draw 缓存 | SRP Batcher / native batching, C# 不直接管 | cached mesh draw commands / state buckets |
| 动态 instancing | native/SRP Batcher/instancing | dynamic instancing at `FMeshDrawCommand` level + GPUScene instance culling |

---

## 12. 对自研 RenderPipeline 的参考结论

如果目标是做一个可控但不过度复杂的 renderer,可以把两套设计拆成三层理解:

```text
RenderPass 层:
  只表达“我要画什么”和“pass global/render target/render state 是什么”

DrawList/RendererList 层:
  根据 culling result + filtering + shader tag/pass type 生成 draw item 列表

DrawCommand 层:
  把 draw item 编译为绑定好的 GPU draw command
  包含 pipeline state、shader bindings、vertex/index buffer、draw args、object id
```

Unity SRP 的优点是 pass 接口干净,业务侧不接触底层 draw details。UE5 的优点是 draw command 构建完全可控,便于缓存、排序、动态 instancing、GPUScene、PSO precache 和调试。

一个折中方向是:

```text
RenderPass 不直接知道 VB/IB
Renderer/PrimitiveProxy 负责产出 MeshBatch-like 描述
MeshPassProcessor/DrawCommandBuilder 负责把 MeshBatch 编译成 DrawCommand
DrawCommand 显式保存底层绑定和 draw args
```

这样可以保留 Unity 式 pass 简洁性,同时拥有 UE5 式可见、可缓存、可优化的 draw command 层。

---

## 13. 源码锚点

Unity Graphics:

| 文件 | 内容 |
|------|------|
| `Packages/com.unity.render-pipelines.universal/Runtime/Passes/DrawObjectsPass.cs` | `ShaderTagId` 列表、`FilteringSettings`、`InitRendererLists`、`cmd.DrawRendererList` |
| `Packages/com.unity.render-pipelines.universal/Runtime/RenderingUtils.cs` | `CreateDrawingSettings`、`CreateRendererListWithRenderStateBlock` |
| `Packages/com.unity.render-pipelines.universal/ShaderLibrary/UnityInput.hlsl` | `UnityPerDraw` |
| `Packages/com.unity.render-pipelines.universal/Shaders/LitInput.hlsl` | `UnityPerMaterial` |
| `Packages/com.unity.render-pipelines.universal/Runtime/UniversalRenderPipeline.cs` | `GetPerObjectLightFlags`、`CreateLightData` |

UE5:

| 文件 | 内容 |
|------|------|
| `Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h` | `DrawStaticElements`、`GetDynamicMeshElements` |
| `Engine/Source/Runtime/Engine/Public/MeshBatch.h` | `FMeshBatchElement`、`FMeshBatch` |
| `Engine/Source/Runtime/Renderer/Public/PrimitiveSceneInfo.h` | `FPrimitiveSceneInfo`、static mesh arrays、cached command infos |
| `Engine/Source/Runtime/Renderer/Private/PrimitiveSceneInfo.cpp` | `AddStaticMeshes`、`DrawStaticElements` 收集 static mesh |
| `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h` | `FMeshDrawCommand`、`FMeshPassProcessor` |
| `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl` | `BuildMeshDrawCommands` |
| `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp` | `SetDrawParametersAndFinalize`、`SubmitDraw` |
| `Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp` | dynamic command generation、sort、dispatch draw |
| `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` | dynamic mesh relevance、view command assembly |
| `Engine/Source/Runtime/Renderer/Private/SceneRendering.cpp` | `FSceneRenderer::SetupMeshPass` |
| `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp` | `FBasePassMeshProcessor` |
| `Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp` | `FDepthPassMeshProcessor` |
| `Engine/Source/Runtime/Engine/Public/PrimitiveUniformShaderParameters.h` | primitive uniform shader data |
| `Engine/Source/Runtime/Engine/Public/PrimitiveSceneShaderData.h` | GPUScene primitive data upload结构 |
| `Engine/Shaders/Private/MaterialTemplate.ush` | material shader 通过 `PrimitiveId` / `GetPrimitiveData` 访问 primitive data |

