# RadRay 渲染运行时具体设计(对照 Unity SRP/URP 真实源码)

> 本文是 `srp_runtime_architecture.md` 的落地版。所有结构来自实读 Unity SRP 源码
> (`Unity-Technologies/Graphics`,master,`com.unity.render-pipelines.universal/Runtime`),
> 逐一映射到 RadRay 的 `radray::render` 抽象。
> **只谈运行时架构,不谈渲染 feature**;**完全不沿用**当前 `modules/runtime` 的 `Material`/`renderer` 那套。
>
> 源码引用(本文末 §10 给出每个类的源文件与行号锚点):
> - `UniversalRenderPipeline.cs` — 相机循环、cull、Submit
> - `ScriptableRenderer.cs` — pass 队列、SortStable、Execute
> - `Passes/ScriptableRenderPass.cs` — RenderPassEvent、ScriptableRenderPassInput、抽象基类
> - `Passes/DrawObjectsPass.cs` — ShaderTagId 列表、FilteringSettings、RenderStateBlock、RendererList、DrawRendererList
> - `RenderingUtils.cs` — `CreateDrawingSettings`(多 tag 优先级解析、perObjectData 注入)
> - `Passes/DepthOnlyPass.cs` — 最小 pass 范本(单 tag + perObjectData=None + RendererListParams 三元组)

---

## 0. SRP 真实运行时分层(读源码后确认)

Unity 实际跑的链路,自顶向下五层,每层职责互不重叠:

```
RenderPipeline.Render(context, cameras)                       // UniversalRenderPipeline.cs:570
   └─ RenderCameraStack → RenderSingleCamera(context, camData) // :910
         ├─ TryGetCullingParameters / SetupCullingParameters   // :923/:950  ScriptableRenderer 提供 cull 参数
         ├─ cullResults = context.Cull(ref cullingParameters)  // :1012     ★ 唯一一次裁剪,所有 pass 共享
         ├─ CreateLightData / CreateShadowAtlasAndCullShadowCasters // :1026/:1041
         └─ renderer.Execute(context, frameData)               // ScriptableRenderer 走 pass 队列
               ├─ SortStable(m_ActiveRenderPassQueue)          // ScriptableRenderer.cs:953/1254  按 RenderPassEvent 插入排序
               └─ foreach pass in queue:  pass.RecordRenderGraph(...) // :1004
                     └─ DrawObjectsPass:                       // DrawObjectsPass.cs
                           ├─ CreateDrawingSettings(shaderTagIdList, …, sortFlags) // :217  ★ ShaderTag→shader pass 解析
                           ├─ CreateRendererListWithRenderStateBlock(cull, draw, filter, stateBlock) // :237  ★ filter+排序+生成 list
                           └─ cmd.DrawRendererList(rendererList) // :150  ★ 录制(变体/PSO 解析由 RendererList 内部完成)
```

五个事实,直接决定 RadRay 的设计:

1. **裁剪只发生一次**(`context.Cull`,:1012),结果 `cullResults` 被所有 pass 共享。pass 不各自裁剪。
2. **pass 是一个有序队列**,排序键是 `RenderPassEvent`(一个整数枚举,见 §2),`SortStable` 是稳定插入排序(:1254)。
3. **pass 不持有 material 名单**。它持有的是 `List<ShaderTagId>`(`DrawObjectsPass.cs:17`)+ `FilteringSettings`(:15)+ `RenderStateBlock`(:16)。
4. **"画谁/用哪段 shader" 由 `RendererList` 内部解析**:`CreateDrawingSettings` 把 ShaderTag 交给引擎,引擎对每个可见 renderer 用它的 material→shader 找匹配 tag 的 pass,找不到就不进 list(这就是 relevance,URP 还单独收集 `objectsWithError`,:238)。
5. **现代 URP 走 RenderGraph**(`AddRasterRenderPass` + `builder.SetRenderAttachment/UseTexture/UseRendererList`,DrawObjectsPass.cs:256+),资源生命周期与 barrier 由 graph 自动推导。

---

## 1. 对象映射表(SRP → RadRay)

| SRP 类型 | 源码锚点 | RadRay 对应 | RadRay 持有什么 |
|----------|----------|-------------|-----------------|
| `RenderPipeline` | URP.cs:570 | `RenderPipeline` | 相机循环、cull 调度、帧资源、Submit |
| `ScriptableRenderer` | Renderer.cs:482 | `RenderPipelineExecutor` | `vector<RenderPass*>` 队列、SortStable、Execute |
| `ScriptableRenderPass` | SRPass.cs:207 | `RenderPass` | `RenderPassEvent`、`ShaderTagId` 列表、Filtering、RenderState、RT 意图、space0、per-object 布局 |
| `RenderPassEvent` | SRPass.cs:67 | `RenderPassEvent`(enum) | pass 排序键 |
| `ShaderTagId` / `Tags{}` | DrawObjects.cs:17 | `TagSet`(`string` kv 数组) | 标识 shader 里的哪个 pass(`"LightMode"=...` 等) |
| `FilteringSettings` | DrawObjects.cs:15 | `FilteringSettings` | queue range + layerMask(第一层意图) |
| `RenderStateBlock` | DrawObjects.cs:16 | `RenderStateOverride` | stencil/depth 覆盖 |
| `DrawingSettings` | DrawObjects.cs:217 | `DrawingSettings` | tag 列表 + sortFlags + per-view 常量句柄 |
| `CullingResults` | URP.cs:1012 | `CullingResults` | 可见 renderer 子集 + 可见灯光 |
| `RendererList` | DrawObjects.cs:150 | `RendererList` | 解析+排序后的 `MeshDrawCommand` 序列 |
| `Renderer`(被裁剪对象) | — | `Renderer` | 几何、world matrix、bounds、几何语义、material 引用 |
| `Material`(.mat) | — | `Material` | shader 引用 + 属性值(space1)+ keyword + 语义 |
| `Shader`(.shader) | — | `Shader` | 每个 LightMode 一段 pass 源 + space1 布局 + keyword 轴 |
| Shader 变体编译缓存 | (引擎内部) | `ShaderVariantCache` | key=(ShaderId, LightMode, KeywordSet) |
| (PSO 由 RHI 缓存) | — | `RootSignatureCache` / `PipelineStateCache` | 见 §6 |

> 注意第 3 列右侧:**Material 实例从不进任何编译缓存的 key**。这是 SRP Batcher 可批的根,§6 详述。

---

## 2. RenderPassEvent:pass 排序键(照搬 SRP 数值,留插入间隙)

SRP 的枚举是**故意稀疏**的(`SRPass.cs:60` 注释明说 "Spaced built-in events so we can add events in between"),让用户能 `RenderPassEvent + offset` 插队。RadRay 直接照搬:

```cpp
// modules/runtime/include/radray/runtime/render/render_pass_event.h
enum class RenderPassEvent : int32_t {
    BeforeRendering              = 0,
    BeforeRenderingShadows       = 50,
    AfterRenderingShadows        = 100,
    BeforeRenderingPrePasses     = 150,
    AfterRenderingPrePasses      = 200,
    BeforeRenderingGbuffer       = 210,
    AfterRenderingGbuffer        = 220,
    BeforeRenderingDeferredLights= 230,
    AfterRenderingDeferredLights = 240,
    BeforeRenderingOpaques       = 250,
    AfterRenderingOpaques        = 300,
    BeforeRenderingSkybox        = 350,
    AfterRenderingSkybox         = 400,
    BeforeRenderingTransparents  = 450,
    AfterRenderingTransparents   = 500,
    BeforeRenderingPostProcessing= 550,
    AfterRenderingPostProcessing = 600,
    AfterRendering               = 1000,
};
```

排序用稳定插入排序(对应 `ScriptableRenderer.cs:1254 SortStable`),相同 event 保持入队顺序:

```cpp
void SortStable(vector<RenderPass*>& q) {
    for (size_t i = 1; i < q.size(); ++i) {
        RenderPass* cur = q[i];
        size_t j = i;
        while (j > 0 && cur->Event() < q[j-1]->Event()) { q[j] = q[j-1]; --j; }
        q[j] = cur;
    }
}
```

---

## 2b. Tags:最简设计 — 每个 shader pass 带一个 string kv 数组

不引入 `LightModeId` 这种强类型句柄。直接照搬 Unity `Pass { Tags { ... } }` 的原貌:每个 shader pass 携一个小型的 string→string 键值数组。`"LightMode"` 只是其中一个约定 key,旁边还可以有 `"Queue"` / `"RenderType"` 等。

```cpp
// 一个 pass 的标签集:就是一排 string kv
struct TagSet {
    vector<std::pair<string, string>> tags;   // 最简;数量极少(通常 1~3 条),线性查足够
    Nullable<std::string_view> Find(std::string_view key) const {
        for (auto& [k, v] : tags) if (k == key) return v;
        return nullptr;
    }
};

// pass 想要的 LightMode 也就是几个字符串(按优先级排序,对齐 SetShaderPassName 的 index)
using WantedLightModes = vector<string>;   // 如 {"UniversalForward", "SRPDefaultUnlit"}
```

解析就是一个字符串比较:遍历 pass 想要的 LightMode 列表(优先级从高到低),看 shader 哪个 pass 的 `"LightMode"` tag 命中,第一个命中的就是要用的 pass。命中不了 → 该 renderer 不进 list(relevance 失败)。

> 字符串比较只发生在构建 RendererList 时,不在逐 draw 热路径;pass 数、tag 数都极小,无需提前 intern。若后续性能不够,再把 `string` key 换成哈希/intern 句柄也不迟——但默认从最简的 string kv 起步。

---

## 3. RenderPass:对照 DrawObjectsPass 的真实字段

SRP 的 `DrawObjectsPass`(`DrawObjectsPass.cs:13-106`)就持有四样东西:`m_ShaderTagIdList`、`m_FilteringSettings`、`m_RenderStateBlock`、`m_IsOpaque`。RadRay 的 `RenderPass` 抽象基类照此设计,再补上 SRP 里隐含在 `CreateDrawingSettings` / RenderGraph builder 里的 space0 与 RT 职责:

```cpp
// modules/runtime/include/radray/runtime/render/render_pass.h
class RenderPass {
public:
    virtual ~RenderPass() = default;

    // —— 排序与解析(对应 DrawObjectsPass 的成员)——
    virtual RenderPassEvent Event() const = 0;                       // SRPass.cs:212 renderPassEvent
    virtual const WantedLightModes& ShaderTags() const = 0;         // DrawObjects.cs:17 m_ShaderTagIdList(按优先级的 LightMode 字符串列表)
    virtual FilteringSettings Filtering() const = 0;                 // :15 m_FilteringSettings(第一层意图)
    virtual SortingCriteria SortFlags() const = 0;                   // :204 opaque→FrontToBack / transparent→BackToFront
    virtual RenderStateOverride StateOverride() const { return {}; } // :16 m_RenderStateBlock(stencil/depth)
    virtual KeywordSet PassKeywords() const { return {}; }           // multi_compile 轴(管线驱动)

    // —— 输出目标意图(对应 RenderGraph builder.SetRenderAttachment*)——
    virtual RtFormatSet RTFormats() const = 0;

    // —— space0 / per-object(SRP 用 SetGlobal* + DrawObjectPassData,见 :114)——
    virtual render::DescriptorSet* ViewSet(const SceneView&) const = 0;        // per-view,pass 自建自填
    virtual void WritePerObject(std::span<byte> dst, const Renderer&, const SceneView&) const = 0;

    // —— 录制入口(对应 RecordRenderGraph / ExecutePass)——
    virtual void Execute(RenderContext&, const SceneView&, const CullingResults&) = 0;
};
```

`DrawObjectsPass` 在 RadRay 里是一个**通用具体类**(不是基类),game 直接配置它,正如 URP 里 BasePass/Transparent 都是 `new DrawObjectsPass(...)`:

```cpp
// 通用不透明 pass:照搬 DrawObjectsPass.Init 的默认 tag 集(DrawObjects.cs:87)
RenderPass* MakeOpaquePass() {
    return new DrawObjectsPass(DrawObjectsPass::Desc{
        .event      = RenderPassEvent::BeforeRenderingOpaques,
        .shaderTags = { "UniversalForward", "SRPDefaultUnlit" },  // 解析 shader 的哪个 pass
        .filtering  = { .queueRange = RenderQueue::Opaque },      // 第一层:应不应该画
        .sortFlags  = SortingCriteria::CommonOpaque,
        .isOpaque   = true,
    });
}
```

---

## 4. Execute:对照 DrawObjectsPass.ExecutePass + InitRendererLists

SRP 的真实执行分两步(先建 RendererList,再录制),RadRay 照搬:

```cpp
// 对应 DrawObjectsPass.InitRendererLists (:201) + ExecutePass (:108)
void DrawObjectsPass::Execute(RenderContext& ctx, const SceneView& view, const CullingResults& cull) {
    // (1) DrawingSettings:把 tag 列表 + sortFlags 交给引擎(对应 CreateDrawingSettings :217)
    DrawingSettings draw{
        .shaderTags = ShaderTags(),
        .sortFlags  = SortFlags(),
        .viewConstants = ViewSet(view),     // per-view(SRP 里是 SetGlobal* 的那批)
    };
    // (2) RendererList:filter + relevance 解析 + 排序,一步到位(对应 CreateRendererListWithRenderStateBlock :237)
    //     ★ 这一步内部完成:意图过滤 → 每 renderer 取 material→shader→按 tag 找 pass(找不到丢弃)
    //       → 查 ShaderVariantCache → 查 PSOCache → 生成 MeshDrawCommand → 按 sortFlags 排序
    RendererList list = ctx.CreateRendererList(cull, draw, Filtering(), StateOverride());

    // (3) 录制(对应 ExecutePass :150 cmd.DrawRendererList)
    render::GraphicsCommandEncoder& enc = ctx.Encoder();
    enc.BindDescriptorSet(0, draw.viewConstants);     // space0,每 pass 一次
    ctx.DrawRendererList(enc, list, *this);           // 内部 per-command 绑 space1 + 写 per-object
}
```

`CreateRendererList` 内部(引擎机器,game 不可见)就是上一份文档 §5 的 DrawRenderers 循环,这里给出和 SRP 对齐的精确版本:

```cpp
RendererList RenderContext::CreateRendererList(const CullingResults& cull,
                                               const DrawingSettings& draw,
                                               const FilteringSettings& filter,
                                               const RenderStateOverride& stateOv) {
    vector<MeshDrawCommand> cmds;
    for (const Renderer* r : cull.renderers) {
        if (!filter.Test(r)) continue;                       // 第一层:意图(queue/layer)  DrawObjects 等价 FilteringSettings
        Material* mat = r->GetMaterial();
        Shader*   sh  = mat->GetShader();
        // 第二层:relevance — 按 LightMode 字符串找 shader 的对应 pass(找不到丢弃,URP 收进 objectsWithError :238)
        std::string_view tag;
        if (!sh->ResolveTag(draw.shaderTags, &tag)) continue;
        KeywordSet kw = draw.passKeywords | mat->MaterialKeywords();
        render::Shader* var = ShaderVariantCache::Get(sh->Id(), tag, kw);   // key 不含 mat 实例
        if (!var) continue;
        render::RootSignature* rs = RootSignatureCache::Get(var);
        render::GraphicsPipelineState* pso =
            PipelineStateCache::Get(var, r->GetVertexLayout(), ResolveState(stateOv, mat), draw.rtFormats);
        MeshDrawCommand cmd;
        cmd.pso       = pso;
        cmd.geometry  = r->BatchElement();
        cmd.space1Set = mat->GetDescriptorSet(rs);           // per-material,懒建+缓存
        cmd.renderer  = r;                                   // per-object 在录制时由 pass.WritePerObject 写
        cmd.sortKey   = MakeSortKey(draw.sortFlags, pso, mat, r);
        cmds.push_back(cmd);
    }
    std::sort(cmds.begin(), cmds.end(), BySortKey{});        // 批 = 排序副产物(SRP 同)
    return RendererList{std::move(cmds)};
}
```

---

## 5. Renderer / Material / Shader 接口(三层,与 SRP 语义对齐)

```cpp
// —— Renderer:被 cull 的对象,几何 + 语义 + material 引用 ——
class Renderer {
public:
    virtual MeshBatchElement BatchElement() const = 0;            // VBV/IBV/range(仅几何)
    virtual const render::VertexBufferLayout& GetVertexLayout() const = 0;
    virtual const Eigen::Matrix4f& WorldMatrix() const = 0;
    virtual Aabb WorldBounds() const = 0;                         // 给 context.Cull
    virtual uint32_t LayerMask() const = 0;                       // FilteringSettings 第一层用
    virtual RenderQueue Queue() const = 0;                        // 来自 material.blendMode,决定落 opaque/transparent
    virtual Material* GetMaterial() const = 0;                    // 只引用,不拥有
};

// —— Material:纯数据(对应 .mat:shader 引用 + 属性值 + keyword)——
class Material {
public:
    Shader*    GetShader() const;
    KeywordSet MaterialKeywords() const;                          // shader_feature 轴
    BlendMode  GetBlendMode() const;                              // Opaque/Masked/Transparent
    bool       IsTwoSided() const;
    float      AlphaCutoff() const;
    render::DescriptorSet* GetDescriptorSet(render::RootSignature*) const; // space1,值填进 CBUFFER,懒建+缓存
};

// —— Shader:多 pass 代码容器(对应 .shader 的多 Pass{ Tags{ "LightMode"=... } })——
class Shader {
public:
    ShaderId Id() const;
    // 按 pass 想要的 LightMode 优先级列表,找本 shader 命中的那个 pass 的 LightMode。
    // ★ 对齐 SRP:CreateDrawingSettings 用 SetShaderPassName(i, tag) 填 DrawingSettings,
    //   index 即优先级(RenderingUtils.cs:811-813)。wanted[0] 最优先,命中即返回;
    //   全不命中 → 该 renderer 不进 list(relevance 失败)。实现就是 string 比较。
    bool ResolveTag(const WantedLightModes& wanted, std::string_view* out) const;
    const ShaderPassSource& GetPassSource(std::string_view lightMode) const; // VS/PS 入口 + space1 布局 + 拼接点
    const TagSet& GetTags(std::string_view lightMode) const;                  // 该 pass 的完整 string kv
    std::span<const KeywordAxis> KeywordAxes() const;
};
```

---

## 5b. per-object 数据契约:PerObjectData 是 pass 拥有的 flag 集(源码实证)

这是我之前没读码就断言、现在用源码坐实的一点,也是废掉 ObjectConstants/Override 的关键依据。

SRP 里 per-object 数据不是一个跨 pass 的 god-struct,而是每个 pass 在 DrawingSettings.perObjectData 上声明一个 flag 集,告诉引擎本趟需要为每个 draw 准备哪些 per-object 数据(光照探针、lightmap、light index、motion 等)。两处实证对比最清楚:

- DrawObjectsPass(forward)用 renderingData.perObjectData(RenderingUtils.cs:763 把它塞进 DrawingSettings)—— 它要完整的 per-object 光照数据。
- DepthOnlyPass 明确写 drawSettings.perObjectData = PerObjectData.None(DepthOnlyPass.cs:71)—— depth 预通道什么 per-object 数据都不要。

同一个 renderer、同一份几何,在两个 pass 里 per-object 需求完全不同。这正证明:per-object 契约归 pass,不归 material,也不该有跨 pass 的统一结构。

RadRay 对应设计:RenderPass::WritePerObject 自己决定写哪些字节(§3),并在 DrawingSettings 上带一个等价的 flag 声明,引擎据此为每个 draw 准备数据:

```cpp
enum class PerObjectData : uint32_t {     // 对应 UnityEngine.Rendering.PerObjectData
    None            = 0,
    LightProbe      = 1 << 0,
    Lightmaps       = 1 << 1,
    LightData       = 1 << 2,   // per-object light index 列表
    MotionVectors   = 1 << 3,
    ReflectionProbes= 1 << 4,
    // …按需扩展
};
// RenderPass 暴露它(forward 给完整集,depth-only 给 None,与 SRP 一致)
virtual PerObjectData RequiredPerObjectData() const { return PerObjectData::None; }
```

> 结论落到 RadRay:depth/shadow pass 走 PerObjectData::None + 只含裁剪空间位置的 per-object 字节;forward pass 走完整集。ObjectConstants god-struct + ObjectConstantsOverride 在这套 flag 机制下没有存在的位置——需求差异由 pass 各自的 flag + WritePerObject 表达。

---

## 6. 三级缓存:key 设计(SRP Batcher 可批的根)

```
ShaderVariantCache : key = (ShaderId, LightMode, KeywordSet)             → render::Shader*
RootSignatureCache : key = 合并反射的 binding layout                       → render::RootSignature*
PipelineStateCache : key = (variant, VertexLayout, RenderState, RTFormats) → render::GraphicsPipelineState*
```

- `render::Shader` / `render::RootSignature` / `render::GraphicsPipelineState` 全是现成 RHI 对象(`render/common.h:565/567/569`),底层不动。
- **MaterialInstanceId 不进任何 key**。一万个用同一 Shader+同一 KeywordSet 的 Material 共享同一 variant/PSO,只各绑各的 space1 `DescriptorSet`。这正是 SRP Batcher:同 shader variant 的 draw 之间只换 per-material CBUFFER,不重建管线状态。
- 懒编译:`ShaderVariantCache::Get` miss 时才编译该 `(shader, tag, keywords)` 组合。

---

## 7. RenderPipeline:相机循环(对照 RenderSingleCamera)

```cpp
// 对应 UniversalRenderPipeline.RenderSingleCamera (URP.cs:910)
class RenderPipeline {
public:
    void Render(RenderContext& ctx, std::span<Camera* const> cameras) {
        for (Camera* cam : cameras) RenderSingleCamera(ctx, cam);
        ctx.Submit();                                             // 对应 context.Submit() :1053
    }
private:
    void RenderSingleCamera(RenderContext& ctx, Camera* cam) {
        CullingParameters cp;
        if (!cam->TryGetCullingParameters(&cp)) return;           // :923
        executor_->SetupCullingParameters(cp, cam);               // :950  pass 可调 cull 参数(如阴影距离)
        CullingResults cull = ctx.Cull(cp);                       // :1012 ★ 唯一裁剪
        SceneView view = MakeView(cam);
        LightData lights = CreateLightData(cull.visibleLights);   // :1026
        CreateShadowAtlasAndCullShadowCasters(lights, cull, ctx); // :1041 阴影 caster 二次裁剪(归 shadow pass 私有)
        executor_->Execute(ctx, view, cull);                      // 走 pass 队列
    }
    unique_ptr<RenderPipelineExecutor> executor_;                 // 对应 ScriptableRenderer
};

// 对应 ScriptableRenderer.Execute (Renderer.cs:953/1004)
class RenderPipelineExecutor {
public:
    void EnqueuePass(RenderPass* p) { queue_.push_back(p); }      // :1038
    void Execute(RenderContext& ctx, const SceneView& view, const CullingResults& cull) {
        SortStable(queue_);                                       // :953  按 RenderPassEvent
        for (RenderPass* p : queue_) p->Execute(ctx, view, cull); // :1004
        queue_.clear();                                           // :1218
    }
private:
    vector<RenderPass*> queue_;                                   // :482 m_ActiveRenderPassQueue
};
```

---

## 8. 与现有 `modules/runtime` 的关系:整体替换

废弃(本设计完全不沿用):
- `RenderContext` god-struct 里的 `ShadowCascadeData`/`AdditionalShadowData` → 改为 shadow pass 私有的 space0(§3 `ViewSet`)。
- `ObjectConstants` + `ObjectConstantsOverride` → 改为 `RenderPass::WritePerObject` 的 per-pass 字节布局。
- `PixelShaderMode{None,AlphaClipOnly,FullColor}` → 改为 `Shader::ResolveTag`:depth-only 要不要 PS = shader 有没有那个 LightMode 的 pass。
- `SceneLightBuffer` 进 runtime → 改为 shadow/forward pass 各自按需建 space0。
- 旧 `Material` 烤死 VS/PS → 改为 `Shader`(多 LightMode 源)+ `Material`(纯数据)两层。

保留并复用:`radray::render` 全部 RHI 抽象(`Shader`/`RootSignature`/`DescriptorSet`/`GraphicsPipelineState`/`CommandEncoder`),以及 `PrimitiveSceneProxy` 的几何镜像思路(改名 `Renderer`,去掉 material 内容拥有)。

---

## 9. 最小垂直切片(落地顺序)

1. **词汇层**:`RenderPassEvent`、`TagSet`/`WantedLightModes`(string kv)、`KeywordSet`、`ShaderId`、`SortingCriteria`、`FilteringSettings`。
2. **缓存层**:`ShaderVariantCache`(先桩:固定返回手写变体)、复用现有 RS/PSO 缓存或新建。
3. **数据层**:`Renderer`/`Material`/`Shader` 接口 + 一个 `Shader`(含 `UniversalForward` 一个 pass)。
4. **机器层**:`RenderContext::CreateRendererList` + `DrawRendererList`(§4)。
5. **编排层**:`RenderPipelineExecutor`(SortStable+Execute)+ `RenderPipeline`(cull+循环)+ 通用 `DrawObjectsPass`。
6. **验证**:跑通 BasePass(`UniversalForward`,space0=相机 CB,per-object=world matrix);
   再加 ShadowPass(`ShadowCaster`,space0=光矩阵,per-object 不同布局),确认:
   - `ObjectConstants`/`Override`/`RenderContext` 阴影字段**完全删除**;
   - 一个 Material 在两 pass 各编一个 variant,实例数据共享(同 shader 多实例不重复编);
   - material 的 shader 没有 `ShadowCaster` pass 时,`ResolveTag` 失败 → 自动跳过(relevance)。

---

## 10. 源码锚点(本文依据,Unity-Technologies/Graphics @ master)

| 文件 | 关键行 | 内容 |
|------|--------|------|
| `UniversalRenderPipeline.cs` | 570 | `Render(context, cameras)` 入口 |
| | 910 | `RenderSingleCamera` |
| | 923/950 | `TryGetCullingParameters` / `SetupCullingParameters` |
| | 1012 | `context.Cull(ref cullingParameters)` — 唯一裁剪 |
| | 1026/1041 | `CreateLightData` / `CreateShadowAtlasAndCullShadowCasters` |
| | 1053 | `context.Submit()` |
| `ScriptableRenderer.cs` | 482 | `m_ActiveRenderPassQueue` |
| | 953/1004 | Execute 里 `SortStable` + foreach pass |
| | 1038 | `EnqueuePass` |
| | 1254 | `SortStable` 稳定插入排序实现 |
| `Passes/ScriptableRenderPass.cs` | 67 | `RenderPassEvent` 枚举(稀疏间隙) |
| | 207/212 | `ScriptableRenderPass` 抽象基类 + `renderPassEvent` |
| | 311 | `RecordRenderGraph` |
| `Passes/DrawObjectsPass.cs` | 15-19 | `m_FilteringSettings`/`m_RenderStateBlock`/`m_ShaderTagIdList`/`m_IsOpaque` |
| | 87 | 默认 tag 集 `UniversalForward`/`UniversalForwardOnly`/`SRPDefaultUnlit` |
| | 108/150 | `ExecutePass` → `cmd.DrawRendererList` |
| | 201/217 | `InitRendererLists` → `CreateDrawingSettings(tagList, …, sortFlags)` |
| | 237/238 | `CreateRendererListWithRenderStateBlock` + `objectsWithError` |
| | 256+ | RenderGraph: `AddRasterRenderPass` / `SetRenderAttachment` / `UseRendererList` |
| `RenderingUtils.cs` | 756-772 | `CreateDrawingSettings(tag, …)` — 把 `perObjectData`/`mainLightIndex`/`enableInstancing` 塞进 `DrawingSettings` |
| | 811-813 | 多 tag 版:`SetShaderPassName(i, tag)`,**index = 优先级**(tag 解析机制) |
| `Passes/DepthOnlyPass.cs` | 20 | `ShaderTagId("DepthOnly")` |
| | 71 | `drawSettings.perObjectData = PerObjectData.None`(per-object 需求归 pass 的实证) |
| | 73 | `RendererListParams(cullResults, drawSettings, filteringSettings)` — 真实三元组 |
| | 58/86-87 | `CreateRendererList` + `cmd.DrawRendererList` 最小路径 |
