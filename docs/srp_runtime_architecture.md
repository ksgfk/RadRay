# RadRay 渲染运行时架构(参照 Unity SRP / URP 的最小骨架)

> 本文只谈**运行时架构**:有哪些对象、谁拥有谁、数据如何单向流动、命令如何生成与提交、缓存按什么 key 寻址。
> **不谈渲染 feature**:不规定光照模型、阴影算法、后处理、具体 SurfaceData 字段。那些是内容,挂在这套骨架上即可。
> 本文**完全不沿用**当前 `modules/runtime` 里的 `Material` / `renderer` 那套(`RenderContext` god-struct、`ObjectConstants`、`ObjectConstantsOverride`、`PixelShaderMode`、`SceneLightBuffer` 进 runtime 等)。这是一份重写稿。

---

## 0. 一句话定位

照搬 Unity SRP/URP 的运行时分工:**Pipeline 编排相机与 Pass;Pass 表达"我要画哪一类、用哪个 LightMode";引擎内部的 DrawRenderers 拿可见的 Renderer、按 LightMode 标签从 Material 引用的 Shader 里解析出具体 pass、组合编译/查表得到 PSO、排序后录制**。Material 是纯数据(Shader 引用 + 属性值 + keyword),Shader 是多 LightMode 的代码容器,编译产物落在以 ShaderId 寻址的变体缓存里——**Material 实例不进编译 key**,这样海量实例共享一份变体,才能批。

---

## 1. 对象模型:六个一等对象 + 三级缓存

| 对象 | 对应 Unity | 拥有什么 | 不拥有什么 |
|------|-----------|----------|-----------|
| `RenderPipelineAsset` | RenderPipelineAsset | 全局配置(质量、Pass 列表蓝图) | 任何 GPU 资源、每帧状态 |
| `RenderPipeline` | RenderPipeline (instance) | 相机循环、Pass 调度、跨帧资源 | 场景内容、Shader 代码 |
| `RenderPass` | ScriptableRenderPass | 输出目标意图、渲染状态意图、它认的 `LightMode` 标签、space0(per-view)、per-object 字节布局 | Material 名单、Shader 代码、编译产物 |
| `Renderer` | Renderer / FPrimitiveSceneProxy | 几何(VBV/IBV/draw range)、world matrix、bounds、几何语义标志、对 `Material` 的**引用** | Material 内容、Shader、PSO、per-object ABI |
| `Material` | Material (.mat) | 对 `Shader` 的**引用**、属性值(space1 CBUFFER 数据)、贴图引用、keyword 选择、材质语义(blend/masked/twoSided) | 任何 shader 代码、任何编译产物、任何 pass/view 数据 |
| `Shader` | Shader (.shader) | 每个 `LightMode` 一段 pass 代码模板、space1 布局声明、keyword 轴定义 | 属性的**具体值**、pass 的 view 数据、PSO |

三级缓存(都不拥有"内容",只是按 key 去重的查找表):

```
ShaderVariantCache : key = (ShaderId, LightModeId, KeywordSet)        → render::Shader*   (编译后的 VS/PS)
ShaderBindingLayoutCache : key = 合并反射出的 binding layout                 → render::ShaderBindingLayout*
PipelineStateCache : key = (variant, VertexLayout, RenderState, RTFormats) → render::GraphicsPipelineState*
```

> 关键:`ShaderVariantCache` 的 key 里**没有 MaterialInstanceId**。一万个用同一 `Shader`、同一 `KeywordSet` 的 Material 共享同一个编译变体,只是各绑各的 space1 数据。这是"可批"的根。

---

## 2. 所有权 = 绑定频率(唯一的分工公理)

不按"数据可不可得"分职责,按"多久变一次、谁是权威源"分。这条线物理上就是 GPU 的 descriptor set 频率,让它直接当所有权线:

| 频率 | 内容 | 契约归谁 | 填值归谁 | GPU 落点 |
|------|------|----------|----------|----------|
| per-view / per-pass | 相机、灯光、阴影图句柄 | **RenderPass** | **RenderPass** | space0 |
| per-material | 属性值、贴图、blend | **Shader**(布局)/ **Material**(值) | **Material** | space1 |
| per-object | 变换、per-draw 标志 | **RenderPass** | **RenderPass**(经 writer) | push const / per-draw CB |
| vertex input | 顶点布局 | **Renderer** | **Renderer** | IA |

由此自动消解的旧设计债:
- per-object 契约归 pass → **没有跨 pass 的 `ObjectConstants` god-struct**,也就没有 `ObjectConstantsOverride`。ShadowPass 与 BasePass 的 per-object payload 本就不同。
- per-view 归各 pass 私有 → **`RenderContext` 里的阴影级联等字段不进通用上下文**,谁用谁建。
- 灯光 → 是某 pass 的 space0 内容,**`SceneLightBuffer` 不进 runtime 词汇**,由需要它的 pass 自己建。

---

## 3. 数据流:单向,绝无 pass→material 反向依赖

```
Scene (Renderer 列表 + Light 列表)
   │
   │  Cull(view)                         ← 每相机一次,所有 Pass 共享
   ▼
CullingResults (可见 Renderer 子集 + bounds)
   │
   │  for each RenderPass (按 event 排序):
   │     DrawRenderers(cullResults, DrawingSettings{LightMode, sortFlags}, FilteringSettings{layer,queue,renderType})
   ▼
DrawRenderers(引擎内部,见 §5)
   │  filter(renderer) → 意图谓词              ← 第一层:应不应该画(凭语义)
   │  material = renderer.GetMaterial()         ← material 被 renderer 带出来
   │  variant  = ShaderVariantCache.Get(material.shader, pass.LightMode, keywords)
   │  if (!variant) continue;                   ← 第二层:能不能画(交点存在性)
   │  pso  = PSOCache.Get(variant, renderer.VertexLayout, pass.RenderState, pass.RTFormats)
   │  cmd  = { pso, renderer.geometry, material.space1Set, pass.WritePerObject(...) }
   ▼
sort(commands by SortKey)                       ← 同 PSO/material 相邻 = 批
   │
   ▼
record into render::CommandEncoder,bind pass.ViewSet(space0) once per pass
```

依赖方向永远是:`RenderPass → Renderer`(经谓词)、`Renderer → Material`(引用)、`Material → Shader`(引用)。**没有 `Pass → Material`,没有双向注册表。** 新增 Material 只要正确报告语义就自动落进对的 Pass;新增 Pass 只要写好谓词就自动捞到合格 Renderer。

---

## 4. 两层"画谁"判定(对应 URP/UE5 的 filter + relevance)

URP 的 `FilteringSettings` + `ShaderTagId` 解析,本质是两层:

**第一层——意图谓词(RenderPass 显式写,凭语义):**
```cpp
// "我要画哪一类",策略,归 Pass。凭 Renderer 暴露的语义标志(material blend mode、几何 cast-shadow…)
FilteringSettings opaque{ .renderQueueRange = Opaque, .layerMask = ... };
```
回答"**应不应该画**"。

**第二层——LightMode/变体解析(DrawRenderers 自动做,凭交点是否存在):**
```cpp
variant = ShaderVariantCache.Get(material.shader, pass.LightMode, keywords);
if (!variant) continue;   // material 的 shader 没有这个 LightMode 的 pass → 静默跳过
```
回答"**能不能画**"。**不写进谓词**(否则 pass 又要懂 material 的 shader 细节,退回耦合)。这正是把 `PixelShaderMode{None,AlphaClipOnly,FullColor}` 扶正:depth-only 要不要 PS,不再是 material 上的枚举,而是"shader 有没有 DepthOnly/ShadowCaster 这个 LightMode 的 pass"。

---

## 5. DrawRenderers:运行时的核心机器(引擎拥有,game 不可见其内部)

```cpp
// 引擎内部,等价 Unity ScriptableRenderContext.DrawRenderers
void DrawRenderers(const CullingResults& vis,
                   const DrawingSettings& draw,      // LightMode + 排序标志
                   const FilteringSettings& filter,  // layer / queue / renderType
                   const RenderPass& pass,
                   render::CommandEncoder& enc)
{
    vector<MeshDrawCommand> cmds;
    for (const Renderer* r : vis.renderers) {
        if (!filter.Test(r)) continue;                              // 第一层:意图
        Material* mat = r->GetMaterial();
        Shader*   sh  = mat->GetShader();
        KeywordSet kw = pass.PassKeywords() | mat->MaterialKeywords();
        render::Shader* var = g_shaderVariantCache.Get(sh->Id(), draw.lightMode, kw);
        if (!var) continue;                                         // 第二层:交点不存在
        render::ShaderBindingLayout* rs = g_rsCache.Get(var);
        render::GraphicsPipelineState* pso = g_psoCache.Get(
            var, r->GetVertexLayout(), pass.RenderState(*mat), pass.RTFormats());
        MeshDrawCommand cmd;
        cmd.pso        = pso;
        cmd.geometry   = r->BatchElement();                         // VBV/IBV/range
        cmd.space1Set  = mat->GetDescriptorSet(rs);                 // per-material(懒建+缓存)
        pass.WritePerObject(cmd.perObjectBytes, *r, vis.view);      // per-object,pass 决定布局与内容
        cmd.sortKey    = MakeSortKey(pso, mat, r);
        cmds.push_back(cmd);
    }
    std::sort(cmds.begin(), cmds.end(), BySortKey{});               // 批 = 排序副产物
    enc.BindDescriptorSet(0, pass.ViewSet(vis.view));               // space0,每 pass 一次
    for (auto& c : cmds) RecordDraw(enc, c);
}
```

`RenderPass` 退回它**不可剥夺的核心**:决定 LightMode、filter、render state、RT、space0、per-object 布局。它**完全不认识** MVP/相机/具体 shader,那些要么是 space0 里它自己建的,要么是变体缓存兜的。

> 关键设计结论:pass 与 shader pass 之间必须定义 ABI。只要 shader 声明了 pass 要求的 `LightMode`,并遵守该 `LightMode` 对应的 descriptor/cbuffer/keyword/vertex input/output layout 约定,它就能被这个 pass 画;pass 不需要知道它是 Lit、Toon 还是其他材质模型。反过来,如果 shader 只挂了同名 `LightMode` 但不实现对应 ABI,那就是 shader 自己声明错误,结果应当是编译失败、校验失败或运行期跳过/error material,而不是让 pass 解析 material 内部细节。

---

## 6. 接口草图(runtime 只定义机器与词汇,game 填内容)

```cpp
// ---- Pipeline:相机循环 + Pass 调度(等价 RenderPipeline.Render)----
class RenderPipeline {
public:
    virtual void Render(RenderContext& ctx, std::span<Camera* const> cameras) = 0;
    // 典型实现:for cam: cull → for pass(sorted by event): pass->Execute(ctx, cam, cullResults)
};

// ---- RenderPass:一个 LightMode 的编排单元(等价 ScriptableRenderPass)----
class RenderPass {
public:
    virtual RenderPassEvent Event() const = 0;                       // 排序位置(Shadow<Opaque<Transparent…)
    virtual LightModeId     LightMode() const = 0;                   // 解析 Shader 的哪个 pass
    virtual FilteringSettings Filtering() const = 0;                 // 第一层意图谓词的数据形式
    virtual KeywordSet      PassKeywords() const = 0;                // multi_compile 轴(管线驱动)

    virtual MeshPassRenderState RenderState(const Material&) const = 0;
    virtual RtFormatSet         RTFormats() const = 0;

    virtual render::DescriptorSet* ViewSet(const SceneView&) const = 0;   // space0,pass 自建自填
    virtual void WritePerObject(std::span<byte> dst,
                                const Renderer&, const SceneView&) const = 0; // per-object 字节布局

    virtual void Execute(RenderContext&, const SceneView&, const CullingResults&) = 0;
    // 默认实现 = 调一次 DrawRenderers(cull, {LightMode(), sort}, Filtering(), *this, enc)
};

// ---- Renderer:几何 + 语义 + material 引用(等价 Renderer / 现 PrimitiveSceneProxy)----
class Renderer {
public:
    virtual MeshBatchElement BatchElement() const = 0;               // VBV/IBV/range(仅几何)
    virtual const render::VertexBufferLayout& GetVertexLayout() const = 0;
    virtual const Eigen::Matrix4f& WorldMatrix() const = 0;
    virtual Aabb WorldBounds() const = 0;                            // 给 cull
    // 几何/实例语义(第一层谓词读这些)
    virtual uint32_t LayerMask() const = 0;
    virtual bool CastsShadow() const = 0;
    virtual bool IsVisible() const = 0;
    // material 引用(只引用,不拥有内容)
    virtual Material* GetMaterial() const = 0;
};

// ---- Material:纯数据(Shader 引用 + 值 + keyword + 语义)----
class Material {
public:
    Shader*    GetShader() const;
    KeywordSet MaterialKeywords() const;                             // shader_feature 轴(material 驱动)
    // 材质语义(第一层谓词读这些;不含任何 shader 代码)
    BlendMode  GetBlendMode() const;   // Opaque/Masked/Transparent → 决定落哪个 queue
    bool       IsTwoSided() const;
    float      AlphaCutoff() const;
    // per-material descriptor set:用值填 space1,懒建 + 缓存(布局来自 Shader 反射)
    render::DescriptorSet* GetDescriptorSet(render::ShaderBindingLayout* rs) const;
};

// ---- Shader:多 LightMode 代码容器 + keyword 轴(等价 .shader / ShaderGraph 产物)----
class Shader {
public:
    ShaderId Id() const;
    bool HasPass(LightModeId) const;                                 // 第二层 relevance 的依据之一
    // 取某 LightMode 的源(VS/PS 入口、space1 布局声明、pass 模板拼接点)
    const ShaderPassSource& GetPassSource(LightModeId) const;
    std::span<const KeywordAxis> KeywordAxes() const;
};

// ---- 三级缓存:不拥有内容,只去重 ----
struct ShaderVariantCache { render::Shader* Get(ShaderId, LightModeId, const KeywordSet&); };
struct ShaderBindingLayoutCache { render::ShaderBindingLayout* Get(render::Shader* variant); };
struct PipelineStateCache  { render::GraphicsPipelineState* Get(render::Shader*, const render::VertexBufferLayout&, const MeshPassRenderState&, const RtFormatSet&); };
```

> 这些是 runtime 提供的**接口与机器**。游戏/示例层去实现具体的 `RenderPipeline`(几个 pass、什么顺序)、具体的 `RenderPass`(BasePass/ShadowPass 的 space0 与 per-object)、提供 `Shader` 资产与 `Material` 实例。runtime 不内置任何光照/阴影/后处理 feature。

---

## 7. Shader 的运行时形态:LightMode 标签 + 两类 keyword(对齐 URP)

- **一个 `Shader` 资产 = 多个 pass,每个 pass 一个 `LightMode` 标签**(`UniversalForward` / `ShadowCaster` / `DepthOnly` …)。DrawRenderers 用 `draw.lightMode` 去 `Shader` 里选 pass;选不到就跳过(第二层 relevance)。
- **两类 keyword 轴**:
  - `PassKeywords()`(= URP `multi_compile`,管线驱动):阴影开关、灯光数等,全部变体都可能编。
  - `MaterialKeywords()`(= URP `shader_feature`,material 驱动):`_NORMALMAP`、`_ALPHATEST` 等,只编被某 Material 用到的。
- **变体 key = (ShaderId, LightModeId, PassKeywords ∪ MaterialKeywords)**。懒编译:首次遇到才编,编完进 `ShaderVariantCache`。每轴都小,乘积可控。

> 渲染 feature(具体哪种光照、SurfaceData 有哪些字段、pass 模板内部怎么算)**不在本文范围**。运行时只关心:Shader 能按 LightMode 给出一段可编译源、能报告 keyword 轴、编译产物可缓存可寻址。

---

## 8. 与现有 `radray::render` 底层的衔接(不改底层)

本架构的三级缓存产物全部是现成的 `radray::render` 对象,底层一行不用动:
- `ShaderVariantCache` → `render::Shader*`(已存在 `class Shader`,见 `render/common.h:565`)。
- `ShaderBindingLayoutCache` → `render::ShaderBindingLayout*`(`common.h:567`),key 按合并程序反射,天然含 space0(pass)+ space1(material)+ push(pass)。
- `PipelineStateCache` → `render::GraphicsPipelineState*`(`common.h:569`)。
- 录制走 `render::CommandEncoder / GraphicsCommandEncoder`(`common.h:551-552`),descriptor set 用 `render::DescriptorSet`(`common.h:566`)。

也就是说本文是**纯 runtime 层重写**:废掉旧的 `Material`/`renderer` 编排与 god-struct,换成"Pipeline→Pass→DrawRenderers→三级缓存"的 SRP 骨架,底层 RHI 抽象不动。

---

## 9. 健壮性来自哪里(总结)

1. **每个绑定频率恰好一个 owner**(space0→Pass、space1→Material、per-object→Pass、IA→Renderer),无归属争议。
2. **依赖严格单向**(Pass→Renderer→Material→Shader),无双向注册表,增删 Pass/Material 互不波及。
3. **编译产物是交点缓存的去重结果**,key 用 ShaderId 而非 Material 实例 → 海量实例共享变体,可批。
4. **画谁分两层**:意图谓词(语义,归 Pass)+ relevance(交点存在性,引擎自动兜底),`PixelShaderMode` 这种"pass 需求漏进 material"的残影被消除。
5. **runtime 只提供机器与词汇,feature 全在 game 层**:换光照/阴影/后处理不触碰这套骨架。

---

## 10. 最小垂直切片(验证用,建议落地顺序)

1. 定 `LightModeId` / `KeywordSet` / `ShaderId` 词汇 + `ShaderVariantCache`(先桩实现:固定返回一个手写变体)。
2. 定 `Renderer` / `Material` / `Shader` 接口,做一个三角形 + 一个 `Shader`(含 `UniversalForward` 一个 pass)。
3. 定 `RenderPass` 基类 + `DrawRenderers`,实现一个 BasePass(space0 = 相机 CB,per-object = world matrix)。
4. 跑通后加 ShadowPass(LightMode=`ShadowCaster`,space0 = 光矩阵,per-object 不同布局),验证:
   - `ObjectConstants`/`Override`/`RenderContext` 阴影字段**完全不需要**;
   - 一个 Material 在两个 pass 各编一个变体,实例数据共享;
   - material 若没有 `ShadowCaster` pass,DrawRenderers 自动跳过(第二层 relevance)。
```
