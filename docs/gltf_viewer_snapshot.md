# gltf_viewer 当前能力快照(重写前留档)

> 用途:`examples/gltf_viewer` 将在新渲染架构上**从头重写**,旧代码整体删除。
> 本文只**客观记录它现在做到了什么**,作为重写时的功能对照表(不评价合理性;不合理处留待最后讨论)。
> 依据:实读 `examples/gltf_viewer/gltf_viewer.cpp`(1522 行)与 `gltf_viewer.hlsl`(257 行)。

---

## 1. 渲染 Pass(5 图形 + 1 调试 compute)

`InitPipeline()`(cpp:845-853)按序注册:

| Pass | 代码 | RT | depth | render state |
|------|------|----|-------|-------------|
| ShadowCasterPass(方向光 CSM) | cpp:324-399 | 无 color | D32_FLOAT,Clear/Store,每 cascade 一个 array slice | `Shadow(depthBias, slopeBias)`,WriteColor=false,变体 `ShadowCaster` |
| AdditionalShadowCasterPass(点/聚光) | cpp:401-484 | 无 color | D32_FLOAT array,Clear/Store | 同 Shadow 预设,WriteColor=false,`ShadowCaster` |
| PreZPass | cpp:486-541 | 无 color | D32_FLOAT,Clear/Store | `PreZ()`,WriteColor=false |
| BasePass(不透明) | cpp:543-623 | backbuffer BGRA8_UNORM,Clear/Store | D32,DepthLoad=Load(复用 Pre-Z) | `OpaqueBase()`:depth **Equal** + 不写深度 + 只写 RGB |
| TransparentPass | cpp:625-695 | backbuffer,Load/Store | D32,Load,不写深度 | `Transparent()`:depth **LessEqual** + src-alpha over 混合 + 只写 RGB |
| ShadowPreviewAtlas(调试) | compute,cpp:921-962 | RWTexture2D rgba8 | — | 把 shadow array 拼 2x2 预览给 ImGui |

- 不透明/透明拆分:`OpaquePrimitiveFilter`(`!IsTransparent()`,cpp:313-318)/ `TransparentPrimitiveFilter`(cpp:320-322)。
- 无独立 skybox / post pass;tone mapping 内联在 BasePass 的 PS。

## 2. Shader 入口与变体

入口(hlsl):
- `VSMain`(59):唯一 VS,内部 `#ifdef SHADOW_CASTER`(63-71)切换;阴影路径用 `apply_shadow_bias`/`apply_shadow_clamping`,把 `CameraPosition` 复用为光方向、`.w` 为 depthBias、`Debug.y` 为 normalBias。
- `PSMain`(107):主着色 PS,输出 `SV_Target0`。
- `PSDepthOnlyMain`(252-257):depth-only PS,仅 `#ifdef ALPHA_TEST` 时 `clip()`。

变体(宏 define,非字符串 tag):
- `SHADOW_CASTER`:`PassVariant.Add(shader_define::ShadowCaster)`(cpp:372/457),VS 分支。
- `ALPHA_TEST`:alpha 裁剪(hlsl:146-148, 253-256);开启来源推测为 glTF alphaMode=MASK(未在本文件内验证)。
- material 固定 `VsEntry="VSMain"`/`PsEntry="PSMain"`(cpp:826-827);depth-only 入口由 pass `WriteColor=false` 在 `MeshPassProcessor` 内选中(未在本文件内验证)。

PS debug 分支(108-143):`Debug.x` = 1 法线 / 2 UV / 3 白 / 4 常量 / 5 解码法线 / 7 cascade 叠加色;overlay 时 BasePass 置 `Debug[0]=7`(cpp:605-607)。

## 3. Per-view 数据(space0,一个 descriptor set)

`SceneLightBuffer::Update` 一次性填充(cpp:782-793),hlsl:42-50:
- `gDirectionalLights` StructuredBuffer t0 / `gPointLights` t1 / `gSpotLights` t3
- `gLightInfo` cbuffer b1(dir/point/spot 数量)
- `gShadowParam` cbuffer b2(方向光 CSM)/ `gAdditionalShadowParam` cbuffer b3
- `gShadowMap` Texture2DArray<float> t2 / `gAdditionalShadowMap` Texture2DArray<float> t4
- `gShadowSampler` SamplerComparisonState s0

混装:3 cbuffer + 3 StructuredBuffer + 2 texture array + 1 comparison sampler。仅 Base/Transparent 注入(cpp:598-599, 676-677);阴影/Pre-Z 不需要。

## 4. Per-object 数据(push constant)

`SceneConstants gScene`,`VK_PUSH_CONSTANT ConstantBuffer<SceneConstants> : register(b0, space0)`(hlsl:41,21-26):
- `MVP` float4x4 / `Model` float4x4(world)
- `CameraPosition` float4(主 pass 相机位;阴影 pass 复用为光方向+depthBias)
- `Debug` uint4(x debug 模式;阴影 pass y 装 normalBias)

各 pass 用 `ObjectConstantsOverride` 回调写(cpp:373-380/458-465/605-607),参数名固定 `"gScene"`。除 world matrix 外无其他常规 per-object 数据(无 per-object material index、无骨骼调色板)。

## 5. Material 数据(space1)

`MaterialConstants gMaterial`,`register(b0, space1)`(hlsl:29-35),5×float4 Principled 参数:
- BaseColorFactor / EmissiveFactorAlphaCutoff / Principled0(metallic,roughness,specular,specularTint)/ Principled1(anisotropic,sheen,sheenTint,flatness)/ Principled2(clearcoat,clearcoatGloss,specularTransmission,eta)

贴图 5 张 + 1 sampler(hlsl:52-57):gBaseColor / gNormalMap / gMetallicRoughness / gOcclusion / gEmissive,`gMaterialSampler` s0。全在 space1。`MaterialDescriptor`(cpp:821-831)描述 shader 路径/入口/cull;贴图与因子作为材质实例数据。

## 6. 特性支持矩阵

| 特性 | 状态 | 证据 |
|------|------|------|
| 多光源(方向/点/聚光) | ✅ | hlsl:181/203/225,`gLightInfo.Counts` |
| 方向光 CSM 4 级 | ✅ | `MaxShadowCascades=4`(cpp:50/61),`ComputeDirectionalCascades`(1013-1132);主 PS 仅 cascade[0] 采样(hlsl:190-191) |
| 点/聚光 additional shadow atlas | ✅ | cpp:401-484 |
| alpha test / clip | ✅ | `ALPHA_TEST` + `clip()`(hlsl:147,255) |
| tone mapping(Reinhard + linear→srgb) | ✅ 内联 | hlsl:247-248 |
| instancing | ❌ | 逐 primitive 一次 draw,push constant per-object |
| 透明排序 | ❌ | cpp:684 注释 "not depth-sorted" |
| 双面材质 | ❌ | 固定 `CullMode::Back`(cpp:829);PS 有法线翻转(hlsl:124-126)但非双面渲染 |
| 骨骼动画 / 多 UV / 顶点色 | ❌ | 顶点输入无 joints/weights/UV1/COLOR |
| MSAA | ❌ | `SampleCount=1`(cpp:119,143) |
| HDR backbuffer | ❌ | BGRA8_UNORM(cpp:701) |
| IBL / 环境贴图 / ambient | ❌ | 无;无光时仅 emissive + 直接光×occlusion(hlsl:246) |
| skybox | ❌ | 无 |

## 7. 顶点布局

`VertexInput`(hlsl:6-11):POSITION0 float3 / NORMAL0 float3 / TEXCOORD0 float2 / TANGENT0 float4(w=手性,重建副切线 hlsl:89/102)。

## 8. 绘制循环

`OnRenderView`(cpp:742-800):
1. 填相机视图 `FillSceneView`(759)。
2. 剔除 `SceneRenderer::Cull` → `rc.Visible`(763-764)。**一次相机剔除,所有 pass 共用**(阴影 pass 不按光锥重剔)。
3. 算方向光 cascade(768)+ 构建点/聚光附加阴影(773),acquire shadow array SRV。
4. `_lightBuffer.Update` → `rc.ViewDescriptorSet`(783-793)。
5. `_pipeline.Render(rc)`(796):按注册序逐 pass = `BeginRenderPass` → viewport/scissor → 组装 `MeshPassProcessor::Config` → `DrawRenderers(encoder, visible, view, config, filter)` → `EndRenderPass`。
6. shadow preview compute(798)。

`DrawRenderers` 内部(for object → 选 PSO → 写 per-object push constant → bind material set → draw)在 `SceneRenderer`/`MeshPassProcessor`,本文件未含。

## 9. 其他依赖的 runtime 设施(重写时需对照)

- `RenderContext`(god-struct):View / Visible / ViewDescriptorSet / ShadowCascadeData / AdditionalShadowData 等。
- `SceneLightBuffer`:per-view 灯光/阴影 set 的统一写入者。
- `RenderResourcePool`:`Acquire`/`Transition` 瞬态资源(shadow array 等)+ 跨 pass 状态迁移。
- `MeshPassProcessor::Config`:PSO cache、RT 格式、render state、`ObjectConstantsParam`/`ObjectConstantsOverride`、可选 view descriptor set。
- `PixelShaderMode` / `PassVariant` / `ObjectConstantsOverride`:旧变体与 per-object 注入机制(analysis.md 认定为待消除的债)。
- 相机组件 `FillSceneView`、`StaticMeshSceneProxy` 等组件→proxy 镜像。
