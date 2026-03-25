# RadRay Render Graph 初版公开 API 草案

更新日期：2026-03-25

本文档不是在重复 [render-graph-report.md](render-graph-report.md) 的调研结论，而是在那份报告的优先级排序基础上，结合三套引擎源码细读结果，给出一版适合 RadRay 的公开 API 草案。

针对 “shader 资源槽位如何接进 render graph、哪些边界信息应继续由 graph API 声明” 这个更窄的问题，另见 [render-graph-shader-binding-v1.md](render-graph-shader-binding-v1.md)。

本次再次直接检查的源码重点如下：

- UE5 `RenderGraphBuilder.h` / `RenderGraphDefinitions.h` / `RenderGraphPass.h` / `RenderGraphResources.h` / `RenderGraphValidation.h` / `RenderGraphBuilder.cpp` / `RenderGraphValidation.cpp`
- Unity `RenderGraph.cs` / `RenderGraphBuilders.cs` / `NativePassCompiler.cs`
- Godot `rendering_device_graph.h` / `rendering_device_graph.cpp`

调研方式仍然是 `gh api` 直接读取远程源码，并在调用前设置代理：

```powershell
$env:HTTP_PROXY="http://127.0.0.1:10808"
$env:HTTPS_PROXY="http://127.0.0.1:10808"
```

## 1. 范围

这版 API 不是“把所有 render graph 能力一次塞满”，而是只纳入真正应该先公开的部分。

根据 [render-graph-report.md](render-graph-report.md) 的排序，初版必须覆盖：

| 优先级 | 能力 | 是否纳入 v1 | 原因 |
| --- | --- | --- | --- |
| 最高 | 资源访问声明模型 | 是 | 极高/很高能力都建立在它之上 |
| 最高 | 生命周期、状态机与 barrier 输入 | 是 | 没有这个，API 只是顺序录制壳子 |
| 极高 | 图编译、依赖闭包与 culling | 是 | 决定 API 是否真的能编译成执行计划 |
| 极高 | 外部资源边界管理 | 是 | 决定是否能接入真实 runtime / swapchain / history |
| 很高 | Raster / Attachment / Pass 类型语义 | 是 | 决定主渲染路径能否优雅接入 |
| 很高 | Validation | 是 | 决定 API 是否可长期维护 |

初版明确不公开：

- Async Compute 调度
- 多队列 fork / join
- secondary command buffer 并行录制
- 高级 transient aliasing overlap
- renderer list / global texture 传播
- blackboard / profiling / graph viewer 等工程配套层

注意：虽然用户当前要求是“先纳入极高、很高优先度能力”，但这几个能力的公开 API 无法脱离“最高优先度”的访问声明与资源边界契约单独存在，所以 v1 会把这两层一起定下来。

## 2. 三家源码里最值得抽出来的东西

### 2.1 UE5：真正有价值的不是宏，而是契约

UE5 最值得学的不是把 `RDG` 参数宏原样搬过来，而是下面四条契约：

1. `FRDGBuilder` 明确把 “AddPass 参数结构里的 RDG 资源” 作为 barrier 和 lifetime 的唯一输入。
2. `ERDGPassFlags` 把 `Raster / Compute / AsyncCompute / Copy / NeverCull / NeverMerge / NeverParallel` 这类执行语义放进 pass 本体，而不是做成零散布尔开关。
3. `RegisterExternalTexture/Buffer`、`QueueTextureExtraction/BufferExtraction`、`Set*AccessFinal` 把图内外边界做成了一等能力；`UseExternalAccessMode`、`UseInternalAccessMode` 说明 UE5 还支持更复杂的执行期外借模型。
4. `RenderGraphValidation` 不只是“创建时做 assert”，还覆盖 create、extract、pass add、pass execute begin/end 等完整生命周期；UE5 还额外验证 external access mode 这类进阶能力。

对 RadRay 的启发是：

- 要学 UE5 的“公开契约”
- 不要照搬 UE5 的“参数反射实现手法”

原因很简单：UE5 能靠反射宏把资源访问从参数结构自动收集出来，但 RadRay 现在没有这套基础设施。我们应该复制的是“资源访问声明必须有唯一可信来源”这个思想，而不是强行复制它的元编程外观。

### 2.2 Unity：typed builder 和 attachment 语义最适合做公开面

Unity 最值得学的是它公开给用户的那一层：

1. `AddRasterRenderPass / AddComputePass / AddUnsafePass`
2. `EnableAsyncCompute / AllowPassCulling / AllowGlobalStateModification`
3. `CreateTransientTexture / CreateTransientBuffer`
4. `UseTexture / UseBuffer`
5. `SetRenderAttachment / SetRenderAttachmentDepth / SetInputAttachment / SetRandomAccessAttachment`

这些 API 说明了两件事：

- 公开 API 应该是 typed builder，不应该让用户直接碰 barrier / layout
- raster attachment 必须是一等公民，不能退化成“所有写纹理都一样”

对 RadRay 来说，这一层非常适合作为公开 API 外形。

### 2.3 Godot：低层 hazard graph 应该藏在公开 API 背后

Godot 最值钱的不是它的表面接口，而是它内部的 lowering 方式：

1. `ResourceUsage` 被细分成 `COPY_FROM / COPY_TO / RESOLVE_FROM / RESOLVE_TO / TEXTURE_SAMPLE / STORAGE_IMAGE_READ_WRITE / ATTACHMENT_COLOR_READ_WRITE ...`
2. `_usage_to_image_layout()`、`_usage_to_access_bits()`、`_is_write_usage()` 把高层 usage 降成 barrier 输入
3. `_add_command_to_graph()` 在记录期就增量建立 hazard 依赖
4. `_boost_priority_for_render_commands()`、barrier grouping、`add_synchronization()` 说明执行器和公开 builder 应该解耦

对 RadRay 的启发是：

- 公开 API 讲 “资源访问语义”
- 内部执行器讲 “usage / access / layout / hazard graph”

也就是说，Godot 该学的是内部 lowering 和执行器分层，不是把低层命令图直接公开给上层。

## 3. RadRay v1 的总原则

### 3.1 两层结构

RadRay render graph 最终应明确拆成两层：

1. 公开的 Frame Graph API
2. 内部的 Execution Graph / Hazard Graph

公开 API 负责：

- Pass 类型
- 资源句柄
- 资源读写声明
- attachment 语义
- import / create / transient / extract
- final access / final use desc
- validation 契约

内部执行层负责：

- 依赖闭包
- culling
- resource lifetime range
- barrier compile
- layout / access lowering
- 后续 merge / reorder / queue 扩展

### 3.2 初版只公开“意图”，不公开“状态机细节”

用户应该声明：

- 这个 pass 是 raster / compute / copy / unsafe
- 这个资源被读、写还是读写
- 这个纹理是不是 color/depth attachment
- 这个资源是不是 import / extract / transient
- 这个资源在图外最终要处于什么 access

用户不应该直接声明：

- 具体 barrier
- 具体 image layout
- pipeline stage mask
- split barrier / merged barrier

这部分应该在编译器内部，按 Godot/UE5 的方式求解。

### 3.3 公开 API 必须和现有 runtime 对齐

当前 runtime 已经有 [gpu_system.h](include/radray/runtime/gpu_system.h) 这条稳定 GPU 边界，因此 render graph 的外部资源接入不应该直接暴露裸 `render::Texture*` 作为所有权单位，而应该优先围绕：

- `GpuTextureHandle`
- `GpuTextureViewHandle`
- `GpuBufferHandle`
- `GpuFrameContext`
- `GpuAsyncContext`

来定义图内外边界。

执行 lambda 内部当然仍需要拿到 `render::CommandBuffer*`、`render::Texture*`、`render::Buffer*` 去真正录命令，但资源导入/导出边界应该复用 runtime 已有句柄体系。

## 4. 建议的公开 API 形态

### 4.1 命名和句柄

公开层建议继续使用 `Rg*` 前缀，避免和底层 `Gpu*` 资源句柄混淆：

```cpp
namespace radray {

struct RgTextureHandle {
    uint32_t Id{0};
    constexpr bool IsValid() const noexcept { return Id != 0; }
};

struct RgBufferHandle {
    uint32_t Id{0};
    constexpr bool IsValid() const noexcept { return Id != 0; }
};

struct RgPassHandle {
    uint32_t Id{0};
    constexpr bool IsValid() const noexcept { return Id != 0; }
};

}  // namespace radray
```

这里的关键点不是句柄长什么样，而是：

- 图资源句柄和 runtime 资源句柄分离
- `Rg*Handle` 表示图里的逻辑节点
- `Gpu*Handle` 表示 runtime 管理的真实资源

### 4.2 Pass 类型和 pass flag

Pass 类型建议直接学 UE5，但按 RadRay 当前阶段收敛：

```cpp
enum class RgPassType : uint8_t {
    Raster,
    Compute,
    Copy,
    Unsafe,
};

enum class RgPassFlags : uint32_t {
    None          = 0,
    NeverCull     = 1 << 0,
    NeverMerge    = 1 << 1,
    NeverParallel = 1 << 2,

    // 仅预留，不在 v1 打开
    AsyncCompute  = 1 << 3,
};
```

这里没有单独公开 `Present` pass，原因不是它不重要，而是当前 [gpu_system.h](include/radray/runtime/gpu_system.h) 已经把 acquire / present 封装在 `GpuFrameContext` 与 `SubmitFrame` 语义里。对 RadRay v1，更自然的表达方式是：

- backbuffer 作为 imported texture 进入图
- 最终 pass 把它写成 color attachment 或 copy dst
- graph 在 epilogue 把它的最终访问语义收口到 `Present`

如果未来出现多 surface、离线图、图外 present 编排，再考虑显式 `PresentPass`。

### 4.3 当前边界状态模型

当前头文件在 imported 边界上没有单独公开 `RgAccess` 这类图边界抽象，而是继续沿用 runtime 已有的：

- `render::TextureState`
- `render::BufferState`

来表达资源进入 graph 前的外部已知状态；而在 graph 结束时，`Set*AccessFinal` 已经改为直接接受 graph 自己的 `RGTextureUseDesc` / `RGBufferUseDesc`。

这样做的现实原因是：

- render graph 需要先和现有 runtime / swapchain / history 资源边界直接接起来
- runtime 当前公开契约本来就围绕 `render::TextureState` / `render::BufferState`
- imported 资源的初始状态天然来自 graph 外部世界，因此沿用 runtime 状态模型最直接
- v1 优先保证最小功能循环闭合，而不是一次把边界抽象重新发明完整

但这里要明确一条实现约束：

- **图内部仍然应保留比 `render::TextureState` / `render::BufferState` 更丰富的 usage / access / stage / range 信息**
- **真正 lowering 到 barrier 时，应直接针对不同后端生成 D3D12 enhanced barriers 或 Vulkan barrier2 等结构，不经过 render 公共 API 再转一层**

也就是说，当前的 `render::TextureState` / `render::BufferState` 只用于 imported 边界契约，不应该成为 graph 内部资源同步计划的上限；final access 以及 pass 内访问仍应保留 graph 自己的 use desc 语义。

### 4.4 资源描述

```cpp
struct RGTextureDesc {
    render::TextureDescriptor NativeDesc{};
    bool ForceNonTransient{false};
};

struct RGBufferDesc {
    render::BufferDescriptor NativeDesc{};
    bool ForceNonTransient{false};
};

struct RGImportedTextureDesc {
    GpuTextureHandle Texture{};
    GpuTextureViewHandle DefaultView{};
    render::TextureDescriptor NativeDesc{};
    render::TextureState InitialState{render::TextureState::UNKNOWN};
    RGTextureBindingView InitialView{};
};

struct RGImportedBufferDesc {
    GpuBufferHandle Buffer{};
    render::BufferDescriptor NativeDesc{};
    render::BufferState InitialState{render::BufferState::UNKNOWN};
    RGBufferBindingView InitialView{};
};
```

这里保留 `ForceNonTransient`，是因为 UE5 在资源对象上长期保留 `bTransient / bForceNonTransient` 这种状态位是对的。即便初版 aliasing 还不做，也应该先把“这个资源不能被 transient 化”变成稳定语义。

### 4.5 导入、创建、提取与最终访问

```cpp
class RenderGraph {
public:
    void Reset() noexcept;

    RGTextureHandle ImportTexture(std::string_view name, const RGImportedTextureDesc& desc);
    RGBufferHandle ImportBuffer(std::string_view name, const RGImportedBufferDesc& desc);

    RGTextureHandle CreateTexture(std::string_view name, const RGTextureDesc& desc);
    RGBufferHandle CreateBuffer(std::string_view name, const RGBufferDesc& desc);

    GpuTextureHandle ExtractTexture(RGTextureHandle handle, const RGTextureBindingView& view, const RGTextureUseDesc& finalState);
    GpuBufferHandle ExtractBuffer(RGBufferHandle handle, const RGBufferBindingView& view, const RGBufferUseDesc& finalState);

    void SetTextureAccessFinal(RGTextureHandle handle, const RGTextureBindingView& view, const RGTextureUseDesc& useDesc);
    void SetBufferAccessFinal(RGBufferHandle handle, const RGBufferBindingView& view, const RGBufferUseDesc& useDesc);
};
```

这一组 API 是初版最重要的边界能力，因为它直接决定 graph 是否能接入真实 runtime：

- `Import*` 把图外资源注册进图
- `Extract*` 把图内资源带出图，并直接返回 graph 收口后的 runtime handle
- `Set*AccessFinal` 定义 epilogue 边界

这里有几个约束必须从一开始写死：

- imported 资源必须给出可解释的初始 state，或者显式声明为 `UNKNOWN`
- imported / extracted 边界都必须带上明确 view，不能把部分子资源访问偷偷隐含掉
- extracted 资源必须给出明确 final state
- `Set*AccessFinal` 必须给出完整的 use desc，而不是只给一个 render 层终态枚举
- graph-created 资源如果要长期离开图，必须先 `Extract*` 或等价 promote

这一版先只保留“图开始前导入、图结束后导出”这类第 1 类能力，不公开执行期 `UseExternalAccessMode` / `UseInternalAccessMode` 这种中途切换模型。后者可以等 v2 在 validation 和执行器都更稳定后再纳入。

这一层若没有，render graph 不能成为 runtime 中枢，只能成为 demo DSL。

## 5. Pass Builder 设计

### 5.1 为什么不用 UE5 那种参数反射

UE5 的精髓是“资源访问声明必须成为唯一可信输入”，不是“必须靠参数宏反射”。

RadRay v1 更适合的做法是：

- 用 Unity 风格 typed builder 作为公开形态
- 用 builder 调用本身作为依赖输入
- 让 `PassData` 只承载执行期需要的数据，不承载依赖声明机制

也就是说，我们复制 UE5 的语义强约束，但不复制它的反射外观。

### 5.2 公开的 builder 分类

```cpp
class RGRasterPassBuilder;
class RGComputePassBuilder;
class RGCopyPassBuilder;
class RGRasterShaderPassBuilder;
class RGComputeShaderPassBuilder;

class RenderGraph {
public:
    template <typename PassData, typename SetupFn>
    RGPassHandle AddRasterPass(std::string_view name, SetupFn&& setup, RGPassFlags flags = RGPassFlag::None);

    template <typename PassData, typename SetupFn>
    RGPassHandle AddComputePass(std::string_view name, SetupFn&& setup, RGPassFlags flags = RGPassFlag::None);

    template <typename PassData, typename SetupFn>
    RGPassHandle AddCopyPass(std::string_view name, SetupFn&& setup, RGPassFlags flags = RGPassFlag::None);

    template <typename PassData, typename SetupFn>
    RGPassHandle AddRasterShaderPass(std::string_view name, render::RootSignature* rootSignature, SetupFn&& setup, RGPassFlags flags = RGPassFlag::None);

    template <typename PassData, typename SetupFn>
    RGPassHandle AddComputeShaderPass(std::string_view name, render::RootSignature* rootSignature, SetupFn&& setup, RGPassFlags flags = RGPassFlag::None);
};
```

当前公开形态里实际有五种 builder：

- `Raster`：学 Unity 的 attachment 语义
- `Compute`：学 UE5 的 workload / flag 模型
- `Copy`：把 copy 单独做成语义类型，不和 compute 混在一起
- `RasterShader`：把 shader binding 与 raster attachment 合并到同一 builder
- `ComputeShader`：把 shader binding 与 compute pass 合并到同一 builder

### 5.2.1 Execute callback 也必须是 typed 的

builder 不仅要负责声明依赖，也要负责注册执行函数：

```cpp
template <typename PassData>
using RGRasterExecuteFn = void(*)(RGRasterPassContext&, const PassData&);

template <typename PassData>
using RGComputeExecuteFn = void(*)(RGComputePassContext&, const PassData&);

template <typename PassData>
using RGCopyExecuteFn = void(*)(RGCopyPassContext&, const PassData&);

template <typename PassData>
using RGRasterShaderExecuteFn = void(*)(RGRasterShaderPassContext&, const PassData&);

template <typename PassData>
using RGComputeShaderExecuteFn = void(*)(RGComputeShaderPassContext&, const PassData&);

class RGRasterPassBuilder : public RGPassBuilderBase {
public:
    template <typename PassData, typename ExecuteFn>
    void SetExecute(ExecuteFn&& fn);
};
```

这里建议延续 Unity / UE5 的思路：

- 一个 pass 只能注册一个 execute 函数
- `PassData` 在 setup 完成后视为只读
- execute 阶段只能访问声明过的图资源

### 5.3 通用资源访问声明

```cpp
class RGPassBuilderBase {
public:
    RGTextureHandle UseTexture(RGTextureHandle handle, const RGTextureUseDesc& desc);
    RGBufferHandle UseBuffer(RGBufferHandle handle, const RGBufferUseDesc& desc);

    RGTextureHandle CreateTransientTexture(std::string_view name, const RGTextureDesc& desc);
    RGBufferHandle CreateTransientBuffer(std::string_view name, const RGBufferDesc& desc);

    void AllowPassCulling(bool value);
};
```

这一层相当于 Unity `UseTexture / UseBuffer / CreateTransient* / AllowPassCulling` 的精简版，也是 RadRay 初版最核心的“输入语言”。

编译器内部应把这些高层调用降成更细 usage：

| 公开 API | 内部 lowering |
| --- | --- |
| `UseTexture(..., { .Access = SampledRead, ... })` | `Sampled` / `ShaderRead` |
| `UseTexture(..., { .Access = StorageWrite, ... })` | `StorageWrite` |
| `UseTexture(..., { .Access = StorageRead | StorageWrite, ... })` | `StorageReadWrite` |
| `UseBuffer(..., { .Access = ConstantRead / ShaderRead / IndirectRead, ... })` | 对应只读 buffer usage |
| `UseBuffer(..., { .Access = ShaderWrite / CopyWrite, ... })` | 对应写 buffer usage |
| `UseBuffer(..., { .Access = ShaderRead | ShaderWrite, ... })` | `StorageReadWrite` |

这一步要学 Godot，而不是公开给用户。

实现上应先生成 backend-neutral 的 graph 资源同步计划，再在最后一步按后端特化 lowering；不建议先降到 render 公共 API 再从 render API 反推 enhanced barriers / barrier2。

### 5.4 Raster builder 必须把 attachment 做成一等公民

```cpp
class RGRasterPassBuilder : public RGPassBuilderBase, public RGRasterAttachmentBuilderBase {
public:
    template <typename PassData, typename ExecuteFn>
    void SetExecute(ExecuteFn&& fn);
};
```

attachment 相关信息当前通过专门的 desc 结构显式给出：

- `RGColorAttachmentDesc`
- `RGDepthAttachmentDesc`
- `RGResolveTargetDesc`

这比只给一个 `handle + ops` 更贴近当前实现，因为它同时保留了 attachment view、aspect、clear/load/store 等信息。

如果 attachment 不是一等能力，会立刻出现两个问题：

1. render pass merge 无法建立稳定前提
2. 绝大多数主 raster 路径都只能退回手写后端命令

初版先不正式公开：

- `SetInputAttachment`
- `SetShadingRateAttachment`
- `FragmentDensityMap`

但 builder 内部设计要预留扩展位，不要把 attachment 模型做死。

### 5.5 Copy builder 应该显式限制语义

```cpp
class RGCopyPassBuilder : public RGPassBuilderBase {
public:
    void CopyTexture(const RGCopyTextureDesc& desc);
    void CopyBuffer(const RGCopyBufferDesc& desc);
    void CopyBufferToTexture(const RGCopyBufferToTextureDesc& desc);
    void CopyTextureToBuffer(const RGCopyTextureToBufferDesc& desc);
    void ResolveTexture(const RGResolveTextureDesc& desc);
};
```

把 `Copy` 做成单独 pass 类型有两个好处：

- 用户侧更不容易把 copy 和 compute / raster 混用
- 编译器更容易直接降成 `CopySource / CopyDest / ResolveSource / ResolveDest`

### 5.6 Shader builder 是当前的扩展点

当前头文件没有单独公开 `UnsafePass`。现阶段的扩展点是：

- `RGRasterShaderPassBuilder`
- `RGComputeShaderPassBuilder`

也就是说，系统优先鼓励“通过 root signature + binding layout 自动 lower shader 资源访问”，而不是先暴露一个无限自由的 escape hatch。

## 6. 执行上下文

建议给不同 pass 暴露 typed context，但都只暴露“执行期需要的东西”：

```cpp
class RGRasterPassContext {
public:
    GpuAsyncContext& RuntimeContext() noexcept;
    render::GraphicsCommandEncoder* Cmd() noexcept;

    render::Texture* GetTexture(RGTextureHandle handle) noexcept;
    render::TextureView* GetTextureView(const RGTextureBinding& binding) noexcept;
    render::Buffer* GetBuffer(RGBufferHandle handle) noexcept;
};

class RGComputePassContext { /* 同上，Cmd() 返回 render::ComputeCommandEncoder* */ };
class RGCopyPassContext { /* 同上，Cmd() 返回 render::CommandBuffer* */ };
class RGRasterShaderPassContext : public RGShaderPassContextBase { /* 同上 */ };
class RGComputeShaderPassContext : public RGShaderPassContextBase { /* 同上 */ };
```

这里不建议直接把完整 graph internals 暴露给 execute lambda。execute lambda 只应该看到：

- 当前可录制命令的 `render::CommandBuffer*`
- 已经分配好的真实资源对象
- 当前 runtime submission 上下文

这和 UE5 的 pass lambda / Unity 的 graph context 一样，都是“只给执行所需最小权限”。

## 7. Compile / Execute 行为契约

初版公开 API 至少应该定义下面这个生命周期：

```cpp
class RenderGraph {
public:
    bool Compile(Nullable<string*> reason = nullptr);
    bool Execute(GpuAsyncContext& context, Nullable<string*> reason = nullptr);
};
```

`Compile()` 内部最少要完成：

1. validation
2. 依赖闭包建立
3. pass culling
4. 资源 lifetime 求解
5. barrier 输入收集
6. raster merge 前置分析
7. epilogue final access / final use desc 收口

这一步要学 UE5 `Execute()` 之前的 compile 管线，也要学 Unity `BuildGraph -> CullUnusedRenderGraphPasses -> FindResourceUsageRangeAndSynchronization -> PrepareNativeRenderPasses` 这种明确阶段切分。

`Execute()` 内部则应该：

1. 分配 / 绑定真实资源
2. 录制 pass 命令
3. 插入必要 barrier
4. 处理 extraction
5. 收口 final access / final use desc

初版不要求对外暴露 compiled graph introspection API；先把公开契约定稳更重要。

## 8. Validation 契约

Validation 是 v1 必须公开承诺的能力，不是“以后再补的 debug 工具”。

建议至少在 debug 下强校验以下规则：

1. `RasterPass` 必须声明 color/depth attachment，除非以后明确支持 `SkipRenderPass`。
2. `ComputePass` 和 `CopyPass` 不能声明 attachment。
3. 同一纹理在同一 raster pass 内，初版禁止同时作为 attachment 和 sampled/storage 使用。
4. extracted 资源必须有明确 final state。
5. `Set*AccessFinal` 必须给出合法的 use desc，且 view 与资源类型匹配。
6. graph-created 资源如果在图外还要继续活着，必须通过 `Extract*` 暴露出去。
7. imported 资源必须在导入时给出初始 state 或明确 `UNKNOWN`。
8. 没有可观察输出、也没有 side effect 的 pass，如果不标记 `NeverCull`，编译器可以直接裁掉。
9. shader pass 中每个 `Bind(...)` 都必须能在 `BindingLayout` 里解析到合法 slot，且资源类型要匹配。
10. `CopyPass` 中不能混入 draw/dispatch 语义。

这些规则直接对应 UE5 `RenderGraphValidation` 和 Unity builder 里的 attachment / global state / culling 约束，只是按 RadRay 当前阶段收敛了一下。

## 9. 一段推荐用法

下面这段示例故意贴近 RadRay 当前 runtime，而不是照抄 UE/Unity：

```cpp
struct ToneMapPassData {
    RgTextureHandle HdrColor{};
    RgTextureHandle BackBuffer{};
};

RenderGraph graph{};

RGTextureHandle hdr = graph.CreateTexture("HdrColor", {
    .NativeDesc = hdrDesc,
});

RGTextureHandle backBuffer = graph.ImportTexture("BackBuffer", {
    .Texture = backBufferHandle,
    .DefaultView = backBufferViewHandle,
    .NativeDesc = backBufferDesc,
    .InitialState = render::TextureState::Present,
    .InitialView = {},
});

graph.AddRasterPass<ToneMapPassData>("ToneMap",
    [&](RGRasterPassBuilder& builder, ToneMapPassData& pass) {
        pass.HdrColor = builder.UseTexture(hdr, {
            .Access = RGTextureAccess::SampledRead,
            .Stages = RGStage::PixelShader,
        });
        pass.BackBuffer = backBuffer;

        builder.SetColorAttachment(0, {
            .Handle = backBuffer,
            .View = {},
            .Ops = {
                .Load = render::LoadAction::DontCare,
                .Store = render::StoreAction::Store,
            },
        });

        builder.AllowPassCulling(false);

        builder.SetExecute(
            [](RGRasterPassContext& ctx, const ToneMapPassData& pass) {
                auto* cmd = ctx.Cmd();
                auto* src = ctx.GetTexture(pass.HdrColor);
                auto* dst = ctx.GetTexture(pass.BackBuffer);
                // bind pipeline / descriptors / fullscreen draw
            });
    });

graph.SetTextureAccessFinal(backBuffer, {}, {
    .Access = RGTextureAccess::Present,
    .Stages = RGStage::Present,
});

string reason;
if (!graph.Compile(&reason)) {
    // report error
}

graph.Execute(frameContext, &reason);
```

这个例子体现的正是三家源码里最精华的那部分：

- 外形是 Unity 风格 typed raster builder
- 边界契约是 UE5 风格 import / epilogue 访问收口 / culling
- 内部 lowering 应该走 Godot 风格 usage/access/layout/hazard graph

## 10. v1 之后再开的能力

下面这些能力应该在 v1 之后再公开：

- `AsyncCompute`
- queue fork / join
- render pass merge 策略控制
- `UseExternalAccessMode / UseInternalAccessMode`
- input attachment
- shading rate attachment
- transient aliasing overlap hints
- graph blackboard
- graph introspection / visualization

这里最重要的原则是：不要为了“看起来先进”提前公开 async compute 或 aliasing 控制位。UE5 和 Godot 都说明，这些能力只有在资源访问、external boundary、validation 已经站稳后才值得暴露。

## 11. 最终结论

如果把这份草案压缩成一句话，那就是：

- **公开 API 学 Unity**
- **资源边界和 validation 学 UE5**
- **内部 lowering 和执行器学 Godot**

更具体一点：

1. 对用户公开 typed builder、attachment、import/create/transient/extract
2. 对外部世界公开 imported 初始 state、final use desc，以及图结束后的资源导出边界
3. 对编译器内部保留 usage/access/stage/range/hazard graph，并在最后按后端特化 lowering 到实际 barrier
4. 对工程长期可维护性，从 v1 就把 validation 当成正式功能

这才是最接近 RadRay 现阶段、也最不容易返工的一版 render graph 公开 API 起点。
