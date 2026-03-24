# Unity SRP / UE5 RDG / Godot Render Graph 实现调研报告

更新日期：2026-03-24

## 0. 任务范围与研究方法

本报告仅覆盖以下三套实现：

- Unity `Unity-Technologies/Graphics` 中 SRP RenderGraph
- Unreal Engine 5 `EpicGames/UnrealEngine` 中 RDG（Render Dependency Graph）
- Godot `godotengine/godot` 中 `RenderingDeviceGraph`

本次调研通过 GitHub CLI 直接查看远程源码，且在每次 `gh` 调用前都设置了代理：

```powershell
$env:HTTP_PROXY="http://127.0.0.1:10808"
$env:HTTPS_PROXY="http://127.0.0.1:10808"
```

调研重点不是“这些引擎有没有 render graph”这种概念层回答，而是尽量回答下面这些实现层问题：

1. 图的抽象层级在哪里
2. Pass 是如何声明和记录的
3. 依赖是如何推导出来的
4. 资源生命周期、导入/导出、池化、瞬态资源是怎么做的
5. 编译阶段做了哪些优化
6. 执行阶段如何落到命令缓冲和 barrier
7. 多队列、异步计算、验证、调试、标签系统如何落地
8. 这些实现对 RadRay 有什么直接可用的启发

---

## 1. 执行摘要

三套实现虽然都可归入“render graph”范畴，但抽象层级并不相同：

- **Unity SRP RenderGraph**：偏上层、偏“渲染框架 API”的 render graph。它给 SRP 作者提供 typed pass builder、资源句柄、attachment 声明、renderer list、global texture 传播等能力，再通过内部 compiler 编译成 native pass 执行计划。
- **UE5 RDG**：偏核心引擎层、偏“资源依赖和状态系统”的 render graph。它的重点是参数结构反射、访问声明、barrier 编译、生命周期管理、异步计算区间、提取/外部访问、强验证和并行编译执行。
- **Godot RenderingDeviceGraph**：偏驱动后端、偏“命令和资源 hazard 图”的实现。它不是 Unity 那种高层 frame graph DSL，而是 `RenderingDevice` 内部用来记录实际 buffer/texture/draw/compute/raytracing/callback 命令、建立依赖边、插入 barrier、重排执行顺序的低层 command/resource graph。

如果只用一句话概括：

- **Unity** 最适合参考“对渲染管线作者友好的 Pass API 设计”
- **UE5** 最适合参考“完整、严谨、可扩展的资源状态与执行模型”
- **Godot** 最适合参考“驱动层命令图、barrier 求解、命令重排和 secondary command buffer 执行器”

对于 RadRay，最值得采用的不是原样照搬某一家，而是：

- API 层参考 Unity 的 typed builder 和 attachment 语义
- 核心编译/资源状态层参考 UE5 RDG
- 后端执行器和命令重排层参考 Godot 的 `RenderingDeviceGraph`

---

## 2. 本次重点检查的源码范围

### 2.1 Unity

核心文件：

- `Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/RenderGraph.cs`
- `Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/RenderGraphBuilders.cs`
- `Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/RenderGraphPass.cs`
- `Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/RenderGraphResourceRegistry.cs`
- `Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/RenderGraphResourcePool.cs`
- `Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/Compiler/NativePassCompiler.cs`

### 2.2 UE5

核心文件：

- `Engine/Source/Runtime/RenderCore/Public/RenderGraphBuilder.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderGraphDefinitions.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderGraphPass.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderGraphResources.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderGraphValidation.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderGraphBlackboard.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraphBuilder.cpp`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraphValidation.cpp`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraphResourcePool.cpp`

### 2.3 Godot

核心文件：

- `servers/rendering/rendering_device_graph.h`
- `servers/rendering/rendering_device_graph.cpp`
- `servers/rendering/rendering_device.h`
- `servers/rendering/rendering_device.cpp`

---

## 3. 三套系统的总体结构判断

### 3.1 抽象层级

| 系统 | 抽象层级 | 面向对象 | 主要价值 |
| --- | --- | --- | --- |
| Unity SRP RenderGraph | 高层渲染框架 | SRP / 渲染器作者 | 更友好的 Pass 构建和资源声明 |
| UE5 RDG | 核心引擎渲染中枢 | 引擎渲染模块作者 | 全局资源依赖、状态和执行编译 |
| Godot RenderingDeviceGraph | 低层驱动调度器 | RenderingDevice 内部 | 命令 hazard 建模、barrier 插入、命令重排 |

### 3.2 图的本质

- Unity：图的主体是“**声明式 Pass + 资源访问关系**”，编译后再映射成 native render pass 和资源生命周期。
- UE5：图的主体是“**Pass 参数结构里声明的资源读写 + 明确的 pass flag + 编译出的 barrier/lifetime**”。
- Godot：图的主体是“**已记录好的具体命令 + 每条命令对资源的 usage + 命令间依赖边**”，更像一个 hazard-aware command scheduler。

### 3.3 依赖推导方式

- Unity：主要由 builder API 记录资源读写、render attachment、随机访问、global binding、renderer list 使用等信息，再由编译器构图。
- UE5：主要由 pass parameter struct 中的 RDG 资源字段、SRV/UAV、uniform buffer 等元数据自动反射，再结合 `ERDGPassFlags` 编译。
- Godot：调用 `add_*` 记录命令时立即把 `ResourceTracker* + ResourceUsage` 塞入图中，边和 barrier 在记录期就已经逐步建立。

### 3.4 执行形态

- Unity：先录制整帧，再调用 `EndRecordingAndExecute()` 一次性编译执行。
- UE5：先 `FRDGBuilder` 录制，再 `Execute()` 触发 compile、allocate、barrier collect、视图创建、pass 执行。
- Godot：`begin()` 开始清空内部记录，很多 `add_*` 直接累积命令与依赖，`end(reorder, full_barriers, ...)` 做排序、barrier 分组和执行。

---

## 4. Unity SRP RenderGraph

### 4.1 设计定位

Unity 的 RenderGraph 是 SRP 世界里的高层图系统。它最核心的设计目标不是暴露底层 barrier 细节，而是：

- 让渲染管线作者用 pass builder 声明资源依赖
- 让图系统接管资源生命周期、pass culling、pass merge、native render pass 编译
- 在保留 escape hatch 的同时，尽量让多数图工作在更高层的 attachment / texture / buffer / renderer list 语义上

它比 UE5 RDG 更“框架型”，比 Godot 更“用户侧”。

### 4.2 用户侧 Pass API

`RenderGraph.cs` 暴露的主要建图入口是：

```csharp
AddRasterRenderPass<PassData>()
AddComputePass<PassData>()
AddUnsafePass<PassData>()
```

这三个入口代表三种不同层级的 pass：

- `Raster`：面向传统图形渲染流程
- `Compute`：面向计算 pass
- `Unsafe`：逃逸口，给那些不适合强约束建模的场景使用

`RenderGraphBuilders.cs` 中 builder 的关键 API 包括：

```csharp
EnableAsyncCompute(bool)
AllowPassCulling(bool)
AllowGlobalStateModification(bool)

CreateTransientTexture(...)
CreateTransientBuffer(...)

UseTexture(TextureHandle, AccessFlags)
UseBuffer(BufferHandle, AccessFlags)
UseGlobalTexture(int propertyId, AccessFlags)
UseAllGlobalTextures(bool)
SetGlobalTextureAfterPass(TextureHandle, int propertyId)

SetRenderAttachment(TextureHandle, index, ...)
SetInputAttachment(TextureHandle, index, ...)
SetRenderAttachmentDepth(TextureHandle, ...)
SetRandomAccessAttachment(TextureHandle, index, ...)
UseBufferRandomAccess(BufferHandle, index, ...)

UseRendererList(RendererListHandle)
SetRenderFunc(...)
```

从这些 API 可以看出 Unity RenderGraph 的几个鲜明特点：

1. 它不是纯“纹理/缓冲区读写图”，而是把 **raster attachment 语义** 直接做成一等公民。
2. 它把 **全局纹理绑定传播** 纳入图系统，例如 `UseGlobalTexture`、`UseAllGlobalTextures`、`SetGlobalTextureAfterPass`。
3. 它允许 **随机访问 attachment / buffer**，满足 UAV 风格工作负载。
4. 它有 **renderer list** 集成，说明它不仅管资源，也管与 SRP 绘制列表相关的依赖。

### 4.3 Pass 内部记录的数据

`RenderGraphPass.cs` 中能看到每个 pass 至少跟踪了如下关键状态：

- `enableAsyncCompute`
- `allowPassCulling`
- `allowGlobalState`
- `resourceReadLists`
- `resourceWriteLists`
- `transientResourceList`
- `usedRendererListList`
- `setGlobalsList`
- `implicitReadsList`

这意味着 Unity 的 pass 记录不是“只记一个 lambda”，而是记了一整套依赖描述：

- 读哪些资源
- 写哪些资源
- 哪些资源是 transient
- 是否引用 renderer list
- pass 完成后向全局空间暴露哪些 texture
- 是否允许修改 global state
- 是否允许被 cull
- 是否放到 async compute

其中 `AllowGlobalStateModification(true)` 很重要，因为它会使 pass 更像 escape hatch。代码里也体现出这类 pass 会关闭常规 pass culling，原因很直接：一旦 pass 对图外部的全局状态有副作用，单靠资源读写关系已经不足以保证裁剪安全。

### 4.4 录制与执行生命周期

`RenderGraph.cs` 的主流程非常清晰：

```csharp
BeginRecording(in RenderGraphParameters)
// Add*Pass + builder 设置资源依赖 + SetRenderFunc
EndRecordingAndExecute()
```

内部执行链大体是：

```csharp
Execute()
  -> 计算 graph hash
  -> CompileNativeRenderGraph(graphHash)
  -> m_Resources.BeginExecute(m_CurrentFrameIndex)
  -> ExecuteNativeRenderGraph()
  -> ClearGlobalBindings()
EndExecute()
```

其中几个点值得注意：

#### 4.4.1 显式录制边界

- `BeginRecording(...)` 把图置于录制态
- `EndRecordingAndExecute()` 做编译和执行
- 图系统严格区分“录制 pass”与“执行 pass”

#### 4.4.2 图哈希

`Execute()` 中会计算 graph hash 并传给 `CompileNativeRenderGraph(graphHash)`。这说明 Unity 的实现不是每次都从零开始“纯解释执行”，而是有稳定结构识别和 native graph 编译入口。

#### 4.4.3 资源执行边界

- `m_Resources.BeginExecute(m_CurrentFrameIndex)`
- `m_Resources.EndExecute()`

说明资源注册表不是一个纯静态数据库，而是和帧执行边界绑定，执行前后会切换状态。

#### 4.4.4 全局绑定清理

`ClearGlobalBindings()` 是个很容易被忽略但很关键的细节。因为 Unity 把 global texture binding 纳入图语义，所以执行完成后需要清理这些状态，避免帧间污染。

### 4.5 编译器：NativePassCompiler

`Compiler/NativePassCompiler.cs` 体现了 Unity 的 render graph compiler 思路。核心编译阶段包括：

```csharp
ValidatePasses()
SetupContextData(resources)
BuildGraph()
CullUnusedRenderGraphPasses()
TryMergeNativePasses()
FindResourceUsageRangeAndSynchronization()
DetectMemoryLessResources()
PrepareNativeRenderPasses()
```

这条链路值得逐项看：

#### 4.5.1 `ValidatePasses()`

编译前先做 pass 合法性检查。这与 UE5 强验证思路不同程度一致，只是 Unity 的验证点更多围绕 builder 合法用法、pass 配置一致性和图构建正确性。

#### 4.5.2 `SetupContextData(resources)`

将资源注册表和编译期上下文对齐，为后续构图、pass 合并、native pass 准备数据。

#### 4.5.3 `BuildGraph()`

根据 pass 记录的读写/attachment/renderer list/global state 等信息构出依赖图。

#### 4.5.4 `CullUnusedRenderGraphPasses()`

Unity 显式支持 pass culling。未被最终输出链路需要的 pass 会被裁掉，但带副作用、global state 修改、或被显式禁止裁剪的 pass 不会简单被删。

#### 4.5.5 `TryMergeNativePasses()`

这是 Unity 的一个非常关键差异点：它强烈面向 raster/native render pass 优化。也就是说，图中的逻辑 pass 不一定一一对应底层 native pass，编译器会尝试合并兼容的原生 pass，以减少 render pass 切换和 attachment load/store 开销。

#### 4.5.6 `FindResourceUsageRangeAndSynchronization()`

这个阶段负责识别资源使用区间和同步需求。虽然 Unity 没有像 UE 那样把 barrier 语义公开到同等强度的资源状态模型，但内部仍然要解 lifetime 和 synchronization。

#### 4.5.7 `DetectMemoryLessResources()`

这说明 Unity 编译器已经考虑 tile-based / memoryless attachment 类型优化。对于移动 GPU 或 tile-based 架构，这类检测很有价值。

#### 4.5.8 `PrepareNativeRenderPasses()`

最终把逻辑图整理成可直接执行的 native render pass 数据结构。

### 4.6 资源系统：Registry + Pool

Unity 的资源系统不是单一类，而是由资源注册表和资源池配合完成。

#### 4.6.1 `RenderGraphResourceRegistry`

主要职责：

- 导入外部 texture / buffer / backbuffer
- 创建图内 texture / buffer
- 检查 handle 合法性
- 在执行期间创建/释放 pooled resource

关键入口包括：

```csharp
BeginExecute(int currentFrameIndex)
EndExecute()
ImportTexture(...)
ImportBackbuffer(...)
ImportBuffer(...)
CreateTexture(...)
CreateBuffer(...)
CreatePooledResource(...)
ReleasePooledResource(...)
```

这说明 Unity 的资源视图分成三类：

1. **Imported**：外部资源进入图
2. **Created**：图内新建资源
3. **Pooled**：执行期实际底层资源对象的复用与分配

#### 4.6.2 `RenderGraphResourcePool`

资源池负责跨帧复用，源码中有一个很具体的常量：

```csharp
const int kStaleResourceLifetime = 10;
```

即资源如果在若干帧内没有被重用，会被视为 stale 并清掉。这是非常典型的 frame-temporal pooling 策略。

#### 4.6.3 Transient 资源

Builder 提供：

- `CreateTransientTexture`
- `CreateTransientBuffer`

而 pass 中有 `transientResourceList`。说明 transient 不是“普通 create 资源的别名”，而是被编译器和资源系统显式识别，用于缩短生命周期、提高别名复用机会。

### 4.7 Unity RenderGraph 的实现特征总结

#### 4.7.1 优点

- API 友好，适合管线作者使用
- attachment 语义强，天然适合 raster pass
- 支持 pass culling
- 支持 native pass merge
- 支持 transient 资源与池化
- 支持 global texture 传播
- 有 async compute 入口
- 有 `UnsafePass` 这种工程上很务实的逃生门

#### 4.7.2 局限

- 资源状态模型对用户并不如 UE5 RDG 那样清晰和强显式
- 高层 API 很方便，但也意味着很多后端状态推导发生在内部，扩展底层执行模型时可控性不如 UE5
- 它更偏 SRP 场景，不完全等价于一个“通用底层渲染依赖图内核”

#### 4.7.3 一个关键判断

Unity 的 RenderGraph 本质上是一个“**高层 frame graph + 内部 native pass 编译器**”体系，而不是一个完全公开底层状态机的 RDG 内核。

---

## 5. UE5 RDG

### 5.1 设计定位

UE5 RDG 是这三家里最“中枢化”和“系统化”的实现。`FRDGBuilder` 文件开头的注释就明确说明：

- barrier 和 resource lifetime 来自 pass parameter struct 里的 RDG 参数
- 图会在 `Execute()` 中被 compile、cull、execute

这句话基本定义了 UE5 RDG 的哲学：

- 开发者声明资源关系
- 系统根据声明做依赖、barrier、生命周期和执行调度
- 参数结构是图推导的核心输入

### 5.2 Builder 侧主要 API

`RenderGraphBuilder.h` 暴露的关键入口包括：

```cpp
RegisterExternalTexture(...)
RegisterExternalBuffer(...)

CreateTexture(...)
CreateBuffer(...)
CreateSRV(...)
CreateUAV(...)
CreateUniformBuffer(...)

AllocParameters(...)

AddPass(...)
AddDispatchPass(...)

QueueTextureExtraction(...)
QueueBufferExtraction(...)

SetTextureAccessFinal(...)
SetBufferAccessFinal(...)

UseExternalAccessMode(...)
UseInternalAccessMode(...)

SkipInitialAsyncComputeFence()

FRDGBlackboard Blackboard;
```

这组 API 很有代表性：

1. **资源创建和视图创建是图内一等能力**
2. **参数结构分配是 builder 的核心接口**，意味着参数生命周期和图生命周期绑定
3. **外部资源注册、资源提取、最终访问状态设置** 是标准流程，不是边缘功能
4. **外部访问模式切换** 是正式支持的，说明 RDG 很重视“图内资源临时借给外部使用，再恢复内部跟踪”这类复杂流程
5. **Blackboard** 提供跨 pass / 跨模块传递中间对象的机制

### 5.3 Pass Flag 体系

`RenderGraphDefinitions.h` 中 `ERDGPassFlags` 非常关键：

```cpp
None = 0
Raster = 1 << 0
Compute = 1 << 1
AsyncCompute = 1 << 2
Copy = 1 << 3
NeverCull = 1 << 4
SkipRenderPass = 1 << 5
NeverMerge = 1 << 6
NeverParallel = 1 << 7
Readback = Copy | NeverCull
```

这套 flag 非常能体现 UE RDG 的成熟度：

- `Raster / Compute / AsyncCompute / Copy` 直接描述工作负载类型
- `NeverCull` 明确表达副作用/不可裁剪语义
- `SkipRenderPass` 用于跳过默认 render pass begin/end，并且会禁用 render pass merge
- `NeverMerge` 和 `NeverParallel` 分别控制两个非常实际的编译优化维度
- `Readback` 则是工程化包装出来的组合语义

相比 Unity：

- Unity 把很多语义隐藏在 builder 方法里
- UE5 则把相当一部分执行语义显式编码进 pass flags

### 5.4 Pass 对象内部状态

`RenderGraphPass.h` 中 `FRDGPass` 不只是一个 lambda 容器。它内部至少包含：

- `Name`
- `ParameterStruct`
- `Flags`
- `TaskMode`
- `Pipeline`
- `Handle`
- `Workload`

以及一组重要执行状态位：

- `bSkipRenderPassBegin`
- `bSkipRenderPassEnd`
- `bAsyncComputeBegin`
- `bAsyncComputeEnd`
- `bGraphicsFork`
- `bGraphicsJoin`
- `bRenderPassOnlyWrites`
- `bSentinel`
- `bDispatchAfterExecute`

这些状态揭示出 UE RDG 在执行层做了远比“按顺序调用 lambda”更复杂的事情：

1. 它会显式构造 **异步计算区间**
2. 它会给图形队列和异步计算队列建立 **fork/join** 关系
3. 它支持 **sentinel pass**（例如 prologue / epilogue）
4. 它对 render pass begin/end 是否被跳过、是否只在 render pass 内写资源，都有单独状态

### 5.5 资源模型

`RenderGraphResources.h` 是 UE RDG 最值得细读的部分之一。资源对象上可看到大量状态位：

- `bExternal`
- `bExtracted`
- `bProduced`
- `bTransient`
- `bForceNonTransient`
- `ReferenceCount`
- `AliasingOverlaps`
- `LastProducer` / `LastProducers`

还有一个非常重要的判断：

```cpp
bool IsCullRoot() const
{
    return bExternal || bExtracted;
}
```

这句话很关键，因为它说明 UE5 的 pass/resource culling 不是简单从“最终 backbuffer 输出”倒推，而是有一套一般化的根定义：

- 外部资源
- 被提取出的资源

都可以成为 cull root。

#### 5.5.1 这些状态位意味着什么

- `bExternal`：资源来自图外部，图不能完全控制它的初始存在性
- `bExtracted`：资源在图执行后要交回外部继续使用，因此不能在图中被随意消亡
- `bProduced`：资源是否在图中真正被生产
- `bTransient`：走 transient allocator / aliasing 逻辑
- `bForceNonTransient`：即便可 transient，也强制不要 transient
- `ReferenceCount`：生命周期回收的核心计数
- `AliasingOverlaps`：记录 transient aliasing 期间的重叠信息
- `LastProducer`：用于 barrier 和状态推进

#### 5.5.2 View 也是图的一部分

UE RDG 不只管 texture/buffer 本体，也把 SRV/UAV/View 的创建纳入 compile pipeline。它会在执行前 `CreateViews(...)`，并让 uniform buffer 创建依赖 view 有效。

### 5.6 外部资源与提取机制

UE5 RDG 对“图和图外世界的边界”定义得非常完整：

#### 5.6.1 注册外部资源

- `RegisterExternalTexture`
- `RegisterExternalBuffer`

图开始时把已有 RHI / pooled 资源接进来。

#### 5.6.2 提取图内资源

- `QueueTextureExtraction`
- `QueueBufferExtraction`

图结束时把资源交给外部。

#### 5.6.3 最终访问状态

- `SetTextureAccessFinal`
- `SetBufferAccessFinal`

这很重要，因为它让“图结束后资源处于什么状态”成为显式契约，而不是隐含行为。

#### 5.6.4 外部访问模式切换

- `UseExternalAccessMode`
- `UseInternalAccessMode`

这套机制是 UE RDG 非常工程化、也非常成熟的一部分。很多引擎只做“import / export”，但 UE5 允许图内资源在某个阶段切到外部访问模式，然后再恢复图内跟踪。

### 5.7 编译与执行流水线

`RenderGraphBuilder.cpp` 体现出的流水线大致是：

```cpp
Execute()
  -> Compile()
  -> CompilePassBarriers()
  -> CollectPassBarriers()
  -> Allocate pooled / transient resources
  -> CreateViews()
  -> 创建 uniform buffers / 上传 buffer
  -> ExecutePass(...)
```

从源码细节看，UE5 在执行前会插入 sentinel pass，例如：

- `Graph Prologue (Graphics)`
- `Graph Epilogue`

这进一步说明它的执行计划是显式编排过的，而不是直接遍历用户 pass 列表。

#### 5.7.1 `Compile()`

编译阶段至少会处理：

- pass culling
- render pass merge 条件
- async compute interval
- 资源生命周期收集

源码中还能看到诸如：

- `AsyncComputePassCount`
- `RasterPassCount`

说明编译行为会根据图内 pass 类型统计结果走不同分支。

#### 5.7.2 Barrier 编译

关键方法：

- `CompilePassBarriers()`
- `CollectPassBarriers()`

UE5 的 barrier 模型非常强，它不是记录期就一股脑插 barrier，而是在图编译后结合最终 pass 序和 pipeline 情况统一生成。

#### 5.7.3 资源分配

关键方法：

- `AllocateTransientResources(...)`
- `AllocatePooledTextures(...)`
- `AllocatePooledBuffers(...)`

背后对应：

- `TransientResourceAllocator`
- `GRenderTargetPool`
- `GRenderGraphResourcePool`

也就是说 UE5 同时拥有：

1. **瞬态分配器**
2. **普通 pooled texture 分配**
3. **普通 pooled buffer 分配**

#### 5.7.4 视图与上传任务

关键方法：

- `CreateViews(...)`
- `SubmitBufferUploads(...)`

同时源码可见大量 `UE::Tasks` 任务调度，说明编译和执行准备阶段能够并行化：

- barrier collect
- pooled 资源分配
- transient 分配
- view 创建
- uniform buffer 准备
- buffer 上传

这也是 UE RDG 比 Unity 和 Godot 更像“全引擎渲染调度中枢”的原因之一。

### 5.8 异步计算

UE5 RDG 的 async compute 不是简单给 pass 打个标签就结束，而是有完整区间语义：

- pass flag 有 `AsyncCompute`
- pass 状态有 `bAsyncComputeBegin / bAsyncComputeEnd`
- 图形侧还有 `bGraphicsFork / bGraphicsJoin`
- builder 还有 `SkipInitialAsyncComputeFence()`

这说明它不仅知道“某个 pass 在 async compute 上跑”，还知道：

- 区间从哪里开始
- 在哪里和 graphics 分叉
- 在哪里 join 回来
- 初始 fence 是否可省略

这套模型比 Unity 的 `EnableAsyncCompute(bool)` 强得多，也比 Godot 当前图中那种“主要通过 command dependency + stage barrier”管理的方式更抽象、更系统。

### 5.9 验证系统

`RenderGraphValidation.cpp` 是 UE5 RDG 的护城河之一。可见的验证入口包括：

- `ValidateCreateTexture`
- `ValidateCreateBuffer`
- `ValidateExtractTexture`
- `ValidateExtractBuffer`
- `ValidateUseExternalAccessMode`
- `ValidateUseInternalAccessMode`
- `ValidateAddPass`
- `ValidateExecutePassBegin`
- `ValidateExecutePassEnd`

已确认的重要验证点包括：

- pass flag 与 pass 工作负载是否匹配
- 没有图跟踪输出的 pass 是否必须 `NeverCull`
- barrier 和访问模式是否合法
- 是否在允许的执行窗口之外访问 RHI 资源
- 外部访问模式切换是否合理

这意味着 UE RDG 从设计上就是“强约束系统”，并不鼓励开发者把它当成弱语法糖层。

### 5.10 UE5 RDG 的实现特征总结

#### 5.10.1 优点

- 资源状态模型最完整
- barrier/lifetime/extraction/final access 都做成一等能力
- async compute 支持最系统
- 验证最强
- 支持并行编译和执行准备
- transient aliasing 能力最成熟

#### 5.10.2 成本

- 理解门槛最高
- 参数结构、访问声明、pass flags 约束较重
- 对引擎基础设施依赖非常深，不适合直接照抄到小型项目

#### 5.10.3 一个关键判断

UE5 RDG 更接近“**渲染后端核心调度内核**”，不是单纯的高层 frame graph 语法。

---

## 6. Godot RenderingDeviceGraph

### 6.1 设计定位

Godot 当前这套实现的名字虽然也叫 graph，但它和 Unity / UE5 的层级不同。

最准确的理解方式是：

- 它是 `RenderingDevice` 内部的 **命令记录、资源 hazard 跟踪、barrier 插入和执行重排器**
- 它不是一个给上层渲染器作者暴露的高层 typed frame graph DSL

换句话说，Godot 的实现更接近：

- 低层 command graph
- 资源 usage graph
- 驱动执行器

而不是“高层 pass 描述语言”。

### 6.2 生命周期入口

`rendering_device_graph.h/.cpp` 的主要外部入口是：

```cpp
initialize(...)
finalize()
begin()
end(bool reorder, bool full_barriers, ...)
begin_label(...)
end_label()
resource_tracker_create()
resource_tracker_free()
framebuffer_cache_create()
framebuffer_cache_free()
```

#### 6.2.1 `initialize(...)`

`RenderingDeviceGraph::initialize(...)` 做的事很具体：

- 保存 `driver`
- 保存 `device`
- 保存 `render_pass_creation_function`
- 创建 frame ring：`frames.resize(p_frame_count)`
- 为每帧创建 `secondary_command_buffers`
- 为每个 secondary buffer 创建：
  - `command_pool`
  - `command_buffer`
- 读取底层 API trait：
  - `API_TRAIT_HONORS_PIPELINE_BARRIERS`
  - `API_TRAIT_CLEARS_WITH_COPY_ENGINE`
  - `API_TRAIT_BUFFERS_REQUIRE_TRANSITIONS`

这说明 Godot 的 graph 从一开始就是面向真实驱动差异和 secondary command buffer 执行模型的。

#### 6.2.2 `finalize()`

`finalize()` 会：

- 等待 secondary command buffer 相关任务结束
- 释放各帧的 secondary command pool
- 清空 `frames`

#### 6.2.3 `begin()`

`begin()` 会清空大量内部记录容器：

- `command_data`
- `command_data_offsets`
- `command_normalization_barriers`
- `command_transition_barriers`
- `command_buffer_barriers`
- `command_acceleration_structure_barriers`
- label 相关缓冲
- adjacency / slice list 节点

并重置：

- `command_count`
- `command_timestamp_index`
- `command_synchronization_index`
- `command_synchronization_pending`
- 当前 label
- draw/compute instruction list 索引
- `tracking_frame++`

这说明 Godot 的 graph 是标准“每帧 begin，记录命令，end 执行”的环形工作流。

### 6.3 记录的命令类型

`RecordedCommand::Type` 枚举中可以看到 Godot graph 直接记录具体命令：

- `TYPE_ACCELERATION_STRUCTURE_BUILD`
- `TYPE_BUFFER_CLEAR`
- `TYPE_BUFFER_COPY`
- `TYPE_BUFFER_GET_DATA`
- `TYPE_BUFFER_UPDATE`
- `TYPE_COMPUTE_LIST`
- `TYPE_RAYTRACING_LIST`
- `TYPE_DRAW_LIST`
- `TYPE_TEXTURE_CLEAR_COLOR`
- `TYPE_TEXTURE_CLEAR_DEPTH_STENCIL`
- `TYPE_TEXTURE_COPY`
- `TYPE_TEXTURE_GET_DATA`
- `TYPE_TEXTURE_RESOLVE`
- `TYPE_TEXTURE_UPDATE`
- `TYPE_CAPTURE_TIMESTAMP`
- `TYPE_DRIVER_CALLBACK`

而且 compute / draw / raytracing 不是简单一个命令，而是各自还有内部 instruction list：

#### 6.3.1 Raytracing list

- `add_raytracing_list_begin`
- `add_raytracing_list_bind_pipeline`
- `add_raytracing_list_bind_uniform_set`
- `add_raytracing_list_set_push_constant`
- `add_raytracing_list_trace_rays`
- `add_raytracing_list_uniform_set_prepare_for_use`
- `add_raytracing_list_usage`
- `add_raytracing_list_end`

#### 6.3.2 Compute list

- `add_compute_list_begin`
- `add_compute_list_bind_pipeline`
- `add_compute_list_bind_uniform_set`
- `add_compute_list_bind_uniform_sets`
- `add_compute_list_dispatch`
- `add_compute_list_dispatch_indirect`
- `add_compute_list_set_push_constant`
- `add_compute_list_uniform_set_prepare_for_use`
- `add_compute_list_usage`
- `add_compute_list_end`

#### 6.3.3 Draw list

- `add_draw_list_begin`
- `add_draw_list_bind_index_buffer`
- `add_draw_list_bind_pipeline`
- `add_draw_list_bind_uniform_set`
- `add_draw_list_bind_uniform_sets`
- `add_draw_list_bind_vertex_buffers`
- `add_draw_list_clear_attachments`
- `add_draw_list_draw`
- `add_draw_list_draw_indexed`
- `add_draw_list_draw_indirect`
- `add_draw_list_draw_indexed_indirect`
- `add_draw_list_execute_commands`
- `add_draw_list_next_subpass`
- `add_draw_list_set_blend_constants`
- `add_draw_list_set_line_width`
- `add_draw_list_set_push_constant`
- `add_draw_list_set_scissor`
- `add_draw_list_set_viewport`
- `add_draw_list_uniform_set_prepare_for_use`
- `add_draw_list_usage`
- `add_draw_list_end`

这和 Unity / UE5 很不同：Godot graph 记录的是已经接近驱动调用级别的操作序列，不是上层“逻辑 pass”。

### 6.4 资源使用模型：`ResourceUsage`

Godot 在头文件中定义了完整的 `ResourceUsage` 枚举：

- `RESOURCE_USAGE_COPY_FROM`
- `RESOURCE_USAGE_COPY_TO`
- `RESOURCE_USAGE_RESOLVE_FROM`
- `RESOURCE_USAGE_RESOLVE_TO`
- `RESOURCE_USAGE_UNIFORM_BUFFER_READ`
- `RESOURCE_USAGE_INDIRECT_BUFFER_READ`
- `RESOURCE_USAGE_TEXTURE_BUFFER_READ`
- `RESOURCE_USAGE_TEXTURE_BUFFER_READ_WRITE`
- `RESOURCE_USAGE_STORAGE_BUFFER_READ`
- `RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE`
- `RESOURCE_USAGE_VERTEX_BUFFER_READ`
- `RESOURCE_USAGE_INDEX_BUFFER_READ`
- `RESOURCE_USAGE_TEXTURE_SAMPLE`
- `RESOURCE_USAGE_STORAGE_IMAGE_READ`
- `RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE`
- `RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE`
- `RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ_WRITE`
- `RESOURCE_USAGE_ATTACHMENT_FRAGMENT_SHADING_RATE_READ`
- `RESOURCE_USAGE_ATTACHMENT_FRAGMENT_DENSITY_MAP_READ`
- `RESOURCE_USAGE_GENERAL`
- `RESOURCE_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT`
- `RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ`
- `RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ_WRITE`

这里有一个重要观察：

- Unity 更多是 builder 级语义
- UE5 更多是 RHI access + pass flag + parameter reflection
- Godot 则非常直接地定义“某条命令如何使用某个资源”

它的 usage 枚举和 barrier 求解是直接耦合的。

### 6.5 `ResourceTracker`

`ResourceTracker` 是 Godot 这套实现的核心数据结构之一。字段非常丰富：

- `reference_count`
- `command_frame`
- `previous_frame_stages`
- `current_frame_stages`
- `read_full_command_list_index`
- `read_slice_command_list_index`
- `write_command_or_list_index`
- `draw_list_index`
- `draw_list_usage`
- `compute_list_index`
- `compute_list_usage`
- `raytracing_list_index`
- `raytracing_list_usage`
- `usage`
- `usage_access`
- `buffer_driver_id`
- `texture_driver_id`
- `acceleration_structure_driver_id`
- `texture_subresources`
- `texture_size`
- `texture_usage`
- `texture_slice_command_index`
- `parent`
- `dirty_shared_list`
- `next_shared`
- `texture_slice_or_dirty_rect`
- `in_parent_dirty_list`
- `write_command_list_enabled`
- `is_discardable`

它本质上做了几件事：

1. 记资源在当前图中的读写历史
2. 记资源在前一帧和当前帧的 pipeline stages
3. 对 texture slice / dirty rect / parent-child slice 做专门跟踪
4. 对 draw / compute / raytracing list 内的特殊资源使用做额外索引
5. 为 barrier 生成和命令依赖建立提供历史信息

其中 `reset_if_outdated(new_command_frame)` 也非常重要。它在切换新记录帧时：

- 把 `previous_frame_stages = current_frame_stages`
- 清空当前帧各类索引
- 重置写列表和 slice tracking 状态

也就是说 Godot 会把“上一帧的阶段信息”带到下一帧继续用，这对跨帧资源状态延续是必要的。

### 6.6 FramebufferCache

Godot graph 内部还有 `FramebufferCache`：

- `width`
- `height`
- `textures`
- `trackers`
- `storage_map`
- `render_pass_creation_user_data`

这说明 Godot 在 draw list / render pass 级别也有缓存层：

- 一组 attachment 对应的 framebuffer / render pass 组合会被缓存
- cache 销毁时会显式释放 `framebuffer` 和 `render_pass`

这部分体现出它虽然不是高层 frame graph DSL，但它仍然在底层执行器里做 render pass / framebuffer 复用。

### 6.7 图构建：命令记录期即建立依赖与 barrier

Godot 的 `_add_command_to_graph(...)` 是最值得关注的函数之一。根据源码已确认它会做这些事：

#### 6.7.1 命令存储

- `_allocate_command()` 把命令对象打包到对齐后的字节缓冲 `command_data`
- `command_data_offsets` 保存每条命令的偏移

这是一种很后端化、很 cache-friendly 的记录方式。

#### 6.7.2 标签与时间戳处理

- 记录当前 label 层级和颜色
- 处理 timestamp 命令相邻关系
- 处理显式 synchronization 标记

#### 6.7.3 资源 tracker 刷新

- 每个 tracker 在使用前调用 `reset_if_outdated(tracking_frame)`
- 保证跨帧状态推进正确

#### 6.7.4 texture slice / parent / dirty 区域处理

- 管理 slice tracker 与 parent tracker 关系
- 维护 dirty shared list
- 跟踪局部覆盖与共享污染

#### 6.7.5 barrier 生成

它会根据 usage 和历史状态向命令挂接多种 barrier：

- memory barrier
- normalization barrier
- transition barrier
- buffer barrier
- acceleration structure barrier

此外它还会维护：

- `previous_stages`
- `next_stages`

#### 6.7.6 邻接边建立

Godot graph 不是后处理式推导，而是在记录命令时，就根据先前读写历史增量建立 adjacency：

- 从先前 writer 到当前 reader
- 从先前 reader / writer 到当前 writer
- 从 slice / full resource 交集情况建立局部依赖

`_add_adjacent_command(...)` 会更新依赖边与阶段传播。

### 6.8 usage 到 barrier / layout 的映射

Godot 实现里有几组非常关键的映射函数：

- `_usage_to_image_layout()`
- `_usage_to_access_bits()`
- `_is_write_usage()`

这意味着 Godot 的 barrier 生成不是基于高层 pass 语义，而是基于：

1. 这条命令如何使用资源
2. 之前如何使用
3. 使用范围是否相交
4. 是否读写冲突

对于 texture，还会配合：

- `_check_command_intersection()`
- `_check_command_partial_coverage()`

这些函数尤其处理 attachment 写、区域覆盖、slice 局部重叠等情况。

### 6.9 `end(...)`：拓扑排序、层级划分、分组执行

`RenderingDeviceGraph::end(bool p_reorder_commands, bool p_full_barriers, ...)` 是 Godot 这套系统的调度核心。

#### 6.9.1 无命令时直接返回

如果 `command_count == 0`，直接结束。

#### 6.9.2 命令重排

当 `p_reorder_commands` 为真时，Godot 会：

1. 遍历 adjacency list 统计每个节点入度
2. 用类似 Kahn 算法的方式做拓扑排序
3. 为每个节点计算 `level`
4. 给命令打 `priority`
5. 排序后按 level 分批执行

这说明 Godot 的“graph”不是名义上的，它真的会利用图结构做命令重排。

#### 6.9.3 优先级

当前 inspected 版本中显式可见的 priority group 至少包括：

- buffer 类操作优先级较低但会聚合在一起
- texture 类操作聚合
- draw list 聚合
- compute list 聚合
- driver callback 单独作为更高优先级组

源码中对应的 priority 表清楚表达了“按命令类型聚批处理”的意图。

#### 6.9.4 barrier 分组与执行

在重排路径下，执行大致是：

1. `commands_sorted.sort()`
2. 按 level 切片
3. `_boost_priority_for_render_commands(...)`
4. `_group_barriers_for_render_commands(...)`
5. `_run_render_commands(...)`

在不重排路径下，则是按记录顺序逐条分组 barrier 并执行。

#### 6.9.5 帧推进

执行完成后：

```cpp
frame = (frame + 1) % frames.size();
```

所以 secondary command buffer 使用的是一个 frame ring。

### 6.10 执行器细节

`_run_render_commands(...)`、`_run_label_command_change(...)` 等函数体现出很多后端执行器细节：

- 支持 split command buffer
- 支持 draw pass begin/end
- 支持 compute workaround：`avoid_compute_after_draw`
- 执行 texture/buffer copy/update/resolve
- 运行 draw/compute/raytracing instruction list
- 按 label 层级推送 GPU label
- label 名字会附带层级和检测到的操作混合类型，例如 Copy/Compute/Draw/Custom

这说明 Godot 不只是“分析图”，它就是实际的 command buffer 录制/提交准备器。

### 6.11 secondary command buffer

Godot 的一个很实用特点是 secondary command buffer 支持已经内置到图里：

- 初始化阶段为每帧预建 secondary command pool / command buffer
- 存在 `_run_secondary_command_buffer_task`
- 存在 `_wait_for_secondary_command_buffer_tasks`
- graph 能利用 worker thread 为 secondary command buffer 做录制任务

这对于 draw-heavy 场景很有价值。

### 6.12 与 `RenderingDevice` 的集成方式

`rendering_device.cpp` 里已经可以看到：

- `#define RENDER_GRAPH_REORDER 1`
- `#define RENDER_GRAPH_FULL_BARRIERS 0`

也就是说当前 inspected 版本默认：

- **开启命令重排**
- **关闭 full barrier 模式**

`RenderingDevice` 持有：

```cpp
RenderingDeviceGraph draw_graph;
```

并在合适的生命周期中调用：

- `draw_graph.initialize(...)`
- `draw_graph.begin()`
- `draw_graph.end(RENDER_GRAPH_REORDER == 1, RENDER_GRAPH_FULL_BARRIERS == 1, ...)`
- `draw_graph.finalize()`

此外，`RenderingDevice` 还维护：

- `dependency_map`
- `reverse_dependency_map`

以及一系列 “make mutable” 辅助函数：

- `_texture_make_mutable`
- `_buffer_make_mutable`
- `_vertex_array_make_mutable`
- `_index_array_make_mutable`
- `_uniform_set_make_mutable`
- `_dependency_make_mutable`
- `_dependencies_make_mutable_recursive`
- `_dependencies_make_mutable`

这些逻辑的意义是：

- 某个资源被更新、复制、上传、绑定修改时，不只是它自己会受影响
- 与其关联的 dependent resources 也必须变成“可变、需重新跟踪”的状态

在很多需要打断既有依赖关系的路径上，`RenderingDevice` 还会主动插：

- `draw_graph.add_synchronization()`

这说明 Godot 的 graph 不只是自动推导，还允许上层在特定 driver 语义切换点手工插同步屏障标记。

### 6.13 Godot 这套实现的特征总结

#### 6.13.1 优点

- 非常贴近真实命令执行模型
- 资源 usage 到 barrier/layout 映射清晰
- 命令重排是实打实的
- 对 texture slice / dirty rect 跟踪细
- secondary command buffer 支持到位
- 与驱动 trait 和 workaround 深度耦合，工程可用性强

#### 6.13.2 局限

- 抽象层级较低，不适合直接作为上层渲染器作者 API
- 没有 Unity 那种高层 pass/builder 体验
- 没有 UE5 那种完整的统一参数结构反射和强验证体系
- 更像一个后端执行图，而不是完整的前后端统一 RDG 平台

#### 6.13.3 一个关键判断

Godot 的 `RenderingDeviceGraph` 更准确的名字其实可以理解成“**command/resource hazard graph executor**”。

---

## 7. 横向对比

### 7.1 抽象风格对比

| 维度 | Unity SRP | UE5 RDG | Godot |
| --- | --- | --- | --- |
| 用户感知 | 高层建图 API | 中层到核心层系统 | 后端内部执行器 |
| Pass 抽象 | Raster / Compute / Unsafe | Pass + ParameterStruct + PassFlags | 已记录命令 / list |
| 资源声明 | Builder 显式调用 | ParameterStruct 反射 + Create/Register API | `ResourceTracker + ResourceUsage` |
| 依赖建立时机 | 录制后编译构图 | 编译期统一推导 | 记录命令时增量建立 |
| barrier 粒度 | 内部处理 | 极强、系统化 | usage 级别直接映射 |
| culling | 有 | 有，且 root 定义成熟 | 本质上重排，不是高层 pass culling |
| pass merge | 有 native pass merge | 有 render pass merge 控制 | 更偏命令聚批，不是同类概念 |
| async compute | 有入口 | 最完整 | 以依赖和阶段管理为主 |
| transient/aliasing | 有 | 最成熟 | 重点不在统一 transient 框架 |
| validation | 有，但不如 UE 系统化 | 最强 | 以后端正确性为主，用户级强验证较少 |

### 7.2 依赖模型对比

#### Unity

- 开发者显式告诉图“我读写了什么”
- 图系统再编译出 native pass 和资源计划

#### UE5

- 开发者主要通过参数结构和 pass flags 声明
- 系统根据元数据自动推导

#### Godot

- 开发者实际上已经在录制具体命令
- 图系统做的是 hazard 分析和调度优化

### 7.3 哪家最值得参考什么

#### 参考 Unity 的部分

- Pass builder 体验
- attachment 语义建模
- global texture 传播
- `UnsafePass` 这种有边界的逃生口

#### 参考 UE5 的部分

- 资源状态位设计
- external / extract / final access 模型
- cull root 定义
- async compute interval
- validation 框架
- transient allocator + pooling + aliasing overlap

#### 参考 Godot 的部分

- 记录期增量 hazard 建图
- usage 到 barrier/layout/access 的直接映射
- texture slice / dirty rect 跟踪
- 命令重排
- barrier grouping
- secondary command buffer 执行器

---

## 8. 对 RadRay 的设计启发

### 8.1 不建议只抄一家的现成形态

因为三家的层级不同：

- Unity 偏 API
- UE 偏内核
- Godot 偏执行器

如果 RadRay 直接只学 Unity，容易得到“好用但后端不够强”的系统。

如果只学 UE5，容易在早期把系统做得过重。

如果只学 Godot，则可能只有低层调度器，没有足够好的上层 frame graph 体验。

### 8.2 更合理的 RadRay 目标形态：双层结构

建议 RadRay 最终做成两层：

#### 第一层：用户侧 Frame Graph API

职责：

- 提供 typed pass builder
- 声明 texture/buffer/attachment 读写
- 支持 import / create / transient / extract
- 支持 compute / raster / copy / present / unsafe 等 pass 类型

这层可主要参考 Unity 的 API 风格，并吸收 UE5 的部分约束设计。

#### 第二层：后端 Execution Graph / Hazard Graph

职责：

- 根据资源访问和 pass 类型生成依赖边
- 推导 barrier 和 resource lifetime
- 做命令分组、重排、合并
- 负责 secondary command buffer / async compute / queue sync

这层可主要参考 UE5 的资源状态模型和 Godot 的命令执行器。

### 8.3 建议优先落地的核心能力

#### 阶段 1：先建立“可用”的核心图

优先做：

1. 统一资源句柄系统
2. `import/create/transient/extract`
3. pass 级读写声明
4. 简单 pass culling
5. 生命周期求解
6. 最基本的 barrier 编译

这阶段可以先不做：

- 多队列 async compute
- secondary command buffer 并行录制
- 高级 transient aliasing overlap 优化

#### 阶段 2：补足 raster 语义

建议加入：

- color/depth attachment 显式建模
- input attachment / resolve / shading rate 之类的扩展挂点
- render pass merge 条件判断

这部分应重点参考 Unity。

#### 阶段 3：补强资源状态系统

建议加入：

- resource external / extracted / produced / transient 状态位
- final access 约束
- external access mode / internal access mode
- 更严格的 validation

这部分应重点参考 UE5。

#### 阶段 4：补强后端执行器

建议加入：

- 命令级重排
- barrier grouping
- secondary command buffer 录制任务
- queue fork / join

这部分应重点参考 Godot 和 UE5。

### 8.4 句柄和资源状态的建议最小集合

对于 RadRay，建议从一开始就给资源对象保留类似 UE5 的基础状态位：

- `external`
- `extracted`
- `produced`
- `transient`
- `force_non_transient`
- `first_use_pass`
- `last_use_pass`
- `last_producer`
- `final_access`

即便初版不全部使用，也建议预留字段和语义位，否则后面加 extraction / async compute / aliasing 会非常痛苦。

### 8.5 Pass 类型建议

建议至少有：

- `Raster`
- `Compute`
- `Copy`
- `Present`
- `Unsafe`

如果未来准备上多队列，还建议 pass flag 至少预留：

- `AsyncCompute`
- `NeverCull`
- `NeverMerge`
- `NeverParallel`

这部分非常适合直接借鉴 UE5 的 `ERDGPassFlags` 设计。

### 8.6 资源使用语义建议

如果 RadRay 想兼顾高层易用性和低层正确性，建议做“两段式语义”：

#### 用户侧

- `ReadTexture`
- `WriteTexture`
- `ReadWriteTexture`
- `ReadBuffer`
- `WriteBuffer`
- `ReadWriteBuffer`
- `SetColorAttachment`
- `SetDepthAttachment`
- `SetResolveTarget`

#### 编译器内部

再把这些高层语义降解为类似 Godot / UE 的底层 usage / access / layout：

- sample
- color attachment
- depth attachment
- storage read/write
- copy src/dst
- resolve src/dst
- indirect
- vertex
- index

这样用户侧 API 不会太硬核，后端也不会失去 barrier 精度。

### 8.7 Validation 必须尽早做

这是三家里最容易被低估但最值钱的部分。

建议 RadRay 尽早加入以下验证：

1. 同一资源在同一 pass 内的非法读写组合
2. attachment 和普通 sampled/storage 使用的非法混用
3. 没有产出但又希望保留副作用的 pass 是否标记 `NeverCull`
4. extracted 资源是否设置了合理 final access
5. unsafe pass 是否绕过了关键生命周期约束

如果这部分等到系统很大再补，调试成本会非常高。

### 8.8 最终建议结论

如果只给 RadRay 一个最实际的路线建议，那就是：

- **API 形态先学 Unity**
- **核心状态机和资源模型先学 UE5**
- **执行器和命令图优化再学 Godot**

不要把 render graph 只理解成“记录一个 DAG”。真正有价值的部分是：

- 资源生命周期求解
- barrier 编译
- 外部资源边界管理
- culling / merge / reorder
- validation
- 调试可视化

而这几部分里，UE5 和 Godot 提供的实现价值通常比表面 API 更高。

---

## 9. 系统各组件能力的重要性评估

这里的“重要性”不是指实现难度，也不是指短期 demo 里最容易看出效果的部分，而是指：

- 缺了这个能力，render graph 是否还成立
- 缺了这个能力，系统是否还能在真实引擎里稳定落地
- 缺了这个能力，后续优化能力是否会失去基础

按照这个标准，从最重要到相对次重要，可以排成下面这个顺序。

### 9.1 排序总览

| 排名 | 组件/能力 | 重要性判断 | 核心原因 |
| --- | --- | --- | --- |
| 1 | 资源访问声明模型 | 最高 | 它决定图系统有没有可靠输入 |
| 2 | 资源状态机、生命周期与 barrier 编译 | 最高 | 它决定系统是否正确 |
| 3 | 图编译、依赖闭包与 culling | 极高 | 它决定声明能否变成执行计划 |
| 4 | 外部资源边界管理 | 极高 | 它决定系统能否融入真实引擎 |
| 5 | Raster / Attachment / Pass 类型语义 | 很高 | 它决定系统能否承载主渲染路径 |
| 6 | Validation | 很高 | 它决定系统能否长期维护和扩展 |
| 7 | 执行调度器（merge / reorder / barrier grouping） | 高 | 它决定系统能否从“能跑”进化到“高效” |
| 8 | Transient、池化与 aliasing | 中高 | 它决定内存效率和资源复用水平 |
| 9 | Async Compute、多队列与并行录制 | 中高 | 它决定高端性能上限，但依赖前置系统成熟 |
| 10 | 工程辅助与易用性能力 | 中 | 它提升可用性，但不定义系统本体是否成立 |

### 9.2 逐项判断

1. **资源访问声明模型**

这是 render graph 的输入语言，优先级必须排第一。没有可靠的访问声明，后面的 lifetime、barrier、culling、merge、async compute 都没有可计算基础，系统最终只能退化成“帮你记一下 pass 顺序”的薄封装。

三家源码都把这一层放在最核心的位置：

- Unity 直接在 pass 上维护 `resourceReadLists`、`resourceWriteLists`、`transientResourceList`
- UE5 明确写明 barrier 和 lifetime 来自 pass parameter struct 中的 RDG 参数
- Godot 则把每条命令的 `ResourceUsage + ResourceTracker` 作为建图输入

这说明 render graph 真正的第一组件，不是 builder 外观，而是“系统拿到了怎样的依赖描述”。

2. **资源状态机、生命周期与 barrier 编译**

如果说第 1 项回答的是“图知道了什么”，第 2 项回答的就是“图能否把这些信息变成正确执行”。这是 render graph 的正确性骨架。

从源码上看，这一层也是 UE5 和 Godot 最有含金量的部分：

- UE5 资源对象上有 `bExternal`、`bExtracted`、`bTransient`、`AliasingOverlaps`、`LastProducer`
- Godot 直接把 `_usage_to_image_layout()`、`_usage_to_access_bits()`、`_is_write_usage()` 做成 barrier 求解基础
- Unity 虽然对用户隐藏了更多底层状态，但编译阶段也显式有 `FindResourceUsageRangeAndSynchronization()`

没有这部分，系统最多只能在非常简单的串行场景下“看起来能用”；一旦出现 UAV、copy、resolve、跨 pass attachment 切换或图内外资源混用，就会迅速失稳。

3. **图编译、依赖闭包与 culling**

render graph 的价值不在“记录”，而在“编译”。声明阶段只是把意图记下来，只有 compile 之后，系统才能得到真正的执行计划、资源使用区间、裁剪结果和同步边界。

Unity 的源码把这一点表达得非常直接：

- `ValidatePasses()`
- `BuildGraph()`
- `CullUnusedRenderGraphPasses()`
- `TryMergeNativePasses()`
- `FindResourceUsageRangeAndSynchronization()`
- `PrepareNativeRenderPasses()`

UE5 也在 `FRDGBuilder` 注释中明确写到：graph 会在 `Execute()` 中被 compile、cull、execute。缺少这层时，系统即便叫 render graph，实质上也更像一个“录制列表”。

4. **外部资源边界管理**

这是很多早期 render graph 设计最容易低估，但在真实引擎里又绝对绕不开的组件。

真实渲染系统里一定会存在大量图内外边界：

- swapchain/backbuffer
- 历史帧资源
- 上传下载缓冲
- 被图执行后继续交回别的系统使用的纹理/缓冲
- 外部导入的 shadow map、scene color、depth pyramid

UE5 把 `IsCullRoot()` 直接定义成 `bExternal || bExtracted`，已经很能说明问题：图的“根”并不只是最终屏幕输出，而是由图内外资源边界共同定义。没有 import / extract / final access / external access mode 这类能力，render graph 很难成为引擎中枢。

5. **Raster / Attachment / Pass 类型语义**

如果系统只会表达“某个资源被读/写”，那它还只是一个泛化依赖图；要真正承载主渲染路径，还必须把 raster 语义做成一等能力。

这部分之所以重要，是因为图形渲染并不是纯 compute world：

- color/depth attachment
- input attachment
- resolve
- copy
- present
- render pass merge 边界

Unity builder 对 attachment 的支持是最直接的证据，而 UE5 的 `ERDGPassFlags` 也把 `Raster`、`Compute`、`AsyncCompute`、`Copy` 等类型显式编码进 pass。少了这层，系统会迫使大量核心渲染路径退回 `UnsafePass` 或手写后端。

6. **Validation**

从“系统能工作”到“系统能长期演进”，中间最关键的跃迁能力就是 validation。

这部分的重要性经常被低估，因为它在 demo 阶段看不出收益，但在工程规模上升后会变成最值钱的护栏。源码层面也能看出三家都没有忽略它：

- Unity 编译前先 `ValidatePasses()`
- UE5 单独有 RenderGraphValidation 体系
- Godot 虽然更偏后端，但也在命令依赖建立和 tracker 使用处保留了大量正确性检查

如果目标只是做一个短期实验系统，validation 可以暂时晚一点；但如果目标是 RadRay 的长期 runtime 中枢，它的真实优先级其实可以和第 5 项非常接近。

7. **执行调度器：merge / reorder / barrier grouping / native pass prepare**

这部分是系统从“正确”走向“高效”的第一道门槛。

前 1 到 6 项更偏“让系统成立并且可靠”，而第 7 项开始更强调“让系统值得用”。三家在这方面的差异也很鲜明：

- Unity 重点做 `TryMergeNativePasses()` 和 `PrepareNativeRenderPasses()`
- UE5 通过 pass flag、barrier collect、render pass merge 控制执行形态
- Godot 则在 `end(...)` 中做 reorder、level 划分、barrier grouping 和实际命令运行

没有这一层，系统仍可能是“正确”的，但往往性能不够稳定，也无法把 render pass / barrier / command list 压缩到足够好的形态。

8. **Transient、池化与 aliasing**

这部分对成熟系统非常重要，但它是建立在生命周期已经可求解的基础之上的。

从源码看：

- Unity 有 `CreateTransientTexture`、`CreateTransientBuffer` 和资源池
- UE5 有 `bTransient`、`bForceNonTransient`、`AliasingOverlaps`

它们都说明成熟 render graph 不只是“知道资源什么时候用”，还会进一步利用这些区间去做复用和别名优化。但如果前面的 lifetime、external/extract、validation 还没站稳，这部分做得再早也容易返工。

9. **Async Compute、多队列与并行录制**

这类能力很强，但不是 render graph 的起点，而是高阶扩展。

原因很简单：多队列不是一个独立组件，它建立在前面很多能力之上：

- pass 类型和依赖闭包必须准确
- barrier 和资源状态必须能跨 pipeline 推导
- fork / join 语义必须清晰
- 图内外边界必须受控

UE5 把 async compute interval 做得最完整，Godot 也把 secondary command buffer 任务化做进了执行器，但二者都不是“先做出来再补核心”的路线，而是等核心模型成熟后再上。

10. **工程辅助与易用性能力**

这一组能力包括但不限于：

- Blackboard
- global texture 传播
- renderer list 集成
- labels / events / profiling tag
- `UnsafePass` / callback 这类 escape hatch

它们很重要，但重要性更多体现在“工程可用性”和“接入成本”，不是“定义 render graph 是否成立”的硬骨架。换句话说，它们会显著影响用户体验，却不应排在状态机、compile、external boundary 这些真正决定系统下限的能力之前。

### 9.3 一个更实用的分层结论

如果把这 10 项再压缩成更适合工程决策的三层，可以得到下面这个判断：

- **第 1 到 4 项**：定义“这是不是一个真正可用的 render graph 内核”
- **第 5 到 7 项**：定义“它能不能优雅承载主渲染路径，并且开始产生明显性能收益”
- **第 8 到 10 项**：定义“它是否已经进入成熟、好用、可持续优化的阶段”

也就是说，对 RadRay 来说，最需要先守住的不是 UI 层 API 花样，而是前四项：

1. 可靠依赖声明
2. 生命周期和 barrier 编译
3. compile/cull
4. import/extract/final access 这类边界能力

只要这四项站稳，系统就已经不是“一个概念上的 DAG”，而是一套真正能控制资源正确性和执行边界的 render graph 中核。

---

## 10. 最后的结论

从源码实现角度看，这三家并不是三种“同构 render graph”：

- Unity 是 **高层 frame graph**
- UE5 是 **全局资源依赖与执行编译内核**
- Godot 是 **低层 command/resource hazard graph**

对 RadRay 而言，最佳路径不是选一家站队，而是组合吸收：

- 用 Unity 的方式定义“开发者怎么写 pass”
- 用 UE5 的方式定义“系统怎么推导资源状态和执行计划”
- 用 Godot 的方式定义“后端怎么把命令真正高效地跑起来”

这会比只模仿某一家的表层 API，更接近一套真正可扩展、可维护、可优化的 render graph 架构。

