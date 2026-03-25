# RadRay Shader 资源与 Render Graph 边界绑定草案

更新日期：2026-03-24

本文档不重复 [render-graph-report.md](render-graph-report.md) 对 Unity / UE5 / Godot 的总体调研，也不重复 [render-graph-public-api-v1.md](render-graph-public-api-v1.md) 对公开 API 的整体草案。

本文只聚焦一个更窄、但会直接决定 render graph 是否容易落地的问题：

- shader 里声明的资源槽位，应该如何成为 render graph pass 的资源引用基准
- 哪些信息可以从 shader / reflection 自动获得
- 哪些信息必须继续由 pass builder 或 graph API 手工声明
- RadRay 现有 `render` / `runtime` 边界里，应该复用哪些现成结构，而不是再造一套平行系统

本次再次直接通过 `gh api` 检查 UE5 RDG 相关源码，并在每次调用前设置代理：

```powershell
$env:HTTP_PROXY="http://127.0.0.1:10808"
$env:HTTPS_PROXY="http://127.0.0.1:10808"
```

本次重点查看的 UE5 文件包括：

- `Engine/Source/Runtime/RenderCore/Public/RenderGraphBuilder.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderGraphResources.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderGraphDefinitions.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderGraphValidation.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraphValidation.cpp`

---

## 1. 目标与非目标

### 1.1 目标

这版草案的目标不是“让 render graph 完全自动推导一切”，而是建立下面这条更稳的契约：

1. **shader 可见的资源槽位** 以 HLSL 为基准
2. **render graph pass 的 shader 资源引用** 尽量不再手工重复声明读写
3. **图内外边界** 继续显式建模，而不是藏在 shader 反射里
4. **非 shader 语义** 继续由 pass builder 手工补齐

更具体地说，这里的“以 HLSL 为基准”应理解成：

- HLSL 作者写下的资源名字、寄存器位置、资源类别、是否只读，应该成为 shader 资源槽的原始事实来源
- render graph 不应要求用户再手写一遍“这个 slot 是 SRV、那个 slot 是 UAV”
- 但 render graph 也不应把 attachment、copy、present、external access 这种根本不属于 shader 反射的信息，错误地强行塞进 shader 侧

### 1.2 非目标

这版草案明确不追求下面这些事情：

- 通过分析 HLSL 源码或 DXIL 指令，区分某个 UAV 在 shader 内到底是 `write-only` 还是 `read-write`
- 从 HLSL 自动推导 color attachment / depth attachment / resolve / present 语义
- 让 render graph 直接以裸 `render::Texture*` / `render::Buffer*` 为长期边界句柄
- 绕开现有 `BindingLayout` / `BindingParameterLayout`，再发明一套平行的 shader 反射描述层

---

## 2. 从 UE5 得到的关键结论

UE5 RDG 对这个问题给出的答案，不是“直接从 HLSL 反射 pass 依赖”，而是更严格的一套分层：

1. **pass parameter struct** 是 graph 依赖的唯一输入
2. **shader-visible resource** 只是 parameter struct 的一部分
3. **render target binding / depth stencil / resolve** 作为单独语义进入 parameter struct
4. **external / extract / epilogue 访问收口** 作为图内外边界单独建模；执行期 external access mode 留到后续版本

源码里最关键的几条信息是：

- `RenderGraphBuilder.h` 头注释明确写着：`Resource barriers and lifetimes are derived from _RDG_ parameters in the pass parameter struct`
- `ERDGPassFlags` 把 `Raster / Compute / AsyncCompute / Copy / NeverCull / SkipRenderPass / NeverMerge / NeverParallel` 明确放在 pass 本体
- `RenderGraphValidation.cpp` 在 `ValidateAddPass(...)` 里，不仅遍历 `TextureSRV / TextureUAV / BufferSRV / BufferUAV`，还单独遍历 `RenderTargetBindingSlots`
- `RenderGraphResources.h` 中资源对象显式区分 `bExternal / bExtracted / bProduced / bTransient / bForceNonTransient`

这说明 UE5 的真正思路是：

- **shader 资源** 是 graph 输入的一部分
- **pass 语义** 不是 shader 资源的附属品
- **graph 边界** 更不是 shader 反射能覆盖的东西

对 RadRay 的直接启发是：

- HLSL 反射适合做 `shader slot schema`
- render graph 仍然需要一层 `pass binding contract`
- external / attachment / copy / present / epilogue 访问收口 仍然必须留在 graph 层

---

## 3. RadRay 现有基础里最应该复用的东西

RadRay 已经有一套相当关键的基础设施，不应该绕开：

- [modules/render/include/radray/render/common.h](../render/include/radray/render/common.h) 里的 `BindingParameterLayout`、`BindingLayout`、`ResourceBindingAbi`
- [modules/render/include/radray/render/shader/hlsl.h](../render/include/radray/render/shader/hlsl.h) 里的 `HlslInputBindDesc`
- [modules/render/src/d3d12/d3d12_binding_layout.cpp](../render/src/d3d12/d3d12_binding_layout.cpp) 里把 HLSL 资源统一降成 `ResourceBindType / Set / Binding / IsReadOnly / IsBindless`
- [modules/runtime/include/radray/runtime/gpu_system.h](include/radray/runtime/gpu_system.h) 里的 `Gpu*Handle` 体系

这几层组合起来，已经给了我们下面这些能力：

1. HLSL 资源名可以被稳定拿到
2. `space/register` 可以被统一成跨后端 ABI
3. 资源是 `Texture / Buffer / RWTexture / RWBuffer / CBuffer / Sampler / AccelerationStructure` 可以被统一分类
4. `IsReadOnly / IsBindless / Count` 这些 graph 需要的关键元数据已经存在
5. shader 反射里的部分 HLSL 特例，例如 push constant 候选 `cbuffer`，已经在 binding layout 阶段被正规化

因此，RadRay render graph 在“以 HLSL 为基准”这件事上，最合理的实现方式不是直接消费原始 `HlslShaderDesc`，而是：

- **作者语义上以 HLSL 为基准**
- **系统实现上优先消费现有的 `BindingLayout / BindingParameterLayout`**

这样做的好处是：

- 不会在 render graph 里重复实现一次 push constant 识别逻辑
- 不会把 D3D12 / Vulkan ABI 统一逻辑复制一份到 runtime
- shader 资源槽描述可以直接和 descriptor set / root signature 体系对齐

---

## 4. 建议的三层边界模型

建议把这个问题明确拆成三层：

| 层级 | 输入来源 | 负责什么 | 不负责什么 |
| --- | --- | --- | --- |
| Shader Slot Schema | `BindingLayout` / reflection | shader 看得见哪些 slot、slot 是什么种类、默认访问倾向 | attachment、copy、present、external |
| Graph Resource Binding | pass setup 时的 `Bind(...)` | 把 shader slot 映射到 graph 资源句柄 | barrier、layout、最终 access |
| Pass Semantic Boundary | graph / pass builder API | import / extract / attachment / copy / resolve / present / epilogue 访问收口 | shader 资源类别判断 |

这三层必须同时存在，不能试图让其中一层吞并另外两层。

### 4.1 第一层：Shader Slot Schema

这一层的职责是表达：

- 这个 shader 或 pipeline 暴露了哪些资源槽位
- 每个槽位的名字是什么
- 它是 texture 还是 buffer
- 它是 SRV / UAV / CBuffer / Sampler / AccelerationStructure 中的哪一种
- 它是否只读
- 它是否数组 / bindless
- 它对应哪个 set/binding

这一层建议直接围绕现有 `BindingParameterLayout` 做一次轻量包装，而不是另起炉灶。

例如：

```cpp
enum class RgShaderSlotClass : uint8_t {
    CBuffer,
    BufferSrv,
    TextureSrv,
    BufferUav,
    TextureUav,
    AccelerationStructure,
    Sampler,
    PushConstant,
};

struct RgShaderSlotDesc {
    string Name;
    DescriptorSetIndex Set{0};
    uint32_t Binding{0};
    RgShaderSlotClass Class{};
    render::ShaderStages Stages{};
    uint32_t Count{1};
    bool IsBindless{false};
    bool IsReadOnly{true};
};
```

但真正的实现不必在 runtime 长期持有一份完全独立的数据副本。更合理的做法是：

- `RgShaderSlotDesc` 只作为 runtime / render graph 层的阅读模型
- 底层仍以 `BindingParameterLayout` 为主

### 4.2 第二层：Graph Resource Binding

这一层的职责是回答：

- 某个 shader slot，这次 pass 实际绑定的是哪一个 graph 资源
- 如果是数组 / bindless，本次 pass 到底引用了哪些 graph 资源
- 如果需要自定义 view，本次绑定的 view 限定是什么

这一层不应该再要求用户写：

- `ReadTexture`
- `WriteBuffer`
- `ReadWriteTexture`

因为这些信息应该已经能从 slot schema 推出来。

用户应该做的是：

```cpp
builder.Bind("gInput", SceneColor);
builder.Bind("gHistory", HistoryColor);
builder.Bind("gOut", BlurOutput);
builder.Bind("gParams", BlurParams);
```

然后由系统根据 slot schema 自动把它们降成 graph 访问记录。

### 4.3 第三层：Pass Semantic Boundary

这一层的职责是回答 shader 反射根本无法回答的问题：

- 这个资源是不是 external / imported
- pass 的 color attachment / depth attachment 是什么
- 是否有 resolve target
- 最终 access 要落到什么状态
- 资源是否需要在图结束后导出到图外继续使用
- 是否是 copy / present / indirect / vertex / index 这类非 shader 使用

这些信息不能混进第一层，也不能指望第二层自动推出。

---

## 5. shader 基准到底应该落在哪里

### 5.1 结论

对 RadRay 来说，“以 HLSL 为基准”最合理的落点不是：

- 直接从 HLSL 反射自动生成整条 pass 依赖

而是：

- 让 HLSL 通过现有 `BindingLayout` 成为 **shader slot schema 的基准**
- 让 pass setup 的 `Bind(...)` 成为 **graph 资源绑定的基准**

换句话说：

- **slot 是 shader 定义的**
- **slot 绑定的是 graph 资源**
- **graph 边界与 pass 语义由 builder 补齐**

### 5.2 为什么不直接让 HLSL 反射成为 graph 依赖输入

因为 HLSL 反射只知道“shader 可能如何看待这个资源”，不知道：

- 这个资源在本次 pass 到底绑定了哪一个 graph 句柄
- 这个 pass 是否还把另一个纹理当 color attachment
- 这个纹理是否是 imported backbuffer
- 这个 buffer 是否同时被拿来做 indirect args
- 这个 pass 是否还有 resolve / copy / present 等不经过 shader 的路径

如果把 HLSL 反射直接当 render graph 输入，最后一定会重新引入额外的补丁 API，系统反而会变得更混乱。

---

## 6. 建议的 binding 契约

### 6.1 每个 shader pass 都应有一个 schema 来源

当前设计里，shader pass builder 的 schema 来源就是 `render::RootSignature*`，并通过它暴露 `BindingLayout`：

- `RenderGraph::AddRasterShaderPass(..., render::RootSignature*, ...)`
- `RenderGraph::AddComputeShaderPass(..., render::RootSignature*, ...)`

这里不建议直接让用户把 `ShaderReflectionDesc` 塞进 render graph。原因是：

- graphics pass 通常是多 shader 合并后的布局
- `BindingLayout` 已经是跨后端统一后的最终 ABI
- render graph 应依赖“最终实际要绑定的布局”，而不是某个尚未 merge 的单 shader 反射

### 6.2 shader pass builder 应只暴露绑定，不暴露读写

建议新增一层 builder：

```cpp
class RGShaderPassBuilderBase {
public:
    render::RootSignature* GetRootSignature() const noexcept;
    const render::BindingLayout& GetBindingLayout() const noexcept;

    RGTextureHandle CreateTransientTexture(std::string_view name, const RGTextureDesc& desc);
    RGBufferHandle CreateTransientBuffer(std::string_view name, const RGBufferDesc& desc);

    void AllowPassCulling(bool value);

    void Bind(std::string_view slotName, const RGTextureBinding& binding);
    void Bind(std::string_view slotName, const RGBufferBinding& binding);
    void BindArray(std::string_view slotName, std::span<const RGTextureBinding> bindings);
    void BindArray(std::string_view slotName, std::span<const RGBufferBinding> bindings);

    void BindSampler(std::string_view slotName, GpuSamplerHandle handle);
    void SetPushConstants(std::string_view slotName, const void* data, uint32_t size);
};
```

其中：

- `Bind(...)` 是 graph resource binding
- `BindSampler(...)` 和 `SetPushConstants(...)` 是 shader binding，但不进入 graph 资源依赖

### 6.3 底层 lowering 仍然应复用现有 pass access 记录

即便公开 API 变成 `Bind(...)`，编译器内部仍然应继续走现有那种 pass access 记录模型：

- `UseTexture(..., const RGTextureUseDesc&)`
- `UseBuffer(..., const RGBufferUseDesc&)`

只是这些记录不再都由用户直接手写，而是由 shader slot schema 自动 lower 生成。

另外，自动 lowering 的终点也不应是 render 公共 API 的 barrier 描述；更合理的分层是：

1. `Bind(...)` 自动 lower 成 graph 内部 access 记录
2. compile 阶段生成 backend-neutral 资源同步计划
3. execute 阶段按后端直接 lower 到 D3D12 enhanced barriers、Vulkan barrier2 等结构

这样做有两个直接好处：

1. 不需要推翻 [render-graph-public-api-v1.md](render-graph-public-api-v1.md) 里已经想清楚的 compile / validation / lifetime 体系
2. shader 驱动 binding 只是公开层优化，不会把内部编译器和资源状态机推翻重写

---

## 7. shader slot 到 graph access 的默认映射

建议先采用一套偏保守、但稳定的默认映射：

| Shader Slot Class | Graph Access Lowering | 说明 |
| --- | --- | --- |
| `CBuffer` | `UseBuffer(..., { .Access = ConstantRead, ... })` | 只读 |
| `BufferSrv` | `UseBuffer(..., { .Access = ShaderRead, ... })` | 只读 |
| `TextureSrv` | `UseTexture(..., { .Access = SampledRead, ... })` | 只读 |
| `AccelerationStructure` | `UseBuffer(..., { .Access = AccelerationStructureRead, ... })` | 初版可先按只读资源对待 |
| `BufferUav` | `UseBuffer(..., { .Access = ShaderRead | ShaderWrite, ... })` | 保守处理 |
| `TextureUav` | `UseTexture(..., { .Access = StorageRead | StorageWrite, ... })` | 保守处理 |
| `Sampler` | 不进入 graph 资源依赖 | 仅 descriptor 绑定 |
| `PushConstant` | 不进入 graph 资源依赖 | 仅命令编码参数 |

### 7.1 为什么 UAV 默认必须保守

HLSL 反射能告诉我们：

- 它是 UAV

但不能可靠告诉我们：

- shader 实际有没有从这个 UAV 读取
- 它是不是纯写
- 它是不是只在某些分支里读

因此 v1 最稳的做法是：

- **所有 UAV 默认按 `ReadWrite` 建模**

这会让 barrier / culling / dependency 比理论最优更保守，但不会错。

### 7.2 可选的优化提示

如果后面需要在不做 DXIL / SPIR-V 指令分析的前提下减少保守性，可以加一个可选 hint：

```cpp
enum class RgShaderAccessHint : uint8_t {
    Default,
    WriteOnly,
    ReadWrite,
};
```

然后允许：

```cpp
builder.SetShaderAccessHint("gOut", RgShaderAccessHint::WriteOnly);
```

但这应该是：

- **可选优化提示**
- **不是 correctness 所需的强制声明**

否则系统又会退回“资源读写需要用户手工重复描述”的老路。

---

## 8. 哪些信息必须继续手工声明

用户提的目标是“只有 external 资源才手动标注”，这个方向基本正确，但还需要补上一句更完整的话：

- **只有 shader 可见的资源引用，才应该交给 shader 基准自动驱动**
- **图边界和非 shader 语义，仍然必须手工声明**

建议把必须手工声明的信息明确分成下面四类。

### 8.1 图内外边界

包括：

- `ImportTexture / ImportBuffer`
- `ExtractTexture / ExtractBuffer`
- `SetTextureAccessFinal / SetBufferAccessFinal`

这是 render graph 成为 runtime 中枢的根本边界，不能交给 shader。

### 8.2 Raster attachment 语义

包括：

- `SetColorAttachment`
- `SetDepthAttachment`
- `SetResolveTarget`
- `Load / Store / Clear`

这些语义和 shader 是否声明了某个 SRV/UAV 没有直接等价关系。

### 8.3 非 shader pipeline 使用

包括：

- `CopyTexture / CopyBuffer`
- `Resolve`
- `Present`
- `Vertex / Index / Indirect`

例如一个 buffer 可能完全不出现在 shader 里，但仍然是 draw indirect 参数；这种依赖不能靠 shader 反射得到。

### 8.4 动态集合与特殊 view

包括：

- bindless / unbound array 到底本次引用了哪些 graph 资源
- 某个 slot 是否使用了特定 mip / slice / range
- 某个 buffer 是否只绑定了局部 range

这些信息同样不能只靠原始 slot schema 推导。

---

## 9. 特殊情况的建议处理方式

### 9.1 Sampler 不是 graph 资源

HLSL reflection 会暴露 sampler slot，但 sampler：

- 不参与 texture / buffer lifetime
- 不参与 barrier
- 不参与 pass culling 根关系

因此建议在 render graph 里明确规定：

- sampler 是 shader binding 对象
- 不是 graph resource

这点非常重要。否则 render graph 会把 descriptor binding 和资源状态机混成一层。

### 9.2 Push Constant 不是 graph 资源

当前 `render` 层已经把部分 HLSL `cbuffer` 正规化为 push constant。render graph 应该直接接受这个结果，而不是重新以 HLSL 原始视角解释一次。

因此建议：

- `BindingParameterLayout.Kind == PushConstant` 的参数不进入 graph 资源依赖
- 它只进入 pass encode 所需的 shader parameter binding

### 9.3 Bindless / Unbound Array 必须有单独规则

bindless 是这套设计里最需要先说清楚的例外，因为 shader 只能告诉我们：

- 这个 slot 是个 unbound array

但它完全不能告诉 graph：

- 本次 pass 真实会用到哪些 graph 资源

因此 v1 建议明确规定：

1. 如果 bindless slot 指向 graph 资源，则 setup 阶段必须显式枚举本次 pass 可访问的资源集合
2. 如果 bindless slot 指向外部全局数组，则这些资源默认不进入 graph 自动依赖推导

也就是说，bindless 不应该破坏整个“shader 基准自动绑定”的主线，但它必须被当作例外明写出来。

### 9.4 View override 只改变绑定视图，不改变资源边界

建议从一开始就把“资源 identity”和“shader view”分开看：

- graph 追踪的是父资源 `Texture / Buffer`
- slot 绑定时可以附带 view override

例如：

```cpp
builder.BindTextureView("gMip3", SceneColor, {
    .BaseMipLevel = 3,
    .MipLevelCount = 1,
});
```

但在 v1 编译器内部，完全可以先保守地按“整个父资源被引用”来建图，后续再做 subresource 精化。

### 9.5 external backbuffer 仍然是 imported texture，而不是特殊 shader 资源

backbuffer 这类资源不应该被做成 shader binding 系统里的特殊分支。

它在 graph 里的正确位置仍应是：

- imported texture
- 初始 state 由 surface / frame acquire 语义给出
- 最终 access 由 `Present` 收口

shader 只是在某个 pass 中可能把它当作 attachment 或 copy dst 使用。

---

## 10. 建议的验证契约

这层设计只有在 validation 先写清楚时才真正成立。建议至少承诺下面这些规则。

### 10.1 shader slot 与 binding 的一致性

1. `Bind(slotName, handle)` 的 `slotName` 必须在 schema 里存在
2. graph-tracked slot 必须被正确类型的资源绑定
3. `TextureSrv / TextureUav` 不能绑定 buffer 句柄
4. `BufferSrv / BufferUav / CBuffer` 不能绑定 texture 句柄
5. `Sampler` 不能绑定 graph 资源句柄
6. `PushConstant` 不能绑定 graph 资源句柄

### 10.2 自动 lowering 的合法性

1. UAV slot 默认按 `ReadWrite` lowering
2. `WriteOnly` 这类 hint 只能用于 UAV slot
3. 如果 slot schema 是只读资源，则不得施加写 hint

### 10.3 pass 语义一致性

1. `RasterShaderPass` 如果不显式声明 attachment，则必须显式走 `SkipRenderPass` 语义
2. attachment 语义不能试图从 shader SRV/UAV 自动推导
3. `CopyPass` / `Present` / `Resolve` 不得依赖 shader slot schema 自动生成

### 10.4 graph 边界一致性

1. graph-created 资源如果要在图外继续使用，必须通过 `Extract*`
2. external 资源必须在导入时给出初始 state 或明确 `UNKNOWN`
3. extracted 资源必须收口 final state
4. `Set*AccessFinal` 必须提供完整 use desc，不能只给一个 render 层终态枚举
5. 执行期 external access mode 不属于这一版公开 API

### 10.5 execute 阶段访问限制

这点应明确学 UE5：

- pass execute 中，只有声明在该 pass shader binding / pass semantic 里的资源，才允许拿到底层 RHI 对象

否则 debug 下就应直接报错，而不是让错误在 barrier 或 GPU validation 阶段才暴露。

---

## 11. 一版建议 API 形态

下面这版接口不是完整 API，只是为了把边界关系表达清楚。

### 11.1 Compute shader pass

```cpp
graph.AddComputeShaderPass<BlurPassData>(
    "Blur",
    blurPso,
    [&](RgComputeShaderPassBuilder& builder, BlurPassData& pass) {
        builder.Bind("gInput", sceneColor);
        builder.Bind("gHistory", historyColor);
        builder.Bind("gOut", blurOutput);
        builder.Bind("gParams", blurParams);

        builder.SetExecute(
            [](RgComputePassContext& ctx, const BlurPassData& pass) {
                // bind root signature / descriptor set / dispatch
            });
    });
```

这一层中：

- shader slot 名字来自 HLSL / `BindingLayout`
- graph access 从 slot schema 自动 lower
- 用户没有再手工写一遍 `ReadTexture/ReadWriteTexture`

### 11.2 Raster shader pass

```cpp
graph.AddRasterShaderPass<ToneMapPassData>(
    "ToneMap",
    toneMapPso,
    [&](RgRasterShaderPassBuilder& builder, ToneMapPassData& pass) {
        builder.Bind("gHdrColor", hdrColor);
        builder.Bind("gExposure", exposureBuffer);

        builder.SetColorAttachment(0, backBuffer, {
            .Load = RgLoadOp::DontCare,
            .Store = RgStoreOp::Store,
        });

        builder.SetExecute(
            [](RgRasterPassContext& ctx, const ToneMapPassData& pass) {
                // fullscreen draw
            });
    });
```

这里的重点是：

- `gHdrColor` 是 shader-visible 资源，自动 lower
- `backBuffer` 是 attachment，必须手工声明

### 11.3 External boundary

```cpp
RGTextureHandle backBuffer = graph.ImportTexture("BackBuffer", {
    .Texture = backBufferHandle,
    .DefaultView = backBufferViewHandle,
    .NativeDesc = backBufferDesc,
    .InitialState = render::TextureState::Present,
    .InitialView = {},
});

graph.SetTextureAccessFinal(backBuffer, {}, {
    .Access = RGTextureAccess::Present,
    .Stages = RGStage::Present,
});
```

这里仍完全不应该交给 shader 侧去决定。

---

## 12. 与现有 public-api-v1 的关系

这份草案不是要推翻 [render-graph-public-api-v1.md](render-graph-public-api-v1.md)。

相反，它建议在那份 API 草案之上再补一层：

1. `RGPassBuilderBase` 作为内部 access lowering 承载层保留
2. 在其上新增 `RGShaderPassBuilderBase`
3. `RGShaderPassBuilderBase` 负责把 `Bind(...)` 自动降成底层 `UseTexture/UseBuffer + desc`
4. attachment / external / final use desc 语义继续直接落在 graph builder 上

也就是说：

- 现有 public API 草案里的 compile / lifetime / validation / boundary 模型仍然成立
- 这份文档只是把“shader 资源引用如何接进那套模型”补完整

---

## 13. 最终建议

如果把这份文档压缩成一句话，那么结论就是：

- **HLSL 决定 shader slot**
- **pass setup 决定 slot 绑定到哪个 graph 资源**
- **render graph 决定 attachment / external / epilogue 访问收口 / copy / present**

更工程化一点说：

1. **不要** 让用户为 shader-visible resource 再手写一次 `ReadTexture/WriteBuffer`
2. **不要** 让 HLSL 反射去承载 attachment / external / epilogue 访问收口 这类图边界语义
3. **要** 复用现有 `BindingLayout / BindingParameterLayout`
4. **要** 把 sampler / push constant 明确排除出 graph 资源依赖
5. **要** 把 UAV 默认当成保守的 `ReadWrite`
6. **要** 把 bindless / special view / non-shader usage 作为例外显式写进契约

这才是最接近 UE5 思想、同时又和 RadRay 当前代码基础最匹配的一版 shader 资源与 render graph 边界绑定方案。
