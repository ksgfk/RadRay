# 实时渲染系统 MVP 架构设计

## 1. 目标

本文档用于规划一套从零开始搭建的最简易且可用的实时渲染系统。目标不是一次性覆盖所有现代渲染特性，而是先建立一个职责清晰、线程边界稳定、后续可扩展到 Render Graph 与跨帧资源管理的基础系统。

MVP 版本只解决以下问题：

- 逻辑层与渲染层完全解耦
- 逻辑线程只发布只读的帧快照，不直接发出渲染指令
- 渲染线程基于帧快照构建并执行最小 Render Graph
- 支持一条稳定可运行的实时渲染链路

MVP 版本明确不追求以下能力：

- Async Compute
- 多队列调度
- GPU Driven 渲染
- 复杂材质图和通用 shader graph
- Render Graph 可视化编辑器
- 自动 Pass 合并
- 完整 Temporal 体系

## 2. 分层

系统固定为以下五层：

### 2.1 Simulation

负责输入、游戏逻辑、ECS、动画、场景状态维护、相机与灯光更新。

约束：

- 不出现任何 RHI 类型
- 不出现任何 CommandBuffer、Texture、Buffer 等 GPU 对象
- 不关心本帧最终如何绘制

### 2.2 Extract

负责从 Simulation 的世界状态中提取一份只读 `FrameSnapshot`。

职责：

- 收集本帧渲染所需的场景输入
- 将逻辑对象转换为渲染友好的轻量结构
- 建立逻辑层与渲染层的正式同步点

约束：

- 不做 RHI 调用
- 不做 barrier 处理
- 不做图编译
- 不直接生成底层 draw command

### 2.3 Render Pipeline

负责根据 `FrameSnapshot` 决定本帧需要哪些渲染 Pass。

职责：

- 组织渲染流程
- 决定使用哪些 transient / persistent 资源
- 为 Render Graph 填充 pass 和资源访问关系

约束：

- 不直接分配底层物理资源
- 不手写 barrier
- 不直接 submit 到 GPU

### 2.4 Render Graph

负责描述并编译单帧 GPU 工作。

职责：

- 管理 pass 与资源依赖
- 拓扑排序
- 资源生命周期分析
- transient 资源分配
- barrier 推导
- 生成执行计划

### 2.5 RHI Backend

负责将 Render Graph 编译结果录制到底层命令缓冲并提交。

职责：

- 录制 CommandBuffer
- 提交到 CommandQueue
- Present
- 基于 GPU 完成情况回收 frame-local 资源

## 3. 线程模型

MVP 采用双线程模型：

- `Logic Thread`
- `Render Thread`

原则如下：

- 逻辑线程只写世界状态和 `FrameSnapshot`
- 渲染线程只读 `FrameSnapshot`
- 渲染线程不回头访问 ECS 或逻辑对象
- 两线程之间通过固定大小的 `FrameSnapshotQueue` 交接

MVP 不单独拆分 RHI 线程。先把 `Logic -> Extract -> RenderGraph -> Submit` 这条链路做稳定，再评估是否需要额外线程。

## 4. 核心数据模型

### 4.1 World 数据

这些结构属于逻辑世界，禁止跨线程直接引用：

- `CameraState`
- `TransformState`
- `MeshInstanceState`
- `LightState`
- `MaterialInstanceState`

### 4.2 FrameSnapshot

`FrameSnapshot` 是逻辑线程发布给渲染线程的只读帧数据。

建议包含：

```cpp
struct FrameSnapshot {
    uint64_t FrameId;
    CameraRenderData MainCamera;
    span<const VisibleMeshBatch> Meshes;
    span<const LightRenderData> Lights;
    span<const UploadRequest> Uploads;
    span<const RenderViewRequest> Views;
};
```

设计约束：

- 完全只读
- 不持有 ECS 指针
- 不持有逻辑对象裸指针
- 不依赖逻辑线程后续继续存活的数据
- 可以跨线程安全传递

### 4.3 RenderFrameContext

每个渲染帧在渲染线程上有一个 `RenderFrameContext`：

```cpp
struct RenderFrameContext {
    uint32_t FrameIndex;
    uint32_t BackBufferIndex;
    const FrameSnapshot* Snapshot;
    FrameLinearAllocator CPUArena;
    UploadArena Uploads;
    DescriptorArena Descriptors;
};
```

职责：

- 保存当前帧渲染执行上下文
- 提供 frame-local 临时内存
- 提供上传缓冲分配能力
- 提供描述符分配能力

### 4.4 Render Graph Handle

Render Pipeline 层不直接操作真实 GPU 资源，只操作逻辑句柄：

```cpp
struct ResourceHandle {
    uint32_t Id;
};

struct PassHandle {
    uint32_t Id;
};
```

## 5. 资源模型

MVP 先支持三类资源。

### 5.1 External

外部导入资源，Render Graph 不拥有其生命周期。

典型例子：

- SwapChain BackBuffer
- 外部深度缓冲
- 资源系统持有的静态纹理和静态 Buffer

### 5.2 Transient

只在当前帧的 Render Graph 内有效。

典型例子：

- HDR Color
- Bloom Chain
- 临时 Compute 输出
- 中间后处理纹理

这些资源由 Render Graph 编译阶段统一分配和回收。

### 5.3 Persistent

跨帧存活，但由渲染系统管理。

典型例子：

- TAA History
- Exposure
- SSR History
- 阴影缓存

即使 MVP 第一阶段暂时不用 Temporal，也必须提前为 `Persistent` 留出正式资源类型，避免未来推翻资源系统。

## 6. Pass 模型

每个 Pass 只做两件事：

- 声明访问哪些资源
- 提供执行函数

建议的最小接口：

```cpp
enum class PassType {
    Raster,
    Compute,
    Copy,
};

enum class ResourceAccess {
    Read,
    Write,
    ReadWrite,
    ColorAttachment,
    DepthAttachment,
    Present,
};

struct PassBuilder {
    ResourceHandle ReadTexture(ResourceHandle handle);
    ResourceHandle WriteTexture(ResourceHandle handle);
    ResourceHandle ReadBuffer(ResourceHandle handle);
    ResourceHandle WriteBuffer(ResourceHandle handle);
    void SetExecute(RenderExecuteFn fn);
};
```

约束：

- Pass 不直接 new 纹理和 Buffer
- Pass 不直接写 barrier
- Pass 不直接操作 SwapChain acquire / present

## 7. Render Graph Compiler 的最小职责

MVP 的 Graph Compiler 只做以下六件事：

1. 建立 pass 与资源访问依赖
2. 对 pass 做拓扑排序
3. 检查非法资源访问
4. 计算资源生命周期
5. 为 transient 资源分配实际 GPU 资源
6. 在 pass 之间插入必要 barrier

MVP 暂不实现：

- 资源 aliasing
- Pass 合并
- 多队列编译
- Async Compute 调度

这些能力可以在系统稳定后逐步加入。

## 8. 单帧时序

### 8.1 Logic Thread

每帧流程：

1. 更新 ECS、动画、相机、灯光、场景状态
2. 执行 Extract，构造 `FrameSnapshot`
3. 将 `FrameSnapshot` 发布到 `FrameSnapshotQueue`
4. 开始下一帧逻辑更新

### 8.2 Render Thread

每帧流程：

1. 等待或获取可消费的 `FrameSnapshot`
2. 初始化 `RenderFrameContext`
3. 导入 external 资源
4. 调用 Render Pipeline 构建 `RenderGraph`
5. 编译 `RenderGraph`
6. 执行图并录制命令
7. Submit 并 Present
8. 等待 GPU retire，回收 frame-local 资源

关键原则：

- 渲染线程永远只读快照
- 快照一旦发布即不可修改
- 世界状态和渲染状态之间只通过快照同步

## 9. Scene 到 Render 的提取策略

建议明确拆成三段：

### 9.1 Gather

从 ECS 或场景系统收集候选对象：

- Camera
- Mesh Renderer
- Light

### 9.2 Extract

将逻辑对象转成渲染友好结构：

- 世界矩阵
- Mesh Handle
- Material Handle
- Bounding Volume
- Sort Key

### 9.3 Prepare

在渲染线程上做渲染准备：

- 视锥裁剪
- 排序
- Batch 构建
- Instance 合并

原则：

- Logic 线程只负责抽取事实
- Render 线程负责决定如何绘制

## 10. MVP 推荐渲染链

为了尽快得到稳定画面，MVP 推荐先做 Forward 路线，而不是 Deferred。

推荐链路如下：

1. `UploadPass`
2. `MainScenePass`
3. `PostProcessPass`
4. `PresentPass`

如果希望再简单一些，可以先只做：

1. `UploadPass`
2. `MainScenePass`
3. `PresentPass`

待 Graph、资源和线程边界稳定后，再加入：

- Depth Prepass
- Shadow Pass
- Bloom
- TAA

## 11. 最小模块划分建议

建议新增或规划以下模块边界：

- `modules/render/runtime`
- `modules/render_graph`
- `modules/render_pipeline`
- `modules/render_scene`
- `modules/rhi`

其中 `modules/render/runtime` 负责放置运行时架构层面的核心类型与文档。

建议未来放入的文件包括：

- `frame_snapshot.h`
- `render_frame_context.h`
- `render_runtime_types.h`
- `frame_snapshot_queue.h`
- `transient_resource_registry.h`
- `persistent_resource_registry.h`

## 12. 最小接口草案

### 12.1 FrameSnapshotQueue

```cpp
class FrameSnapshotQueue {
public:
    bool Publish(FrameSnapshot&& snapshot);
    const FrameSnapshot* AcquireForRender();
    void ReleaseRendered(uint64_t frameId);
};
```

设计要求：

- 固定大小 ring buffer
- 逻辑线程发布完整快照后才允许可见
- 渲染线程只读取完整快照
- 可以丢弃旧快照，但不能读取半成品

### 12.2 Render Pipeline 入口

```cpp
class IRenderPipeline {
public:
    virtual void Build(RenderGraph& graph, const FrameSnapshot& snapshot) = 0;
};
```

### 12.3 Render Graph 入口

```cpp
class RenderGraph {
public:
    ResourceHandle ImportTexture(...);
    ResourceHandle CreateTexture(...);
    PassHandle AddRasterPass(string_view name, PassSetupFn setup);
    PassHandle AddComputePass(string_view name, PassSetupFn setup);
    void Compile();
    void Execute(RenderFrameContext& ctx);
};
```

### 12.4 Pass 执行上下文

```cpp
class RasterPassContext {
public:
    CommandBuffer* Cmd();
    TextureView* GetTextureView(ResourceHandle handle);
    Buffer* GetBuffer(ResourceHandle handle);
};
```

约束：

- `Cmd()` 仅在 Pass Execute 阶段可见
- 不允许向上层长期持有底层命令缓冲

## 13. 同步与丢帧策略

MVP 的帧交接策略建议如下：

- `FrameSnapshotQueue` 使用 2 或 3 个槽位
- 逻辑线程写完整快照后原子发布
- 渲染线程总是消费最后一个完整可用快照
- 如果渲染落后，可丢弃旧快照

必须保证的三条约束：

1. World State 可以掉帧
2. Published Snapshot 不能部分可见
3. RenderFrameContext 与 GPU Inflight Frame 生命周期分离

## 14. 开发顺序

建议按以下阶段推进，避免过早把系统做复杂。

### Phase 1: 单线程闭环

实现：

- `FrameSnapshot`
- 最小 `RenderGraph`
- `MainScenePass`
- `PresentPass`

目标：

- 先验证数据模型和图 API
- 先让一条最小渲染链画出结果

### Phase 2: 双线程

实现：

- `FrameSnapshotQueue`
- Logic / Render 分离
- Frame-local allocator
- Upload arena

目标：

- 验证线程交接与帧调度

### Phase 3: 完整资源生命周期

实现：

- Transient Resource Registry
- Persistent Resource Registry
- Barrier Generation
- Frame Retire 回收

目标：

- 验证资源生命周期、跨帧资源与回收策略

### Phase 4: 逐步现代化

按需加入：

- Culling
- Sort Key
- Instancing / Batching
- Shadow
- Bloom
- TAA
- Temporal 资源

## 15. 明确延后的内容

为防止过度设计，以下能力延后处理：

- 通用材质节点系统
- 全自动 PSO 派生框架
- Render Graph 反射 DSL
- Pass 热插拔编辑器
- 跨 Queue 调度器
- Bindless 资源体系
- ECS 与渲染系统双向自动同步

## 16. 结论

这套 MVP 架构的核心原则是：

- Logic 只发布渲染事实，不直接发出渲染命令
- Render 只消费只读快照，不直接读取逻辑世界
- Pipeline 负责组织帧结构
- Render Graph 负责资源依赖与执行规划
- RHI 只负责执行，不承载高层调度逻辑

只要这五层边界保持稳定，后续无论增加 Deferred、Temporal、Async Compute 还是 GPU Driven，都可以在不推翻系统主骨架的前提下逐步演进。
