# RadRay Render Graph v1 实现设计

更新日期：2026-03-25

本文档基于当前公开头文件 [include/radray/runtime/render_graph.h](include/radray/runtime/render_graph.h)、公开 API 草案 [render-graph-public-api-v1.md](render-graph-public-api-v1.md) 和 shader 绑定草案 [render-graph-shader-binding-v1.md](render-graph-shader-binding-v1.md)，给出一份面向实现的详细设计。

这份文档的目标不是再次讨论“API 应该长什么样”，而是回答下面这些实现问题：

- 公开 API 录入的 pass、资源和边界信息，在内部如何存储
- compile 阶段需要经过哪些子阶段，产出哪些中间结果
- 如何把 `UseTexture / UseBuffer / attachment / copy / shader bind` 统一收敛成一套 graph 资源同步计划
- 如何在不依赖 render 公共 barrier API 的前提下，直接按后端特化 lowering 到 D3D12 enhanced barriers、Vulkan barrier2 等实现
- 当前版本怎样闭合最小功能循环，哪些能力明确留到后续版本

---

## 1. 设计目标

### 1.1 主目标

v1 实现必须满足以下目标：

1. 支持 imported / transient / graph-owned 资源统一进入 render graph
2. 支持 raster、compute、copy、raster shader、compute shader 五类 pass 录入
3. 支持 compile 阶段建立依赖、做 pass culling、求资源生命周期、生成 barrier 计划
4. 支持 execute 阶段分配真实资源、录制命令、插入 barrier、收口 epilogue 访问语义
5. graph 内部保存比 runtime `render::TextureState` / `render::BufferState` 更丰富的语义信息
6. 最终 barrier lowering 直接按后端特化，不通过 render 层公共 barrier 描述绕一圈

### 1.2 次目标

v1 同时要为后续能力留足空间：

1. 更细的 subresource 精化
2. 更精确的 UAV 优化
3. render pass merge
4. transient aliasing
5. async compute / 多队列
6. graph introspection / viewer / profiling

### 1.3 非目标

v1 明确不追求下面这些能力一次到位：

1. 多队列调度
2. execution graph reorder
3. aggressive pass merge
4. aliasing overlap 最优化
5. DXIL / SPIR-V 指令级 UAV 读写分析
6. graph 外执行期 external access mode 切换

---

## 2. 当前公开 API 快照

当前实现必须以头文件为准，而不是以更早的概念草案为准。

### 2.1 资源与访问语义

当前公开接口提供了下面这些核心输入：

1. 资源句柄：`RGTextureHandle`、`RGBufferHandle`、`RGPassHandle`
2. stage：`RGStage`
3. texture access：`RGTextureAccess`
4. buffer access：`RGBufferAccess`
5. 纹理 view：`RGTextureBindingView`
6. buffer view：`RGBufferBindingView`
7. 通用 use desc：`RGTextureUseDesc`、`RGBufferUseDesc`

这意味着 graph 内部不应该再退化回“只读 / 只写”布尔模型，而应完整保留：

1. access bitset
2. stage bitset
3. view / range
4. pass 类型上下文

### 2.2 资源边界

当前边界接口分成两层：

1. imported 初始状态沿用 runtime 状态模型
2. final access 使用 graph 自己的 use desc 模型

具体体现在：

1. `RGImportedTextureDesc::InitialState` / `RGImportedBufferDesc::InitialState` 仍使用 `render::TextureState` / `render::BufferState`
2. `ExtractTexture` / `ExtractBuffer` 返回 `GpuTextureHandle` / `GpuBufferHandle`
3. `ExtractTexture` / `ExtractBuffer` 的 final 参数已经是 `RGTextureUseDesc` / `RGBufferUseDesc`
4. `SetTextureAccessFinal` / `SetBufferAccessFinal` 也已经使用 `RGTextureUseDesc` / `RGBufferUseDesc`

这说明：

1. graph 外进入 graph 的状态边界仍直接和 runtime 对齐
2. graph 内部以及 epilogue 收口已经开始使用 graph 自己的 richer 语义

### 2.3 Pass 形态

当前有五类公开 pass：

1. `AddRasterPass`
2. `AddComputePass`
3. `AddCopyPass`
4. `AddRasterShaderPass`
5. `AddComputeShaderPass`

当前没有单独公开 `UnsafePass`。

因此实现上不应围绕“开放逃生门”来搭建主路径，而应围绕：

1. typed pass builder
2. typed execute context
3. shader bind 自动 lower

来设计主线数据流。

---

## 3. 总体架构

建议把 render graph 实现拆成四层。

### 3.1 Layer 1: Public Recording Layer

职责：

1. 暴露 `RenderGraph`、各类 builder、各类 context 的公开接口
2. 在 setup 阶段记录资源、pass、binding、attachment、copy、extract、final access 等输入
3. 完成最基础的参数合法性检查

这一层不负责：

1. barrier 计算
2. 资源分配
3. pass 排序优化
4. 后端 barrier lowering

### 3.2 Layer 2: Graph Model Layer

职责：

1. 维护 graph 资源记录
2. 维护 pass 记录
3. 维护统一的 resource use / edge / dependency / lifetime 记录模型
4. 作为 compile 阶段所有分析的输入和输出载体

这是本实现的核心层。

### 3.3 Layer 3: Compiled Plan Layer

职责：

1. 维护 culling 后的 pass 顺序
2. 维护每个资源的物理生命周期
3. 维护每个 pass 前后的 barrier plan
4. 维护 descriptor/materialization 计划
5. 维护 epilogue / extraction 收口计划

这一层是 execute 阶段的直接输入。

### 3.4 Layer 4: Backend Lowering Layer

职责：

1. 接收 graph 资源同步计划或 compiled barrier plan
2. 直接按后端生成真实 barrier 命令
3. D3D12 走 enhanced barriers 路径
4. Vulkan 走 barrier2 路径

这一层不应先构造 render 公共 API 的 barrier 描述再反推出后端 barrier。

---

## 4. 核心内部数据结构

下面的结构不要求完全一字不差照抄，但建议整体形状保持一致。

### 4.1 资源记录

```cpp
enum class RGResourceKind : uint8_t {
    Texture,
    Buffer,
};

enum class RGResourceOrigin : uint8_t {
    Imported,
    GraphCreated,
    Transient,
};

struct RGResourceBaseRecord {
    uint32_t Id{0};
    RGResourceKind Kind{};
    RGResourceOrigin Origin{};
    string Name{};
    bool IsExtracted{false};
    bool ForceNonTransient{false};
    bool HasFinalUse{false};
    bool HasPhysicalAllocation{false};
    int32_t FirstUsePass{-1};
    int32_t LastUsePass{-1};
};

struct RGTextureRecord : RGResourceBaseRecord {
    render::TextureDescriptor Desc{};
    GpuTextureHandle ImportedHandle{};
    GpuTextureViewHandle ImportedDefaultView{};
    render::TextureState ImportedInitialState{render::TextureState::UNKNOWN};
    RGTextureBindingView ImportedInitialView{};

    RGTextureUseDesc FinalUse{};
    RGTextureBindingView FinalView{};

    GpuTextureHandle ExtractedHandle{};
    RGTextureBindingView ExtractedView{};
    RGTextureUseDesc ExtractedFinalUse{};
};

struct RGBufferRecord : RGResourceBaseRecord {
    render::BufferDescriptor Desc{};
    GpuBufferHandle ImportedHandle{};
    render::BufferState ImportedInitialState{render::BufferState::UNKNOWN};
    RGBufferBindingView ImportedInitialView{};

    RGBufferUseDesc FinalUse{};
    RGBufferBindingView FinalView{};

    GpuBufferHandle ExtractedHandle{};
    RGBufferBindingView ExtractedView{};
    RGBufferUseDesc ExtractedFinalUse{};
};
```

设计要点：

1. imported / created / transient 用统一资源表管理
2. texture / buffer 共享一套基类字段，但保留各自专有信息
3. final use 与 extracted final use 分开存，避免语义冲突
4. imported initial state 只作为 graph 起点输入，不作为内部主状态模型

### 4.2 Pass 记录

```cpp
enum class RGPassKind : uint8_t {
    Raster,
    Compute,
    Copy,
    RasterShader,
    ComputeShader,
};

struct RGPassResourceUseRef {
    RGResourceKind Kind{};
    uint32_t ResourceId{0};
    uint32_t UseIndex{0};
    bool IsWrite{false};
};

struct RGPassRecord {
    uint32_t Id{0};
    string Name{};
    RGPassKind Kind{};
    RGPassFlags Flags{};
    bool AllowCulling{true};
    bool IsCulled{false};
    bool HasSideEffect{false};
    bool HasExecute{false};

    render::RootSignature* RootSignature{nullptr};
    Nullable<const render::BindingLayout*> BindingLayout{nullptr};

    vector<RGPassResourceUseRef> ResourceUses{};
    vector<RGColorAttachmentDesc> ColorAttachments{};
    std::optional<RGDepthAttachmentDesc> DepthAttachment{};
    vector<RGResolveTargetDesc> ResolveTargets{};

    vector<RGCopyTextureDesc> CopyTextures{};
    vector<RGCopyBufferDesc> CopyBuffers{};
    vector<RGCopyBufferToTextureDesc> CopyBufferToTextures{};
    vector<RGCopyTextureToBufferDesc> CopyTextureToBuffers{};
    vector<RGResolveTextureDesc> ResolveTextures{};

    vector<RGTextureBinding> BoundTextures{};
    vector<RGBufferBinding> BoundBuffers{};
    vector<GpuSamplerHandle> BoundSamplers{};
};
```

设计要点：

1. pass 本身不直接保存 barrier，只保存“输入事实”
2. builder 录入的各种操作统一归档到 pass record
3. shader pass 的 `Bind(...)` 与普通 pass 的 `UseTexture(...)` / `UseBuffer(...)` 最终都要反映成 `ResourceUses`

### 4.3 统一资源访问记录

统一访问记录是整个实现设计里最重要的结构。

```cpp
struct RGTextureUseRecord {
    uint32_t PassId{0};
    uint32_t ResourceId{0};
    RGTextureUseDesc Desc{};
    bool IsImportedBoundary{false};
    bool IsExtractBoundary{false};
    bool IsFinalBoundary{false};
    bool IsAttachment{false};
    bool IsResolveTarget{false};
};

struct RGBufferUseRecord {
    uint32_t PassId{0};
    uint32_t ResourceId{0};
    RGBufferUseDesc Desc{};
    bool IsImportedBoundary{false};
    bool IsExtractBoundary{false};
    bool IsFinalBoundary{false};
};
```

设计要点：

1. graph 内一切 barrier 推导都应围绕 use record 展开
2. imported initial / extracted final / explicit final access 都进入同一模型
3. attachment、resolve target 不是额外的“特殊资源类型”，而是 use record 上的语义标签

### 4.4 Shader 绑定记录

```cpp
struct RGShaderBindingRecord {
    uint32_t PassId{0};
    render::BindingParameterId ParameterId{0};
    render::BindingParameterKind Kind{render::BindingParameterKind::UNKNOWN};
    render::ResourceBindType ResourceType{render::ResourceBindType::UNKNOWN};
    render::ShaderStages ShaderStages{};
    bool IsBindless{false};
    bool IsReadOnly{true};

    vector<RGTextureBinding> TextureBindings{};
    vector<RGBufferBinding> BufferBindings{};
    GpuSamplerHandle Sampler{};
    vector<std::byte> PushConstantData{};
};
```

设计要点：

1. shader binding 记录与 resource use 记录分层保存
2. compile 时由 `BindingLayout + ShaderBindingRecord` 自动 lower 成 use record
3. sampler / push constant 只影响执行绑定，不进入 resource dependency

### 4.5 Compiled Plan

```cpp
struct RGBarrierNode {
    RGResourceKind Kind{};
    uint32_t ResourceId{0};
    RGTextureUseDesc TextureBefore{};
    RGTextureUseDesc TextureAfter{};
    RGBufferUseDesc BufferBefore{};
    RGBufferUseDesc BufferAfter{};
    bool IsSubresource{false};
};

struct RGCompiledPass {
    uint32_t PassId{0};
    RGPassKind Kind{};
    vector<RGBarrierNode> PreBarriers{};
    vector<RGBarrierNode> PostBarriers{};
};

struct RGCompiledPlan {
    vector<uint32_t> PassOrder{};
    vector<RGCompiledPass> CompiledPasses{};
};
```

这里的 `RGBarrierNode` 只是示意。真正实现中建议拆成 texture / buffer 两类，并保留更明确的记录字段，而不是复用 union 风格结构。

---

## 5. Recording 阶段设计

Recording 阶段就是用户调用 builder 的阶段。它的目标不是求解，而是把输入事实以统一格式落到 graph 内部模型。

### 5.1 Handle 分配

建议使用单调递增 id：

1. `0` 保留为 invalid
2. texture / buffer / pass 各自独立 id 空间
3. handle 只保存 id，不保存指针

这样可以简化：

1. debug 验证
2. 序列化 / introspection
3. compile 前后记录分离

### 5.2 Create / Import

`CreateTexture` / `CreateBuffer`：

1. 创建 graph-owned 资源记录
2. 记录 descriptor 和 `ForceNonTransient`
3. 不做实际物理分配

`ImportTexture` / `ImportBuffer`：

1. 创建 imported 资源记录
2. 保存 imported runtime handle
3. 保存 initial state 和 initial view
4. 生成一条 imported boundary use record，作为 compile 起点

### 5.3 Builder 行为

`UseTexture` / `UseBuffer`：

1. 校验 handle 有效
2. 归一化 desc
3. 把 use record 追加到 pass record
4. 更新资源的 first/last use pass 候选

`SetColorAttachment` / `SetDepthAttachment` / `SetResolveTarget`：

1. 记录 attachment desc
2. 自动生成对应的 texture use record
3. 标记 attachment 语义
4. 将 load/store 信息保留到 pass record，不进入资源同步计划主体

`Copy*` / `ResolveTexture`：

1. 记录 copy/resolve desc
2. 生成对应的 source/destination use record
3. source 标记 read，destination 标记 write

### 5.4 Shader Bind 录入

`Bind(...)` 本身不直接等价于 `UseTexture(...)` / `UseBuffer(...)`，它应该先做两件事：

1. 通过 `BindingLayout` 找到参数元数据
2. 记录一条 shader binding record

之后在 compile 初期由专门的 lowering 步骤统一生成 use record。

这样分离的好处是：

1. `Bind(...)` 阶段只处理 schema 一致性
2. resource dependency 逻辑不会散在 builder 代码里
3. 后续如果加入 access hint，也只影响 lowering 逻辑

---

## 6. Compile 总流程

建议把 compile 固定拆成下面九步。

### 6.1 Step 1: Validation

目标：尽早失败。

最少要做：

1. handle 是否存在
2. pass 是否设置 execute
3. attachment 是否类型匹配
4. shader bind slot 是否存在、类型是否匹配
5. copy pass 中是否混入非法语义
6. extracted/final access 是否存在冲突

这一阶段不做全局排序，只做局部合法性和结构一致性。

### 6.2 Step 2: Shader Binding Lowering

目标：把 shader pass 的 `Bind(...)` 转成统一的 use record。

规则建议如下：

1. `CBuffer` -> `RGBufferAccess::ConstantRead`
2. `BufferSrv` -> `RGBufferAccess::ShaderRead`
3. `TextureSrv` -> `RGTextureAccess::SampledRead`
4. `BufferUav` -> 默认 `RGBufferAccess::ShaderRead | RGBufferAccess::ShaderWrite`
5. `TextureUav` -> 默认 `RGTextureAccess::StorageRead | RGTextureAccess::StorageWrite`
6. `AccelerationStructure` -> `RGBufferAccess::AccelerationStructureRead`
7. `Sampler` / `PushConstant` -> 不生成 use record

stage 来源：

1. 来自 `BindingParameterLayout::Stages`
2. 再和 pass kind 做一次合法性交叉验证

### 6.3 Step 3: Normalize Uses

目标：把普通 pass、shader pass、attachment、copy、resolve、boundary 统一成标准 use record 列表。

这一步要做：

1. 统一 view 默认值
2. 把 attachment use record 补齐 `ColorAttachmentWrite` / `DepthStencilWrite` / `ResolveWrite`
3. 把 copy source/destination 补齐成 `CopyRead` / `CopyWrite`
4. 把 imported initial 和 final use / extract use 也纳入统一列表

### 6.4 Step 4: Dependency Closure

目标：建立 pass 之间的资源依赖边。

建议先做最保守版本：

1. 同一资源的 use 按 pass 录入顺序处理
2. 前一个 writer -> 后续所有 reader / writer 建依赖
3. reader-reader 不强制依赖，除非 pass kind 或边界语义需要顺序
4. imported boundary 视作虚拟前驱
5. final use / extract 视作虚拟后继

这是 v1 最稳的做法，足以闭合最小功能循环。

### 6.5 Step 5: Pass Culling

目标：移除无可观察输出的 pass。

根节点定义建议：

1. extracted resource 的生产者
2. 有 explicit final use 的资源生产者
3. imported external output 的最终写者
4. `NeverCull` pass

做法：

1. 从根节点 pass 逆向遍历依赖图
2. 标记所有可达 pass
3. 未标记 pass 直接裁剪

### 6.6 Step 6: Lifetime Solve

目标：确定每个资源的逻辑生命周期区间。

对于每个资源：

1. `FirstUsePass = min(use.PassId)`
2. `LastUsePass = max(use.PassId)`
3. 如果存在 extract/final use，生命周期必须延伸到 epilogue

这一阶段的输出会直接喂给：

1. transient allocation
2. aliasing 预留
3. execute 阶段资源 materialization

### 6.7 Step 7: 资源同步计划生成

目标：在 graph 语义层面生成 pass 间同步计划。

核心原则：

1. 资源同步计划必须保留 graph 自己的 access / stage / range
2. 不要在这里就降成 `render::TextureState` / `render::BufferState`
3. 也不要先转成 render 公共 barrier 描述

对于每个资源 use 链：

1. 计算 `BeforeUse`
2. 计算 `AfterUse`
3. 如果发生写后读、写后写、读后写，生成 barrier node
4. 如果是 UAV self-dependency，也生成同态 barrier node

### 6.8 Step 8: Physical Resource Planning

目标：决定执行时真实资源如何获得。

v1 建议：

1. imported 资源直接引用外部 handle
2. graph-created / transient 资源在 execute 前统一创建
3. 暂不做复杂 aliasing
4. `ForceNonTransient` 直接禁止 aliasing 和 transient 回收优化

### 6.9 Step 9: Build Compiled Plan

目标：输出 execute 可直接消费的 plan。

内容包括：

1. culling 后 pass 顺序
2. 每个 pass 的 pre-barrier / post-barrier
3. 每个资源的 physical materialization plan
4. epilogue final use
5. extract 返回 handle 计划

---

## 7. 资源同步计划设计

### 7.1 为什么要单独做 Graph 资源同步计划

原因很直接：

1. 公开 API 已经提供了比 runtime state 更细的 access/stage/range
2. D3D12 enhanced barriers 和 Vulkan barrier2 都需要比“一个 before/after state”更细的输入
3. 如果 compile 时先把语义压成 render 公共 barrier API，再去后端还原，会无谓损失信息

所以建议 graph 资源同步计划至少保留下面这些字段。

### 7.2 Texture Synchronization Record

```cpp
struct RGTextureSyncRecord {
    uint32_t ResourceId{0};
    RGTextureBindingView View{};
    RGStages SrcStages{};
    RGStages DstStages{};
    RGTextureAccesses SrcAccess{};
    RGTextureAccesses DstAccess{};
    bool IsLayoutTransition{false};
    bool IsUavLikeDependency{false};
};
```

### 7.3 Buffer Synchronization Record

```cpp
struct RGBufferSyncRecord {
    uint32_t ResourceId{0};
    RGBufferBindingView View{};
    RGStages SrcStages{};
    RGStages DstStages{};
    RGBufferAccesses SrcAccess{};
    RGBufferAccesses DstAccess{};
    bool IsUavLikeDependency{false};
};
```

### 7.4 生成原则

1. 资源同步计划只表达 graph 语义，不表达后端枚举
2. 资源同步计划必须支持 whole resource 和 subresource 两种粒度
3. 资源同步计划必须支持 imported initial / final use / extract 边界
4. 资源同步计划不直接携带 load/store，load/store 属于 render pass encode 语义，不属于通用 barrier 语义

---

## 8. 后端特化 Lowering

这是当前设计最重要的实现边界之一。

### 8.1 总原则

`RG*SyncRecord` -> backend-specific barrier

不走：

`RG*SyncRecord` -> render 公共 barrier API -> backend barrier

### 8.2 D3D12 路径

建议专门实现：

1. `MapRGStageToD3D12Sync(...)`
2. `MapRGTextureAccessToD3D12Access(...)`
3. `MapRGTextureAccessToD3D12Layout(...)`
4. `MapRGBufferAccessToD3D12Access(...)`

然后直接填充 enhanced barrier 结构。

原因：

1. D3D12 enhanced barriers 本身就是 sync/access/layout 三维模型
2. `RGStage + RGAccess + View` 与 enhanced barrier 是天然匹配的

### 8.3 Vulkan 路径

建议专门实现：

1. `MapRGStageToVkPipelineStageFlags2(...)`
2. `MapRGTextureAccessToVkAccessFlags2(...)`
3. `MapRGTextureAccessToVkImageLayout(...)`
4. `MapRGBufferAccessToVkAccessFlags2(...)`

然后直接填充 `VkImageMemoryBarrier2` / `VkBufferMemoryBarrier2`。

### 8.4 Imported Boundary Lowering

对于 imported 资源：

1. 起点 before 来自 `render::TextureState` / `render::BufferState`
2. compile 前先把 imported initial state 转成 graph 内部模型里的 synthetic source use
3. 后续全部走 graph 语义

这样 imported 资源只是 graph use 链的一个起点，不会污染整个内部模型。

### 8.5 Final Use / Extract Lowering

对于 final use / extract：

1. `Set*AccessFinal` 生成 synthetic epilogue destination use
2. `Extract*` 也生成 synthetic epilogue destination use
3. 如果同一资源既 `Set*AccessFinal` 又 `Extract*`，必须校验两者是否兼容

---

## 9. Execute 阶段设计

### 9.1 总体流程

`Execute(GpuAsyncContext&)` 建议固定为：

1. 校验 graph 已 compile
2. materialize 真实资源
3. 准备 descriptor / view
4. 按 compiled pass 顺序执行
5. 对每个 pass 先插 pre-barrier
6. 录制 pass encode
7. 必要时插 post-barrier
8. 处理 epilogue final use / extract
9. 返回成功或 reason

### 9.2 真实资源 materialization

建议使用一个 execute-local 资源表：

```cpp
struct RGExecutionResources {
    vector<render::Texture*> Textures{};
    vector<render::Buffer*> Buffers{};
    vector<render::TextureView*> CachedTextureViews{};
};
```

规则：

1. imported 资源直接从 `Gpu*Handle` 解引用到底层对象
2. graph-created / transient 资源通过 `GpuAsyncContext` 创建
3. execute 结束后，未 extract 的 transient 资源跟随 context 生命周期回收

### 9.3 Pass Context

每个 typed context 只暴露：

1. 当前 encode 所需命令编码器
2. 资源句柄到真实对象的访问器
3. shader pass 的 root signature / descriptor set 访问器

不暴露：

1. graph internals
2. barrier plan
3. 其他 pass 的资源

### 9.4 Extraction 处理

`ExtractTexture` / `ExtractBuffer` 的处理建议：

1. execute 末尾确保资源已进入 extract final use
2. 返回的 `Gpu*Handle` 指向真实 runtime 资源
3. 对 graph-created 资源，这意味着它必须从本次 graph 生命周期中晋升为 graph 外可持有资源

这里要特别注意所有权：

1. graph-created 但被 extract 的资源，不应在 execute 结束时被当作纯 transient 释放
2. 未 extract 的 graph-created/transient 资源，仍然从属于 context 生命周期

---

## 10. Validation 细则

下面列出建议落地成 debug 校验的规则。

### 10.1 资源与 view

1. 所有 handle 必须有效
2. texture use 不能指向 buffer handle
3. buffer use 不能指向 texture handle
4. subresource range 不能越界
5. depth/stencil aspect 必须与 format 兼容

### 10.2 Pass 结构

1. raster pass 必须有 color 或 depth attachment
2. compute/copy pass 不得声明 attachment
3. copy pass 不得带 shader binding
4. shader pass 必须提供 root signature
5. 每个 pass 必须设置 execute

### 10.3 Shader Binding

1. slotName 必须能在 `BindingLayout` 中解析
2. 绑定类型必须匹配 `ResourceBindType`
3. sampler / push constant 不得绑定 graph 资源
4. bindless slot 如绑定 graph 资源，必须显式列出具体集合

### 10.4 边界一致性

1. imported 初始 state 可为 `UNKNOWN`，但不能是非法组合
2. `Set*AccessFinal` 的 view 必须与资源类型匹配
3. `Extract*` 的 final use 不能为空语义
4. 同一资源的 final use 与 extract final use 不得冲突

### 10.5 Execute 访问控制

1. pass 只能访问自身声明过的资源
2. shader pass 只能获取已绑定且已 lower 的资源
3. debug 下如果 execute 中访问未声明资源，立即报错

---

## 11. 最小可落地实现范围

为了尽快把 v1 跑起来，建议把实现拆成三层里程碑。

### 11.1 里程碑 A: 最小 compile/execute 闭环

能力：

1. import/create
2. raster/compute/copy pass
3. `UseTexture` / `UseBuffer`
4. attachment / copy / resolve
5. compile 依赖 + culling + lifetime
6. 单队列资源同步计划
7. execute + final use + extract

这一步可以暂不实现 shader pass 自动 lowering。

### 11.2 里程碑 B: Shader Pass 接入

能力：

1. `AddRasterShaderPass`
2. `AddComputeShaderPass`
3. `BindingLayout` 自动 lowering
4. sampler / push constant 排除
5. UAV 保守 `ReadWrite`

### 11.3 里程碑 C: 精化与优化

能力：

1. subresource 精化
2. better UAV hint
3. render pass merge 前置分析
4. transient aliasing 预留

---

## 12. 建议的代码组织

建议实现层至少拆成下面几组文件。

### 12.1 Public API

1. `render_graph.h`
2. `render_graph.inl` 或模板实现文件

### 12.2 Internal Model

1. `render_graph_internal.h`
2. `render_graph_records.h`
3. `render_graph_barrier_ir.h`

### 12.3 Compile

1. `render_graph_compile.cpp`
2. `render_graph_validate.cpp`
3. `render_graph_shader_lowering.cpp`
4. `render_graph_cull.cpp`
5. `render_graph_barriers.cpp`

### 12.4 Execute

1. `render_graph_execute.cpp`
2. `render_graph_contexts.cpp`
3. `render_graph_resources.cpp`

### 12.5 Backend Lowering

1. `render_graph_barrier_d3d12.cpp`
2. `render_graph_barrier_vk.cpp`

这样拆分的优点是：

1. compile 和 execute 职责清楚
2. validation 可单独测试
3. backend lowering 可以独立迭代

---

## 13. 测试策略

建议至少覆盖下面几组测试。

### 13.1 API/Validation

1. 无效 handle
2. 错误 attachment 类型
3. shader bind 类型不匹配
4. 重复 final use 冲突
5. extract 未给 final use

### 13.2 Compile

1. 单资源读写链依赖
2. 多 pass culling
3. imported resource 起点
4. extracted resource 根闭包
5. copy / resolve 依赖生成

### 13.3 Execute

1. backbuffer import -> raster write -> present 收口
2. compute 写 UAV -> copy readback
3. transient texture 生命周期
4. extracted history texture 跨 graph 持续存在

### 13.4 Backend Lowering

1. D3D12 enhanced barrier 字段映射正确
2. Vulkan barrier2 stage/access/layout 映射正确
3. subresource barrier 范围正确

---

## 14. 已知开放问题

当前设计还有几项需要实现时进一步定稿。

### 14.1 final use 与 extract 的优先级

需要明确：

1. 同一资源同时设置 final use 和 extract 时，是否允许
2. 如果允许，哪一个优先
3. 是否要求完全一致

建议 v1：

1. 允许同时存在
2. 但必须完全一致，否则 validation 失败

### 14.2 Attachment 内部读写冲突

当前 API 允许表达：

1. attachment write
2. sampled/storage read/write

但 v1 建议仍保守禁止同一纹理在同一 raster pass 内既是 attachment 又是 sampled/storage。

### 14.3 Buffer format / stride 对 barrier 的影响

buffer view 的 `Stride` / `Format` 当前主要影响 view 创建，不应影响 barrier 语义。

建议明确：

1. barrier 只关注 range / access / stage
2. format/stride 仅属于 view materialization

### 14.4 Present 的 stage/access 归类

当前 API 已有 `RGStage::Present` 和 `RGTextureAccess::Present`。

建议 v1 中：

1. 只把它作为 imported/final use 边界语义使用
2. 不允许普通 pass 内声明 present use

---

## 15. 最终结论

如果把整个实现设计压缩成一句话，那么结论是：

- **公开 API 只负责记录“用户意图”**
- **compile 负责把意图归一化为 graph 内部模型和资源同步计划**
- **execute 负责 materialize 资源、录制命令并按后端特化插 barrier**

更具体一点：

1. imported 初始状态继续沿用 runtime `render::TextureState` / `render::BufferState`
2. graph 内部统一收敛到 `RG*UseDesc + stage + view/range` 的 richer 内部模型
3. shader pass 通过 `BindingLayout` 自动 lower 成统一 use record
4. final access 和 extract 统一通过 use desc 进入 epilogue
5. backend barrier lowering 直接面向 D3D12 enhanced barriers / Vulkan barrier2，而不是先绕到 render 公共 barrier API

这条路径最接近当前公开接口，也最适合作为 RadRay render graph v1 的实现主线。