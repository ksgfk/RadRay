>pass 和 material 各自**拥有并提供** shader 输入的一个**不相交频率分区**(pass 出 space0 + per-object,material 出 space1),布局以 shader 反射为权威,owner 按字节写入而非嵌套一个对齐结构体

先把你给的三条观察确认清楚,因为它们就是整个模型的地基,而且都是对的:

- **shader 与 pass、material 密不可分** → 编译出来的 shader 程序不属于 pass、也不属于 material,它是两者**相交时的产物**。
- **pass 是范围最大的批次** → 遍历是 pass 在外、material 在内。一个 pass 内有很多 material。
- **material 与多个 pass 有交集** → 一个 material 会在 ShadowPass / PreZ / BasePass 各出现一次。

所以 Material × Pass 是**多对多**,真正的 shader program 落在每个交点上。当前 RadRay 的病根就是:没有"交点"这个一等公民,于是 material 自己烤死了 VS/PS,pass 用 god-struct + override 打补丁,shader 契约漏进了 runtime。

## 一、唯一的健壮性公理:绑定频率 = 所有权边界

不要按"数据可不可得"来分职责(我上一轮就错在这),要按**数据多久变一次、谁是它的权威源**来分。这条线物理上就存在(GPU 的 descriptor set 频率),让它同时成为所有权线,系统就不会再有归属争议:

| 频率 | 内容 | 契约归谁 | 填值归谁 |
|------|------|----------|----------|
| per-view / per-pass (space0) | 相机、灯光、阴影图 | **Pass** | **Pass** |
| per-material (space1) | PBR 参数、贴图、blend | **Material** | **Material** |
| per-object (push const) | 变换、per-draw 标志 | **Pass** | **Pass**(经 writer) |
| vertex input | 顶点布局 | **Geometry/Proxy** | Proxy |

注意 per-object 的契约和填值**都归 pass**——这正是你坚持的那点。ShadowPass 每个 draw 要 `(lightDir, bias)`,BasePass 要 `(cameraPos, debug)`,它们是**不同的 per-object payload**,所以 per-object ABI 本就是 per-pass 的,不该有一个跨 pass 的 `ObjectConstants` god-struct。

一旦这张表成立,`ObjectConstantsOverride` 自动消失:没有"框架预填的默认值",就没有需要 override 的东西。`RenderContext` 里的 `ShadowCascadeData`/`AdditionalShadowData` 也自动消失:那是某个 pass 的 set0 内容,归那个 pass 私有,不进通用上下文。

## 二、shader 代码本身怎么拆才"密不可分却不耦死"

"密不可分"不等于"揉成一坨"。物理上它们被编译进同一个程序,但接缝可以是一个**函数调用契约**——这正是 Unity SRP 和 UE5 的做法:

- **Pass 提供"框架"(template)**:`VSMain`/`PSMain` 入口、顶点→像素的数据流、space0 与 per-object 的声明、输出怎么写、pass 级 define。框架里有一句对外的调用:`SurfaceData surf = EvaluateSurface(input);`
- **Material 提供"表面函数"(surface)**:`EvaluateSurface(...)` 的函数体 + space1 的声明。它不关心自己被画进哪个 pass。

```
ShaderProgram(passType, material, variant)
   = compile( PassTemplate.hlsl  +  Material.surface.hlsl ,  passDefines ∪ materialDefines )
```

于是:
- 一个 material 的 surface 函数,被 N 个 pass template 各组合一次 → N 个程序。
- 一个 pass template,被 M 个 material 各组合一次 → M 个程序。
- depth-only pass 的 template 根本不 `#include` 也不调用 surface(除非 material 是 masked,需要 alpha test)——这恰好就是你们现在用 `PixelShaderMode::{None, AlphaClipOnly, FullColor}` 表达的东西。

**关键洞察**:`PixelShaderMode` 这个枚举,就是"pass 的需求漏进 material 的一个粗粒度残影"。它本该是一个一等的 **PassType**。把它扶正,接缝就干净了。

## 三、交点作为一等公民:三级缓存按交点 key

```
ShaderProgramCache :  key = (PassTypeId, MaterialId, VariantKey)   → 编译后的 VS/PS
RSCache            :  key = 合并反射出的 binding layout            （已存在,天然包含 pass 的 set0 + material 的 set1）
PSOCache           :  key = (ShaderProgram, VertexLayout, RenderState, RTFormats)  （已存在）
```

RS 仍按合并程序的反射内容寻址——因为组合后的程序反射里自然就有 set0(来自 pass)+ set1(来自 material)+ push(来自 pass),现有 RSCache/PSOCache 不用改逻辑,只是 key 的上游变了。变体爆炸用乘积但每轴都小(pass 级 × material 级),且懒编译。

## 四、落到 RadRay 的具体形状

三个一等对象 + 一个交点缓存,runtime 只提供机器、不提供内容:

```cpp
// —— Pass 拥有的契约(game 层定义具体的;runtime 只定义接口)——
class RenderPassType {
public:
    virtual std::string_view TemplateSource() const = 0;   // pass 的 .hlsl 框架
    virtual std::string_view VsEntry() const = 0;
    virtual std::optional<std::string_view> PsEntry(bool materialMasked) const = 0; // 取代 PixelShaderMode
    virtual ShaderVariantKey PassDefines() const = 0;       // 取代 PassVariant
    virtual MeshPassRenderState RenderState(const Material&) const = 0;

    // set0:pass 自己建/填(取代 ViewDescriptorSet 硬塞 + SceneLightBuffer 进 runtime)
    virtual render::DescriptorSet* ViewSet(BuildContext&) const = 0;

    // per-object:pass 写自己的字节布局(取代 ObjectConstants + Override)
    virtual void WritePerObject(std::span<byte> dst,
                                const PrimitiveSceneProxy&,
                                const SceneView&) const = 0;
};

// —— Material 收缩成:surface 源 + space1 + 材质级变体轴 ——
//    丢掉写死的 VSMain/PSMain「整体」,改为提供 surface 函数 + set1。
//    保留 blendMode/twoSided/alphaCutoff（这些确实是 material 语义）。

// —— 交点缓存(新的一等公民)——
class ShaderProgramCache {
    render::Shader* GetOrCompile(const RenderPassType&, const Material&, const ShaderVariantKey&);
};
```

`MeshPassProcessor` 退回它**不可剥夺的核心**,完全不认识 MVP / 相机 / 阴影:

```
for proxy in visible(filtered by this pass):
    mat   = proxy.material
    prog  = ShaderProgramCache.Get(passType, mat, passType.PassDefines() ∪ mat.Variants())
    rs    = RSCache.Get(prog)
    pso   = PSOCache.Get(prog, proxy.vertexLayout, passType.RenderState(mat), passType.RTFormats())
    cmd.perObjectBytes = passType.WritePerObject(proxy, view)   // pass 决定布局与内容
    cmd.set1 = mat.DescriptorSet()                              // material 填
    cmd.SortKey = ...
record sorted commands, binding set0(passType.ViewSet) once per pass
```

## 五、这样之后,职责一句话各归各位

- **runtime** = 纯机器:flight 调度、cull+DrawRenderers 骨架、三级缓存(含新的 ShaderProgramCache)、上传、proxy↔component 镜像、瞬态资源池、绑定频率词汇。**不含**任何 god-struct、光照模型、阴影技术、per-object ABI。
- **Pass(game)** = 输出配置 + 渲染状态意图 + space0/per-object 的契约与填值 + 它的 .hlsl 框架。
- **Material(game)** = surface 函数 + space1 参数贴图 + 材质语义(blend/twoSided/cutoff)。
- **ShaderProgram** = 不归任何一方,是 (Pass × Material × Variant) 的缓存产物。

健壮性来自三点:每个绑定频率**恰好一个 owner**(无争议)、shader 用**函数契约**组合(可独立演化)、交点是**显式缓存对象**(可观测、可去重、可懒编译)。这就把现在那四处内容泄漏(ObjectConstants、override、RenderContext 阴影、SceneLightBuffer)从根上消解,而不是再打补丁。

要不要我把这套写成一份设计文档(落到具体的类签名、HLSL 组合约定、迁移步骤),或者先做一个最小垂直切片——定义 `RenderPassType` + `ShaderProgramCache`,把 gltf_viewer 的 BasePass 和 ShadowPass 迁过去验证,证明 `ObjectConstants`/override/RenderContext 阴影字段能被彻底删掉?



那pass是如何发现自己要批量处理哪些material？
Start...
这个问题问到了点子上——它正好暴露了一个常见的错误设计冲动,得先把它挡掉。

## 错误答案:pass 主动去"发现" material

直觉会想:pass 是不是该持有一个"我支持哪些 material"的列表,或者扫一遍场景挑出匹配的 material?**不是。** 一旦 pass 去发现 material,就产生了 pass→material 的反向依赖,Material × Pass 的多对多会退化成需要双向注册表,新增一个 material 要去登记到每个相关 pass,新增一个 pass 要去扫所有 material。这正是要避免的耦合。

## 正确答案:pass 不发现 material,pass 过滤 primitive;material 是被 primitive 带出来的

回到你确认过的三条公理,其中两条在这里起决定作用:

- **pass 是范围最大的批次** → pass 的输入是"本帧可见的 primitive 集合",不是 material 集合。
- **material 与多个 pass 有交集** → 这个"交集"不是 pass 去求的,是**每个 primitive 自己带着 material**走进每个 pass 时自然形成的。

数据流是**单向**的:

```
Scene  ──cull──▶  VisiblePrimitiveList   (一次,所有 pass 共享)
                        │
        每个 pass:     │  filter(proxy) —— pass 用谓词挑自己要画的子集
                        ▼
                  proxy.GetMaterial()     —— material 是从 proxy 上"取"出来的
                        │
                  ShaderProgramCache.Get(thisPassType, material, variant)
```

pass 从来不问"哪些 material 是我的"。它问的是"**哪些 primitive 是我这个 pass 要画的**",答案靠 `PrimitiveFilter` 谓词(你们已有,scene_renderer.h:200)。material 只是被选中的 primitive 顺手带出来的属性。

## 那"批量处理 material" 怎么发生的

它不是 pass 去聚合 material,而是 `MeshPassProcessor` 遍历过滤后的 primitive 时,**自然遇到**一堆 material:

```
for proxy in visible where passType.Filter(proxy):     // pass 只决定这一行
    mat  = proxy.GetMaterial()                          // material 自己冒出来
    prog = ShaderProgramCache.Get(passType, mat, ...)   // 交点在此刻形成
    ...生成 MeshDrawCommand,SortKey 高位放 PSO/material 分组...
sort(commands)    // 排序后,同 material/PSO 的 draw 相邻 —— 这才是"批量"
```

"批量处理同一个 material"是**排序的副产物**,不是 pass 预先收集的结果。SortKey 把同 PSO(同 material→同 program→同 PSO)的命令排到一起,录制时状态切换最少——这就是 batching,和 pass"发现 material"无关。

## 那 pass 怎么决定 filter 谓词?它凭什么知道画哪些 primitive

凭 **material 暴露的语义标志**,不是凭 material 身份。看你们现有的:`OpaquePrimitiveFilter` / `TransparentPrimitiveFilter`(gltf_viewer)靠的是 `Material::IsTransparent()` / `IsMasked()`(material.h:85-86)这种**语义**。

- BasePass 的 filter:`!proxy.material.IsTransparent()`
- TransparentPass 的 filter:`proxy.material.IsTransparent()`
- ShadowPass 的 filter:`proxy.CastsShadow()`(几何/组件语义)

pass 表达的是"我要不透明的 / 我要投影的"这种**意图**,material 暴露"我是不是透明/masked"这种**语义**,两者通过谓词匹配。pass 不需要知道任何具体 material 的存在,material 也不需要登记给任何 pass。新增 material 只要正确报告自己的语义,自动落进正确的 pass;新增 pass 只要写好谓词,自动捞到合格的 primitive。**零双向注册,这就是它健壮的原因。**

## 一句话回答你的问题

> pass **不发现 material**。pass 用谓词从共享可见集里过滤 **primitive**;material 是被选中的 primitive 带出来的属性。pass 与 material 的"交集"在 `MeshPassProcessor` 遍历过滤后 primitive、对每个 `proxy.GetMaterial()` 查 `ShaderProgramCache(passType, material)` 的那一刻**逐个形成**,而"批量"是 SortKey 排序让同 material 的 draw 相邻的副产物。

依赖方向始终是 primitive→material 和 pass→primitive(经谓词),**永远没有 pass→material**。要不要我把这条数据流连同 `PrimitiveFilter` 应该基于哪些 material 语义标志,整理进前面那份设计里?

那pass是凭什么去过滤primitive？
Start...
上一轮我说"pass 凭 material 暴露的语义标志去过滤",这个答案不完整,而且藏着一个还没解决的张力。把它拆透。

## 先说清楚:过滤的输入有三个来源,不止 material

pass 的谓词 `bool filter(const PrimitiveSceneProxy&)` 能看到的,是 proxy 暴露的东西。这些东西按权威源分三类:

1. **几何/实例语义**(归 proxy/component):投不投影(`CastsShadow`)、可见性、layer/mask、是否静态。
2. **material 语义**(归 material,proxy 转发):不透明 / masked / 透明、是否双面。
3. **render relevance**(归交点):这个 material **在这个 pass 上到底有没有一个有效的 shader program**。

前两类你们已经有了。真正没想清楚的是第三类。

## 关键张力:谓词是"应不应该画"还是"能不能画"

"BasePass 画不透明物体"——这是按 material 语义过滤,表达的是 pass 的**意图**。没问题。

但考虑一个反例:一个 material 没有 depth-only 入口(`PsEntry(masked)` 返回 nullopt 且它不是 masked),ShadowPass 该不该画它?用"语义过滤"答不了这个问题——它不透明、会投影,语义上完全合格,可它在 ShadowPass 这个交点上**根本没有可编译的 program**。

这就暴露了:**纯靠 material 语义过滤是不够的,会漏掉"语义合格但交点不存在"的情况。** 这正是前面"交点是一等公民"那条还没还清的债。

## 健壮的答案:过滤分成两层,而且第二层不是"过滤"

把 pass 决定画谁,拆成职责清晰的两层:

**第一层——意图谓词(pass 显式写,凭 material/几何语义):**
```cpp
// pass 表达"我想画哪一类",这是策略,归 pass
passType.Filter = [](const PrimitiveSceneProxy& p) {
    return !p.GetMaterial()->IsTransparent();   // BasePass:不透明
};
```
这层回答"**应不应该画**",凭的是 proxy 暴露的语义标志(material 的 blend mode、几何的 cast-shadow 等)。

**第二层——relevance 解析(MeshPassProcessor 自动做,凭交点是否存在):**
```cpp
for proxy in visible where passType.Filter(proxy):     // 第一层:意图
    mat  = proxy.GetMaterial();
    prog = ShaderProgramCache.Get(passType, mat, variant);
    if (!prog) continue;        // 第二层:交点不存在 → 静默跳过,不是 pass 的策略
    ...
```
这层回答"**能不能画**",凭的是 `(passType × material)` 这个交点能不能产出 program。它**不该写进 pass 的谓词**——因为它不是 pass 的意图,是物理约束。让 MeshPassProcessor 在查交点缓存时自然处理(查不到就跳过),pass 不需要知道。

## 为什么必须这么分

如果把"有没有 program"也塞进 pass 谓词,pass 就又要去了解 material 的 shader 细节——回到 pass→material 耦合的老路。反过来,如果只有意图谓词、不做 relevance 解析,就会拿一个不存在的 program 去建 PSO,崩或报错。

所以两层各凭各的、各归各的:

| 层 | 问题 | 凭什么 | 归谁 | 形式 |
|----|------|--------|------|------|
| 意图谓词 | 应不应该画 | proxy 暴露的 material/几何**语义** | Pass | `PrimitiveFilter` 谓词 |
| relevance | 能不能画 | `(pass×material)` 交点是否存在 | Processor(自动) | 查缓存,空则跳过 |

## 对照 UE5,确认这个拆法是对的

UE5 的 `FMeshPassProcessor::AddMeshBatch` 里这两步是分开的:先按 pass 的 relevance/filter 决定要不要处理这个 batch(意图),再 `TryGetShaders` / `GetEffectiveShaders`——拿不到对应 pass 的 shader 就直接 return,不进 draw command(交点)。RadRay 的 `PixelShaderMode` 其实是把第二层的一个特例(depth-only 要不要 PS)硬塞进了配置,扶正成"查交点缓存,空则跳过"后,这个特殊 case 也一并归位了。

## 一句话回答

> pass 凭 **proxy 暴露的 material/几何语义标志**(不透明、masked、投不投影……)写一个**意图谓词**来过滤 primitive,表达"我应该画哪一类";至于"这个 material 在我这个 pass 上**能不能**画",由 MeshPassProcessor 查 `(passType × material)` 交点缓存自动决定,查不到就跳过——这层不是 pass 的过滤策略,pass 无需知道。

依赖方向依然干净:pass 只认 proxy 暴露的**语义**(不认 material 身份、不认 shader),交点的存在性由缓存兜底。要不要我把"proxy/material 该暴露哪些语义标志"定一个最小集合(blend mode、cast shadow、double sided、可见性),作为这套过滤的契约写进设计?