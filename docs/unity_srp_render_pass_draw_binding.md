# Unity SRP Render Pass 绘制数据与逐物体绑定

本文只分析 Unity SRP/URP 中 `RenderPass` 如何走到最终绘制,以及 pass 与逐物体绑定数据之间的关系。

关键前提:Unity Graphics 仓库中的 C# SRP 代码只暴露到 `RendererList` / `DrawRendererList` 这一层。真正逐 renderer 取 mesh、submesh、material、property block、transform 并绑定 VB/IB/CBV/texture 的逻辑在 Unity engine native 侧,不在 `Packages/com.unity.render-pipelines.*` 包内。但 C# API 和 HLSL ABI 已经明确了数据契约。

---

## 1. 核心结论

SRP 的 object pass 不是低级 draw-command builder,而是 draw-list descriptor builder。

```text
RenderPass(C#)
  -> 声明目标 RT / 输入资源
  -> 声明 FilteringSettings
  -> 声明 DrawingSettings
  -> 声明 RenderStateBlock
  -> 创建 RendererList
  -> Execute 阶段调用 cmd.DrawRendererList(rendererList)

Unity native renderer backend
  -> 遍历 RendererList 内的 renderer
  -> 解析 material / shader pass
  -> 绑定 mesh VB/IB/submesh
  -> 绑定 per-material 数据
  -> 按 DrawingSettings.perObjectData 填 per-object 数据
  -> 发出真实 draw
```

所以 pass 可以不知道 VB/IB/material 细节,不是因为这些数据不需要,而是因为它们归 `Renderer` / `Material` / engine backend 管理。pass 只把"这批 renderer 应该怎样画"描述清楚。

---

## 2. Record 阶段:Pass 准备什么

以 URP `DrawObjectsPass` 为例,`Render()` 中做三类准备:

```text
1. RenderGraph 资源声明
   - color attachment
   - depth attachment
   - shadow map / SSAO / SSR / irradiance / DBuffer 等输入资源

2. RendererList 描述
   - CullingResults
   - DrawingSettings
   - FilteringSettings
   - RenderStateBlock

3. Execute 阶段数据
   - PassData
   - shader global / keyword 所需参数
   - renderer list handle
```

对应源码路径:

- `DrawObjectsPass.Render`:设置 attachment、输入 texture、renderer list、render func。
- `DrawObjectsPass.InitRendererLists`:创建 `DrawingSettings`、修正 `FilteringSettings`、设置 `RenderStateBlock`、创建 `RendererList`。
- `RenderingUtils.CreateRendererListWithRenderStateBlock`:组装 `RendererListParams(cullResults, drawingSettings, filteringSettings)`。

抽象出来就是:

```text
RendererListParams {
  CullingResults     // 候选 renderer 集合
  DrawingSettings    // shader tag、排序、per-object data、main light、instancing
  FilteringSettings  // queue/layer/rendering layer/batch layer
  RenderStateBlock   // depth/stencil/blend/raster override
}
```

---

## 3. Execute 阶段:Pass 真正调用什么

`DrawObjectsPass.ExecutePass` 并不逐物体绑定数据。它只设置本 pass 的全局状态,然后调用 `DrawRendererList`:

```text
cmd.SetGlobalVector(_DrawObjectPassData)
cmd.SetViewport(...)
cmd.SetKeyword(...)
cmd.SetGlobalTexture(...)
cmd.SetGlobalVector(scaleBias)
cmd.SetGlobalFloat(alphaToMaskAvailable)
cmd.DrawRendererList(rendererList)
```

这里的 `DrawRendererList` 是分界线:

```text
C# pass 负责 draw 前的 pass/global 状态
native backend 负责 renderer list 内每个 draw 的 mesh/material/per-object 绑定
```

其他 pass 类型也类似:

| Pass 类型 | 最终调用 | 是否依赖 RendererList |
|----------|----------|----------------------|
| Opaque/Transparent/Depth/DepthNormal | `cmd.DrawRendererList` | 是 |
| Shadow caster | `cmd.DrawRendererList` via shadow renderer list | 是 |
| Skybox | `cmd.DrawRendererList` via skybox renderer list | 是,但不是普通 filtering |
| Deferred lighting | `cmd.DrawMesh` fullscreen/light volume | 否 |
| Copy/Postprocess | `Blitter.BlitTexture` / fullscreen draw | 否 |

---

## 4. DrawingSettings 是 pass 到逐物体数据的主要桥梁

`DrawingSettings` 不只是 shader tag 和排序。它还告诉 engine,这个 renderer list 的每个 draw 需要哪些 per-object 数据。

URP 创建 `DrawingSettings` 时写入:

```text
shaderTagId / SetShaderPassName(...)
sortingSettings
perObjectData
mainLightIndex
enableInstancing
lodCrossFadeStencilMask
overrideMaterial / overrideShader(可选)
```

其中最重要的是 `perObjectData`。

```text
RenderPass
  -> DrawingSettings.perObjectData
  -> Unity native backend
  -> 填充 UnityPerDraw 中对应字段
```

Forward object pass 通常使用 `renderingData.perObjectData`。URP 根据光照模式生成这组 flag,典型包含:

```text
PerObjectData.Lightmaps
PerObjectData.LightProbe
PerObjectData.OcclusionProbe
PerObjectData.ShadowMask
PerObjectData.ReflectionProbes
PerObjectData.LightData
PerObjectData.LightIndices
```

Depth-only pass 则明确不需要这些:

```text
drawSettings.perObjectData = PerObjectData.None
```

Motion vector pass 会声明:

```text
perObjectData = PerObjectData.MotionVectors
```

这说明 per-object 数据需求不是 renderer 固定拥有的"统一结构",而是 pass 通过 `DrawingSettings` 声明出来的需求集。

---

## 5. UnityPerDraw:engine 每个 draw 填的对象数据

URP shader ABI 中,逐物体数据主要在 `UnityPerDraw`:

```hlsl
CBUFFER_START(UnityPerDraw)
float4x4 unity_ObjectToWorld;
float4x4 unity_WorldToObject;
float4 unity_LODFade;
real4 unity_WorldTransformParams;

float4 unity_RenderingLayer;

float4 unity_PackedLightIndices;
half4 unity_LightData;
float4 unity_ProbesOcclusion;

real4 unity_SpecCube0_HDR;
real4 unity_SpecCube1_HDR;
float4 unity_SpecCube0_BoxMax;
float4 unity_SpecCube0_BoxMin;
float4 unity_SpecCube0_ProbePosition;
float4 unity_SpecCube0_Rotation;
float4 unity_SpecCube1_BoxMax;
float4 unity_SpecCube1_BoxMin;
float4 unity_SpecCube1_ProbePosition;
float4 unity_SpecCube1_Rotation;

float4 unity_LightmapST;
float4 unity_DynamicLightmapST;

real4 unity_SHAr;
real4 unity_SHAg;
real4 unity_SHAb;
real4 unity_SHBr;
real4 unity_SHBg;
real4 unity_SHBb;
real4 unity_SHC;

float4 unity_RendererBounds_Min;
float4 unity_RendererBounds_Max;

float4x4 unity_MatrixPreviousM;
float4x4 unity_MatrixPreviousMI;
float4 unity_MotionVectorsParams;
CBUFFER_END
```

这些字段的来源分别是:

| 数据 | 来源 |
|------|------|
| `unity_ObjectToWorld` / `unity_WorldToObject` | Renderer transform |
| `unity_LightmapST` / dynamic lightmap ST | Renderer lightmap 信息 |
| SH / probe / occlusion | light probe / probe volume / baked lighting 数据 |
| reflection probe fields | renderer 关联的 reflection probe 数据 |
| light indices / light data | culling/light setup 产生的 per-object light index |
| rendering layer | Renderer rendering layer mask |
| previous matrices / motion params | motion vector 系统 |
| bounds | renderer bounds |
| LOD fade | LOD/crossfade 系统 |

pass 不逐字段填这些值。pass 只通过 `perObjectData` 声明需要哪些块;engine backend 根据 renderer 自身数据和全局 lighting/culling 结果填。

---

## 6. UnityPerMaterial:material 自己提供的数据

Material 数据走另一条路径。URP Lit shader 里有 `UnityPerMaterial`:

```hlsl
CBUFFER_START(UnityPerMaterial)
float4 _BaseMap_ST;
float4 _BaseMap_TexelSize;
float4 _DetailAlbedoMap_ST;
half4 _BaseColor;
half4 _SpecColor;
half4 _EmissionColor;
half _Cutoff;
half _Smoothness;
half _Metallic;
half _BumpScale;
half _Parallax;
half _OcclusionStrength;
half _ClearCoatMask;
half _ClearCoatSmoothness;
half _DetailAlbedoMapScale;
half _DetailNormalMapScale;
CBUFFER_END
```

SRP Batcher 要求 material constant buffer 布局稳定,所以 URP shader 注释里强调不要用 `ifdef` 改 `UnityPerMaterial` 布局。

Material 相关绑定包括:

```text
Material shader
material keywords
UnityPerMaterial CBUFFER
textures / samplers
MaterialPropertyBlock override
```

这些也不需要 pass 知道。pass 只指定 shader tag,engine 对 renderer 的 material 找到匹配 shader pass 后,自然知道该 material 的 buffer 和 texture 该怎么绑定。

---

## 7. ShaderTagId 让 pass 不依赖具体 material

pass 不说"我要用某个 material 的某个 shader",而是说"我要找这些 LightMode/tag":

```text
DrawObjectsPass:
  SRPDefaultUnlit
  UniversalForward
  UniversalForwardOnly

DepthOnlyPass:
  DepthOnly

DepthNormalOnlyPass:
  DepthNormals
  DepthNormalsOnly

GBufferPass:
  UniversalGBuffer
```

`DrawingSettings.SetShaderPassName(i, tag)` 将这些 tag 交给 engine。native backend 对每个 renderer:

```text
renderer -> material -> shader
shader.FindPassByLightMode(tag0/tag1/tag2...)
```

找到匹配 pass 才画,找不到就跳过或进入 error renderer list。这个机制把 pass 与具体 material 解耦。

重要结论:render pass 与 shader 不是靠 material 类型强绑定,而是靠一套约定好的 ABI 连接。pass 只选择符合 `LightMode` / tag 的 shader pass,并按该 ABI 准备公共数据;只要 shader pass 声明自己属于这个 pass,并按 ABI 读取数据,它就可以被这个 pass 正确绘制。

这个 ABI 至少包含:

```text
shader pass 选择: LightMode / ShaderTagId
全局数据: 固定名字的 global property / cbuffer / texture / buffer
逐物体数据: DrawingSettings.perObjectData -> UnityPerDraw
材质数据: UnityPerMaterial + material texture/sampler/property block
keyword 约定: pipeline multi_compile + material shader_feature
顶点输入语义: POSITION / NORMAL / TANGENT / TEXCOORD...
输出语义: depth-only、shadow、gbuffer、forward color 等 pass 预期输出
```

因此 pass 可以设置很多数据而不理解具体 material。shader 不使用某个全局变量时,这个设置只是无效但安全;shader 需要某个功能时,必须包含对应 URP shader library、声明对应变量/keyword variant,并遵守输出格式。换句话说,`UniversalForward` 既可以画 Lit,也可以画 Toon 或其他自定义材质模型,前提是这些 shader pass 实现了 URP forward ABI。

不兼容时通常有几种结果:

```text
没有匹配 LightMode        -> 不进入该 renderer list
使用 Built-in pass tag     -> checks 下进入 error renderer list / pink shader
缺少需要的变量或 include   -> shader 编译失败或运行结果错误
UnityPerMaterial 布局不稳  -> SRP Batcher 不兼容或属性偏移风险
缺少 keyword variant       -> pass 设置的 keyword 不会得到正确分支
```

所以真正的边界不是"pass 知道 material 细节",而是"shader pass 对外声明自己实现了某个 pass ABI"。

---

## 8. RenderStateBlock / overrideMaterial 的位置

`RenderStateBlock` 由 pass 提供,用于覆盖 draw 的 depth/stencil/blend/raster 状态:

```text
Depth priming 时:
  depth write off
  compare equal

GBuffer/Deferred:
  stencil 标记材质类型

Debug pass:
  替换 render state
```

它影响最终 PSO / dynamic state,但不要求 pass 知道 mesh 或 material 内部。

`overrideMaterial` / `overrideShader` 是可选例外:

```text
drawingSettings.overrideMaterial = overrideMaterial
drawingSettings.overrideMaterialPassIndex = overrideMaterialPassIndex
```

这时 pass 可以强制 renderer list 内所有对象改用同一个 material/shader 绘制。但即使这样,pass 仍不需要知道每个 renderer 的原始 VB/IB/submesh;geometry 仍由 renderer 提供。

---

## 9. Shadow pass 的逐物体绑定

Shadow pass 走专门的 `ShadowDrawingSettings`,不是普通 `DrawingSettings`。

```text
ShadowDrawingSettings(cullResults, shadowLightIndex)
renderGraph.CreateShadowRendererList(ref settings)
```

执行时每个 cascade 设置:

```text
camera position
shadow bias
shadow caster constant buffer
viewport
view/projection matrix
cmd.DrawRendererList(shadowRendererList)
```

对 shadow caster 来说,pass 仍然不绑定每个 mesh/material。它只指定 shadow light、cascade 矩阵、shadow bias 和 shadow renderer list。engine 后端根据 renderer 的 shadow caster pass、mesh 和 transform 发 draw。

---

## 10. 为什么 pass 可以不知道 VB/IB/material 细节

因为 Unity 把数据所有权拆成了三层:

```text
RenderPass owns:
  pass event
  target attachments
  input resources
  shader tag list
  filtering
  render state override
  per-object data flags
  pass/global constants and keywords

Renderer owns:
  mesh
  submesh
  vertex/index buffer
  vertex layout
  transform
  bounds
  lightmap/probe/rendering layer metadata
  material reference

Material owns:
  shader reference
  material properties
  textures/samplers
  material keywords
```

`RendererListParams` 把这三层接起来:

```text
CullingResults gives renderer handles
FilteringSettings decides which renderer handles are used
DrawingSettings decides which shader pass and which per-object data are needed
RenderStateBlock decides render state override
Renderer/Material provide geometry and material payload
Native backend records actual draw commands
```

最终逐物体绑定可以理解为 native backend 内部做:

```text
for renderer in rendererList:
  material = overrideMaterial ? overrideMaterial : renderer.material
  shaderPass = material.shader.FindPass(drawingSettings.shaderTags)
  pso = GetOrCreatePSO(shaderPass, renderState, vertexLayout, rtFormats)

  bind pso
  bind renderer.mesh.vertexBuffers
  bind renderer.mesh.indexBuffer
  bind material.UnityPerMaterial + textures
  fill/bind UnityPerDraw according to drawingSettings.perObjectData
  draw indexed submesh
```

这段不是 C# pass 的职责。C# pass 只负责把足够的描述信息交给 `RendererList`。

---

## 11. 对源码的定位

关键源码锚点:

| 文件 | 内容 |
|------|------|
| `UniversalRendererRenderGraph.cs` | 内置 pass 编排,先 setup lights,再记录 shadow/opaque/skybox/transparent/deferred/postprocess |
| `Passes/DrawObjectsPass.cs` | object pass 的 PassData、RendererList 创建、ExecutePass 调 DrawRendererList |
| `RenderingUtils.cs` | `CreateDrawingSettings`,写入 `perObjectData`、`mainLightIndex`、`enableInstancing`;创建 `RendererListParams` |
| `UniversalRenderPipeline.cs` | 生成 `renderingData.perObjectData` |
| `ShaderLibrary/UnityInput.hlsl` | `UnityPerDraw` ABI |
| `Shaders/LitInput.hlsl` | `UnityPerMaterial` ABI |
| `Passes/DepthOnlyPass.cs` | `perObjectData = None` 的最小 object pass |
| `Passes/MainLightShadowCasterPass.cs` | `ShadowDrawingSettings` 和 shadow renderer list |
