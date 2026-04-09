# RenderGraph 编译分析报告

## 〇、前置约定

- **所有 GPU 资源**均通过 `gpu_system.h` 中的 `GpuRuntime` 分配与管理：
  - 持久资源：`GpuRuntime::CreateBuffer / CreateTexture / CreateTextureView / CreateSampler`
  - 临时资源：`GpuAsyncContext::CreateTransientBuffer / CreateTransientTexture / CreateTransientTextureView`
  - 资源句柄：`GpuBufferHandle / GpuTextureHandle / GpuTextureViewHandle / GpuSamplerHandle`
  - 销毁：`GpuRuntime::DestroyResourceImmediate / DestroyResourceAfter`
- 编译阶段为 **纯 CPU 计算**，不访问 GPU，不创建真实资源；产出是一份可被执行器直接消费的指令计划。
- Barrier lowering 需要 **分后端** 处理（见第六章）。

---

## 一、现有图结构完善度检查

录制态 (recording-time) 图结构**已经相当完善**，覆盖了：

- 节点类型：`Buffer`、`Texture`、`GraphicsPass`、`ComputePass`、`CopyPass`
- 边类型：`ResourceDependencyEdge`（含 stage/access/layout/range 信息）、`PassDependencyEdge`
- Import/Export 语义、Ownership 模型（External / Internal / Transient）
- 完整的 Validate 实现（12 大类 80+ 条校验规则全部落地）
- Builder pattern 可自动建边

---

## 二、编译产物 (CompileResult) 应该包含什么

编译的目标是把录制态的高层、抽象的有向无环图，转化为**可直接驱动 GPU 执行的指令序列**。`CompileResult` 应包含以下核心数据：

### 2.1 线性执行序列 (Execution Timeline)

```
vector<CompiledPassEntry> ExecutionOrder;
```

每个 `CompiledPassEntry` 代表一个即将被执行的 Pass，包含：
- 指向原始 `RDGPassNode*` 的引用
- 该 Pass 执行前需要发射的所有 **Barrier**（以 RDG 中间表示存储，而非 RHI 表示）
- 该 Pass 需要的 **RenderPass descriptor**（对 Graphics Pass）
- 拓扑序序号

### 2.2 Barrier 指令表（RDG 中间表示）

编译阶段的 barrier 输出使用 **RDG 中间表示 (IR)**，而非直接输出 RHI `ResourceBarrierDescriptor`。
原因：D3D12 与 Vulkan 的 barrier 模型差异巨大，统一映射会丢失优化机会（详见第六章）。

```
struct CompiledBufferBarrier {
    RDGBufferHandle Buffer;
    RDGExecutionStages SrcStage;
    RDGMemoryAccesses SrcAccess;
    RDGExecutionStages DstStage;
    RDGMemoryAccesses DstAccess;
    render::BufferRange Range;
};

struct CompiledTextureBarrier {
    RDGTextureHandle Texture;
    RDGExecutionStages SrcStage;
    RDGMemoryAccesses SrcAccess;
    RDGTextureLayout SrcLayout;
    RDGExecutionStages DstStage;
    RDGMemoryAccesses DstAccess;
    RDGTextureLayout DstLayout;
    render::SubresourceRange Range;
};

struct CompiledBarrierBatch {
    vector<CompiledBufferBarrier> BufferBarriers;
    vector<CompiledTextureBarrier> TextureBarriers;
};
```

保留 RDG 层的 Stage/Access/Layout 信息，推迟到 **Barrier Lowering 阶段** 由后端翻译为具体 RHI 调用。

### 2.3 资源生命周期表 (Resource Lifetime)

```
struct ResourceLifetime {
    RDGResourceHandle Resource;
    uint32_t FirstUsePassIndex;   // 在 ExecutionOrder 中的首次使用位置
    uint32_t LastUsePassIndex;    // 在 ExecutionOrder 中的最后使用位置
};
vector<ResourceLifetime> Lifetimes;
```

用于：
- 确定 Internal 资源的创建/销毁时机
- Transient 资源的内存别名分析
- 执行阶段通过 `GpuRuntime::CreateBuffer`/`CreateTexture` 创建，或通过
  `GpuAsyncContext::CreateTransient*` 绑定到 context 生命周期

### 2.4 资源分配计划 (Allocation Plan)

```
struct AllocationPlan {
    struct InternalBufferAlloc {
        RDGBufferHandle Handle;
        render::BufferDescriptor Desc;
        int32_t AliasGroup;  // -1 = 不别名
    };
    struct InternalTextureAlloc {
        RDGTextureHandle Handle;
        render::TextureDescriptor Desc;
        int32_t AliasGroup;
    };
    vector<InternalBufferAlloc> Buffers;
    vector<InternalTextureAlloc> Textures;
};
```

执行阶段根据此计划通过 `GpuRuntime` 或 `GpuAsyncContext` 创建真实资源。

### 2.5 终结 Barrier

```
CompiledBarrierBatch EpilogueBarriers;  // Export 资源的终结状态转换
```

Import 资源的初始状态作为 tracker 的初始值。当第一个 Pass 使用该资源时会自然地从
import state 转换到所需状态，barrier 归入该 Pass 的 PreBarriers，无需单独的 Prologue batch。

### 2.6 （可选）异步队列调度信息

如果未来要支持多队列并行（Graphics + Compute + Copy），还需要：
```
struct QueueAssignment {
    uint32_t PassIndex;
    render::QueueType Queue;
    vector<SyncPoint> WaitBefore;  // 等待其他队列的 fence
    vector<SyncPoint> SignalAfter; // 向其他队列发信号
};
```

---

## 三、编译过程的详细阶段

### 阶段 1: 拓扑排序 (Topological Sort)

**输入**：原始有向图的所有节点和边

**算法**：Kahn 算法（BFS 拓扑排序），Validate 中已有实现

**输出**：所有节点的拓扑序 `vector<uint64_t> topoOrder`

**细节**：
- 在同入度条件下，可以引入启发式排序以优化 barrier 数量。常见策略：
  - **深度优先倾向**：优先执行与当前 Pass 共享更多资源的后继 Pass，减少中间状态切换
  - **写-读距离最小化**：将读取某资源的 Pass 尽可能紧接在写入该资源的 Pass 之后

### 阶段 2: 提取 Pass 执行序列 (Pass Linearization)

**输入**：拓扑序

**操作**：从拓扑序中过滤出所有 Pass 节点，保持相对顺序不变

**输出**：`vector<RDGPassNode*> passExecutionOrder`

Resource 节点不直接出现在执行序列中——它们是被 Pass "消费的"。

### 阶段 3: Dead Code 消除 (Dead Code Elimination)

**输入**：阶段 2 产出的 `passExecutionOrder`

**目标**：如果某些 pass 的输出资源没有被任何后续 pass 消费、也没有被 Export，可以跳过。
在 barrier 计算之前消除死节点，避免为无用 pass 和资源生成 barrier。

**算法**：从所有 Export 资源和有 side-effect 的 Pass 反向 BFS/DFS 标记所有 "活跃" 节点，
未被标记的 Pass 和资源从执行序列中移除。

```
活跃集合 = { 所有设置了 _exportState 的资源节点 }
worklist = 活跃集合

while worklist 非空:
  取出节点 N
  对 N 的所有入边 E:
    对端节点 = E.from
    if 对端节点 not in 活跃集合:
      活跃集合.insert(对端节点)
      worklist.push(对端节点)

从 passExecutionOrder 中移除不在活跃集合的 Pass
```

> **注意**：Validate 的 5.3/5.4 保证了每个 Pass 至少有写出和每个 Resource 至少被引用，
> 但并不保证所有 Pass 的输出最终被消费。DCE 能处理"中间 Pass 链的末端未被 Export"的情况。

### 阶段 4: 资源状态追踪 & Barrier 计算

这是编译最核心、最复杂的阶段。本阶段统一处理以下三件事：

1. **Import 资源的初始状态初始化**（原阶段 7 Prologue）
2. **按执行序列逐 Pass 推进状态、计算 Pre-Barrier**
3. **Export 资源的终结状态 Barrier**（原阶段 7 Epilogue）

Import 资源的初始状态不需要单独的 Prologue Barrier Batch——它作为 tracker 的初始值存在，
当第一个 Pass 使用该资源时会自然地从 import state 转换到所需状态，barrier 归入该 Pass 的 PreBarriers。

**关键决策：Barrier 在 RDG IR 层计算，不在编译阶段映射到 RHI。**

**不在此阶段做 RDG→RHI 映射的原因**：
1. D3D12 的 state-based 模型可以做 state promotion/decay 优化，需要看到 RDG 语义
2. Vulkan 需要同时推导 `VkAccessFlags` + `VkPipelineStageFlags` + `VkImageLayout`，三者耦合
3. D3D12 Enhanced Barriers 和 Legacy Barriers 有不同的最优映射策略
4. 保留 RDG IR 可以让后端在 lowering 阶段做更多合并

#### 4.1 数据结构

**Buffer 状态值**：

```
BufferStateValue = { Stage, Access }
```

**Buffer Tracker**——"默认整资源状态 + 局部覆盖切片"模型：

```
BufferTracker {
    DefaultState: BufferStateValue      // 整资源的默认状态
    Slices: vector<BufferStateSlice>    // 局部覆盖切片（按 Offset 升序）
}
BufferStateSlice {
    Range: BufferRange                  // 原始范围
    NormalizedRange: NormalizedBufferRange  // 归一化后的绝对 offset/size
    State: BufferStateValue             // 该切片的状态
}
```

此设计避免为全资源访问维护稠密表——大多数 buffer 整资源使用时，Slices 为空，仅用 DefaultState 描述。

**Texture 状态值**：

```
TextureStateValue = { Stage, Access, Layout }
```

**Texture Tracker**——"整资源退化 / 逐 subresource 稠密表"模型：

```
TextureTracker {
    UsePerSubresource: bool             // 是否启用逐 subresource 追踪
    ArrayCount: uint32                  // array layer 总数
    MipCount: uint32                    // mip level 总数
    WholeState: TextureStateValue       // 整资源状态（不启用细粒度时使用）
    Cells: vector<TextureStateValue>    // 大小 = ArrayCount * MipCount
}
// Cells 索引方式: Cells[layer * MipCount + mip]
```

当资源的 `depthOrArraySize > 0 && mipLevels > 0`（即编译时维度已知）时启用 per-subresource 追踪；
否则退化为整资源状态，用 `WholeState` 描述。

**归一化**：

所有 range 在使用前通过 `_NormalizeBufferRange` / `_NormalizeTextureRange` 归一化:
- 将 `Size == All` 展开为绝对值（若 totalSize 已知）
- 标记 `IsWholeResource` 用于整资源快速路径判定
- Texture 将 `ArrayLayerCount == All` / `MipLevelCount == All` 展开为绝对值

#### 4.2 步骤一：初始化 Tracker

```
为每个节点分配一个 BufferTracker 和一个 TextureTracker（按 nodeId 索引）。

遍历所有资源节点:
  若是 Buffer 节点:
    若 ownership == External:
      importState = { importState.Stage, importState.Access }
      normalizedImportRange = normalize(importState.Range, buffer.size)
      若 normalizedImportRange.IsWholeResource:
        tracker.DefaultState = importState
      否则:
        tracker.Slices.push_back({ importState.Range, normalizedImportRange, importState })
    否则: // Internal / Transient
      tracker.DefaultState = { NONE, NONE }  // 默认值

  若是 Texture 节点:
    若 depthOrArraySize > 0 && mipLevels > 0:
      tracker.UsePerSubresource = true
      tracker.ArrayCount = depthOrArraySize
      tracker.MipCount = mipLevels
      tracker.Cells.resize(ArrayCount * MipCount, { NONE, NONE, Undefined })
    若 ownership == External:
      importState = { importState.Stage, importState.Access, importState.Layout }
      若 tracker.UsePerSubresource:
        normalizedRange = normalize(importState.Range, arrayCount, mipCount)
        对 normalizedRange 覆盖的每个 (layer, mip):
          tracker.Cells[layer * MipCount + mip] = importState
      否则:
        tracker.WholeState = importState
```

> **要点**：Import 资源的初始状态直接写入 tracker，当第一个 Pass 使用时会自然产生从
> import state 到所需 state 的 barrier，无需单独的 Prologue 阶段。

#### 4.3 步骤二：逐 Pass 遍历，计算 Pre-Barrier

```
对每个 Pass P（按执行序列）:
  创建 BarrierBatch batch

  对 P 的每条 ResourceDependencyEdge（先 inEdges 后 outEdges）:
    requiredState = edge 上的 { stage, access [, layout] }
    resourceNode = edge 指向的资源节点

    分类处理:
```

##### 4.3.1 Buffer Barrier 计算

```
tracker = bufferTrackers[resourceNode.id]
requiredRange = normalize(edge.bufferRange, buffer.size)
requiredState = { edge.stage, edge.access }

// 收集所有与 requiredRange 有重叠的已有切片，按 Offset 升序排列
overlappingSlices = filter(tracker.Slices, 与 requiredRange 重叠)
sort(overlappingSlices, 按 NormalizedRange.Offset 升序)
```

**情况 A：整资源访问 (`requiredRange.IsWholeResource`)**

整资源访问语义上覆盖全部切片。处理完之后 tracker 退化为纯 DefaultState。

```
currentOffset = edge.bufferRange.Offset  // 通常为 0
totalSize = buffer.size  // 若已知

对每个 overlapping slice:
  sliceStart = slice.Offset
  sliceEnd = slice.Offset + slice.Size

  // 处理 slice 之前的空隙（由 DefaultState 覆盖的区域）
  若 currentOffset < sliceStart 且 DefaultState != requiredState:
    emit barrier { src=DefaultState, dst=requiredState, range=[currentOffset, sliceStart) }

  // 处理 slice 重叠区域
  overlapStart = max(currentOffset, sliceStart)
  若 overlapStart < sliceEnd 且 slice.State != requiredState:
    emit barrier { src=slice.State, dst=requiredState, range=[overlapStart, sliceEnd) }

  currentOffset = max(currentOffset, sliceEnd)

// 处理最后一个 slice 之后的尾部区域
若 DefaultState != requiredState:
  若 totalSize 已知:
    若 currentOffset < totalSize:
      emit barrier { src=DefaultState, dst=requiredState, range=[currentOffset, totalSize) }
  否则:
    emit barrier { src=DefaultState, dst=requiredState, range=[currentOffset, All) }

// 更新 tracker: 整资源统一为新状态
tracker.DefaultState = requiredState
tracker.Slices.clear()
```

**情况 B：子范围访问 (`!requiredRange.IsWholeResource`)**

仅影响 `[requestStart, requestEnd)` 区间，保留两侧旧切片状态。

```
requestStart = requiredRange.Offset
requestEnd = requiredRange.Offset + requiredRange.Size
currentOffset = requestStart

对每个 overlapping slice:
  overlapStart = max(requestStart, slice.Offset)
  overlapEnd = min(requestEnd, slice.Offset + slice.Size)
  若 overlapEnd <= overlapStart: continue

  // 处理 overlap 之前的空隙（DefaultState 覆盖）
  若 currentOffset < overlapStart 且 DefaultState != requiredState:
    emit barrier { src=DefaultState, dst=requiredState, range=[currentOffset, overlapStart) }

  // 处理 overlap 区域
  若 slice.State != requiredState:
    emit barrier { src=slice.State, dst=requiredState, range=[overlapStart, overlapEnd) }

  currentOffset = overlapEnd

// 处理尾部空隙
若 currentOffset < requestEnd 且 DefaultState != requiredState:
  emit barrier { src=DefaultState, dst=requiredState, range=[currentOffset, requestEnd) }

// 更新 tracker: 只改写访问区间
新建 nextSlices:
  对现有每个 slice:
    若与 [requestStart, requestEnd) 无交集 → 保留
    若有交集 → 保留左侧残余和右侧残余
  若 DefaultState != requiredState:
    插入新切片 { range=[requestStart, requestEnd), state=requiredState }

对 nextSlices 按 Offset 排序，相邻同态合并:
  若 nextSlices[i] 与 nextSlices[i+1] 状态相同且地址相邻 → 合并为一个

tracker.Slices = 合并后的 nextSlices
```

##### 4.3.2 Texture Barrier 计算

```
tracker = textureTrackers[resourceNode.id]
requiredState = { edge.stage, edge.access, edge.layout }
```

**情况 A：逐 subresource 追踪 (`tracker.UsePerSubresource`)**

```
requiredRange = normalize(edge.textureRange, arrayCount, mipCount)

对 requiredRange 覆盖的每个 (layer, mip):
  currentState = tracker.Cells[layer * MipCount + mip]
  若 currentState != requiredState:
    emit barrier {
      src = currentState,
      dst = requiredState,
      range = SubresourceRange{ layer, 1, mip, 1 }
    }
  tracker.Cells[layer * MipCount + mip] = requiredState
```

每个 subresource 独立发射 barrier，精度落到单个 (layer, mip)。
这避免了整张纹理被无谓串行化——例如 mip 0 正在被 pixel shader 读取的同时，
compute shader 可以写入 mip 1。

**情况 B：整资源退化 (`!tracker.UsePerSubresource`)**

```
若 tracker.WholeState != requiredState:
  emit barrier {
    src = tracker.WholeState,
    dst = requiredState,
    range = edge.textureRange  // 保留原始 range 传递给后端
  }
tracker.WholeState = requiredState
```

#### 4.4 步骤三：Export 资源的 Epilogue Barrier

所有 Pass 处理完毕后，检查设置了 `_exportState` 的资源，将 tracker 中的终态转换到 export state。

```
遍历所有资源节点:
  若 resourceNode._exportState 存在:

  若是 Buffer:
    exportState = { exportState.Stage, exportState.Access }
    exportRange = normalize(exportState.Range, buffer.size)

    // 与步骤二的 Buffer 处理逻辑完全一致:
    // 对 tracker 当前状态（DefaultState + Slices）与 exportState 做比较，
    // 有差异则 emit barrier 到 EpilogueBarriers batch。

  若是 Texture:
    exportState = { exportState.Stage, exportState.Access, exportState.Layout }

    若 tracker.UsePerSubresource:
      exportRange = normalize(exportState.Range, arrayCount, mipCount)
      对 exportRange 覆盖的每个 (layer, mip):
        currentState = tracker.Cells[layer * MipCount + mip]
        若 currentState != exportState:
          emit barrier { src=currentState, dst=exportState, range={ layer, 1, mip, 1 } }
    否则:
      若 tracker.WholeState != exportState:
        emit barrier { src=tracker.WholeState, dst=exportState, range=exportState.Range }
```

Epilogue barriers 收集到单独的 `EpilogueBarriers` batch 中，由执行器在所有 Pass 录制完毕后提交。

#### 4.5 状态比较规则

**Buffer**：`isSame(a, b) = (a.Stage == b.Stage && a.Access == b.Access)`

**Texture**：`isSame(a, b) = (a.Stage == b.Stage && a.Access == b.Access && a.Layout == b.Layout)`

状态完全相同时不产生 barrier。这自然实现了读-读合并：
同一资源被连续多个 Pass 以 **完全相同的** `(Stage, Access [, Layout])` 访问时，
第一次 barrier 之后状态已更新，后续 Pass 比较发现状态相同则跳过。

### 阶段 5: 资源生命周期分析

**输入**：Pass 执行序列 + 资源依赖边

**算法**：

```
对每个 Internal/Transient 资源 R：
  firstUse = min { passIndex(P) | P 有边连接到 R }
  lastUse  = max { passIndex(P) | P 有边连接到 R }
  lifetime[R] = [firstUse, lastUse]
```

**输出**：每个资源的 `[firstUse, lastUse]` 区间

**与 GpuRuntime 的交互**：
- Internal 资源在 `firstUse` 前通过 `GpuRuntime::CreateBuffer/CreateTexture` 创建
- Transient 资源通过 `GpuAsyncContext::CreateTransientBuffer/CreateTransientTexture` 创建，
  生命周期自动绑定到 context submission
- 销毁时机由执行器决定：可用 `DestroyResourceAfter(handle, task)` 延迟到 submission 完成后

### 阶段 6: 内存别名分析 (Memory Aliasing) — 可选/进阶

**目标**：生命周期不重叠的 Transient 资源可以共享同一块 GPU 内存

**算法**：区间图着色 / 贪心分配

```
按 firstUse 排序所有 Transient 资源
维护空闲内存池 = priority_queue<(lastUse, memoryBlock)>

对每个资源 R（按 firstUse 排序）：
  while 池顶 memoryBlock.lastUse < R.firstUse:
    回收到可用池
  if 可用池有尺寸兼容的 block:
    复用，R.aliasGroup = block.aliasGroup
  else:
    新分配，创建新 aliasGroup
```

这在 D3D12 中对应 **Placed Resource + Heap aliasing**，在 Vulkan 中对应 **VkDeviceMemory aliasing with VkImage/VkBuffer**。

**注意**：当前 `GpuRuntime` 暂未暴露 placed resource / memory aliasing 能力，
后续若需实现可在 `render::Device` 层添加 `CreatePlacedBuffer / CreatePlacedTexture` 接口。

### 阶段 7: 构建 RenderPass 描述 (Graphics Pass Only)

对每个 `RDGGraphicsPassNode`，从其 `_colorAttachments` 和 `_depthStencilAttachment` 构建底层的 `render::RenderPassDescriptor`：

```
对每个 GraphicsPass P：
  for each colorAttachment in P._colorAttachments:
    记录 { textureHandle, range, load, store, clearValue }
  if P._depthStencilAttachment:
    记录 { textureHandle, range, depthLoad/Store, stencilLoad/Store, clearValue }
  记录 CompiledRenderPassDesc
```

**注意**：TextureView 的创建需要在执行阶段完成（因为 Internal 资源在创建前不存在物理对象），
所以编译阶段只记录**描述**，不创建真实 View。执行阶段通过 `GpuRuntime::CreateTextureView`
或 `GpuAsyncContext::CreateTransientTextureView` 创建。

---

## 四、编译产物的完整数据结构建议

```cpp
class RenderGraph::CompileResult {
public:
    // ── RDG 中间表示 Barrier ──
    struct BufferBarrier {
        RDGBufferHandle Buffer;
        RDGExecutionStages SrcStage;
        RDGMemoryAccesses SrcAccess;
        RDGExecutionStages DstStage;
        RDGMemoryAccesses DstAccess;
        render::BufferRange Range;
    };

    struct TextureBarrier {
        RDGTextureHandle Texture;
        RDGExecutionStages SrcStage;
        RDGMemoryAccesses SrcAccess;
        RDGTextureLayout SrcLayout;
        RDGExecutionStages DstStage;
        RDGMemoryAccesses DstAccess;
        RDGTextureLayout DstLayout;
        render::SubresourceRange Range;
    };

    struct BarrierBatch {
        vector<BufferBarrier> BufferBarriers;
        vector<TextureBarrier> TextureBarriers;
    };

    // ── 编译后的 Pass ──
    struct CompiledPass {
        RDGPassNode* Node;
        uint32_t ExecutionIndex;
        BarrierBatch PreBarriers;  // 此 Pass 执行前的 barrier（RDG IR）
    };

    // ── 资源生命周期 ──
    struct ResourceLifetime {
        RDGResourceHandle Resource;
        uint32_t FirstPassIndex;
        uint32_t LastPassIndex;
    };

    // ── 资源分配计划 ──
    struct ResourceAllocation {
        RDGResourceHandle Resource;
        std::variant<render::BufferDescriptor, render::TextureDescriptor> Desc;
        int32_t AliasGroup;  // -1 = 不别名
    };

    vector<CompiledPass> Passes;       // 线性执行序列
    BarrierBatch EpilogueBarriers;     // export 转换
    vector<ResourceLifetime> Lifetimes;
    vector<ResourceAllocation> Allocations;

    string ExportCompiledGraphviz() const;
    string ExportExecutionPlantUml() const;
};
```

---

## 五、编译流程总结图

```
原始 RDG (nodes + edges)
        │
        ▼
 ┌──────────────────┐
 │ 1. 拓扑排序       │  Kahn's Algorithm + 启发式优化
 └────────┬─────────┘
          │ topoOrder
          ▼
 ┌──────────────────┐
 │ 2. 提取 Pass 序列 │  过滤 Pass 节点
 └────────┬─────────┘
          │ passExecutionOrder
          ▼
 ┌──────────────────┐
 │ 3. Dead Code 消除 │  反向 BFS 标记活跃节点
 └────────┬─────────┘
          │ 剪枝后的 passExecutionOrder
          ▼
 ┌──────────────────┐
 │ 4. 资源状态追踪   │  Import 初始化 + 逐 Pass barrier
 │  & Barrier 计算   │  + Export epilogue
 │                   │  (保留 Stage/Access/Layout)
 └────────┬─────────┘
          │ barriers per pass + epilogue
          ▼
 ┌──────────────────┐
 │ 5. 生命周期分析    │  [firstUse, lastUse] 区间
 └────────┬─────────┘
          │ lifetimes
          ▼
 ┌──────────────────┐
 │ 6. 内存别名分析   │  贪心/区间着色 (可选)
 └────────┬─────────┘
          │ aliasGroups
          ▼
 ┌──────────────────┐
 │ 7. RenderPass 构建│  GraphicsPass → Desc（不创建真实对象）
 └────────┬─────────┘
          │
          ▼
     CompileResult (RDG IR)
          │
          ▼
 ┌──────────────────┐
 │ 8. Barrier Lower │  分后端: D3D12 / Vulkan
 │   (执行阶段)      │  翻译 RDG IR → RHI Barrier
 └──────────────────┘
```

---

## 六、Barrier Lowering：D3D12 vs Vulkan 后端差异

编译阶段输出 RDG IR 形式的 barrier，由执行器在录制 CommandBuffer 时 lower 到具体后端。
两个后端的 barrier 模型差异是拆分编译/执行的核心原因。

### 6.1 模型对比

| 维度 | D3D12 (Legacy Barriers) | D3D12 (Enhanced Barriers) | Vulkan |
|------|------------------------|--------------------------|--------|
| **核心抽象** | State-to-state transition | Barrier + layout + sync | Access/Stage/Layout 三元组 |
| **粒度** | 整资源或 per-subresource | Buffer region, texture subresource | Buffer range, image subresource range |
| **状态模型** | `D3D12_RESOURCE_STATES` 位掩码 | 解耦 sync + access + layout | `VkAccessFlags` + `VkPipelineStageFlags` + `VkImageLayout` |
| **UAV barrier** | 专用类型 `D3D12_RESOURCE_BARRIER_TYPE_UAV` | `D3D12_BARRIER_TYPE_UAV` | 用 memory barrier + 相同 layout 实现 |
| **Aliasing barrier** | 专用类型 `D3D12_RESOURCE_BARRIER_TYPE_ALIASING` | 显式 aliasing barrier | `VkMemoryBarrier` 或 layout transition from Undefined |
| **Split barrier** | `BEGIN_ONLY` / `END_ONLY` 标志对 | 不适用（已解耦） | 不直接支持（用 event 可模拟） |
| **Image layout** | 隐含在 resource state 中 | 显式 `D3D12_BARRIER_LAYOUT` | 显式 `VkImageLayout` |
| **Pipeline stage** | 隐含在 resource state 中 | 显式 `D3D12_BARRIER_SYNC` | 显式 `VkPipelineStageFlags` |
| **State promotion** | 支持（Common → read-only 自动提升） | 不适用 | 不支持 |
| **State decay** | 支持（ExecuteCommandLists 边界自动回退） | 不适用 | 不支持 |

### 6.2 Lowering 策略

#### D3D12 Lowering

```
输入: CompiledBarrierBatch (RDG IR)

对每个 BufferBarrier:
  beforeState = MapRDGAccessToBufferState(srcAccess)
  afterState  = MapRDGAccessToBufferState(dstAccess)
  if beforeState == afterState == UnorderedAccess:
    输出 UAV barrier
  else if beforeState != afterState:
    输出 Transition barrier
  // else: 状态相同，可能被 state promotion 覆盖，跳过

对每个 TextureBarrier:
  beforeState = MapRDGAccessToTextureState(srcAccess, srcLayout)
  afterState  = MapRDGAccessToTextureState(dstAccess, dstLayout)
  if beforeState == afterState == UnorderedAccess:
    输出 UAV barrier
  else:
    输出 Transition barrier (with subresource if range != All)
```

**D3D12 特有优化机会**：
- **State promotion**：处于 Common 状态的资源被只读访问时可自动提升，无需 barrier
- **Split barriers**：如果 barrier 的 src pass 和 dst pass 之间有其他 pass，可以拆为
  `BEGIN_ONLY`（紧跟 src pass 后）+ `END_ONLY`（紧贴 dst pass 前），让 GPU 异步转换
- **批量提交**：`ID3D12GraphicsCommandList::ResourceBarrier` 接受 span，尽量合批

#### Vulkan Lowering

```
输入: CompiledBarrierBatch (RDG IR)

srcStageMask = 0, dstStageMask = 0
bufferBarriers = [], imageBarriers = []

对每个 BufferBarrier:
  srcAccess = MapRDGAccessToVkAccess(srcAccess_RDG)
  dstAccess = MapRDGAccessToVkAccess(dstAccess_RDG)
  srcStage  = MapRDGStageToVkStage(srcStage_RDG)
  dstStage  = MapRDGStageToVkStage(dstStage_RDG)
  合并到 VkBufferMemoryBarrier + srcStageMask/dstStageMask

对每个 TextureBarrier:
  srcAccess = MapRDGAccessToVkAccess(srcAccess_RDG)
  dstAccess = MapRDGAccessToVkAccess(dstAccess_RDG)
  oldLayout = MapRDGLayoutToVkLayout(srcLayout_RDG)
  newLayout = MapRDGLayoutToVkLayout(dstLayout_RDG)
  srcStage  = MapRDGStageToVkStage(srcStage_RDG)
  dstStage  = MapRDGStageToVkStage(dstStage_RDG)
  合并到 VkImageMemoryBarrier + srcStageMask/dstStageMask

vkCmdPipelineBarrier(srcStageMask, dstStageMask, ..., bufferBarriers, imageBarriers)
```

**Vulkan 特有优化机会**：
- **精确 pipeline stage**：RDG 保留的 `RDGExecutionStage` 可精确映射到 Vulkan stage，
  避免过粗的 `ALL_COMMANDS_BIT`
- **Render pass 内 subpass dependency**：相邻 graphics pass 共享 attachment 时，
  可合并为 Vulkan subpass，用 `VkSubpassDependency` 替代显式 barrier
- **Image layout 合并**：连续的 layout-compatible 只读访问不需要 layout transition

### 6.3 RDG → RHI 映射参考表

RDG IR 中保留了完整的 `(Stage, Access, Layout)` 三元组，lowering 时按以下规则映射：

**Access 映射**：

| RDGMemoryAccess | render::BufferState | render::TextureState | VkAccessFlags |
|---|---|---|---|
| `VertexRead` | `Vertex` | — | `VERTEX_ATTRIBUTE_READ` |
| `IndexRead` | `Index` | — | `INDEX_READ` |
| `ConstantRead` | `CBuffer` | — | `UNIFORM_READ` |
| `ShaderRead` | `ShaderRead` | `ShaderRead` | `SHADER_READ` |
| `ShaderWrite` | `UnorderedAccess` | `UnorderedAccess` | `SHADER_READ \| SHADER_WRITE` |
| `ColorAttachmentRead` | — | `RenderTarget` | `COLOR_ATTACHMENT_READ` |
| `ColorAttachmentWrite` | — | `RenderTarget` | `COLOR_ATTACHMENT_READ \| WRITE` |
| `DepthStencilRead` | — | `DepthRead` | `DEPTH_STENCIL_ATTACHMENT_READ` |
| `DepthStencilWrite` | — | `DepthWrite` | `DEPTH_STENCIL_ATTACHMENT_WRITE` |
| `TransferRead` | `CopySource` | `CopySource` | `TRANSFER_READ` |
| `TransferWrite` | `CopyDestination` | `CopyDestination` | `TRANSFER_WRITE` |
| `HostRead` | `HostRead` | — | `HOST_READ` |
| `HostWrite` | `HostWrite` | — | `HOST_WRITE` |
| `IndirectRead` | `Indirect` | — | `INDIRECT_COMMAND_READ` |

**Stage 映射**：

| RDGExecutionStage | D3D12 (隐含在 state) | VkPipelineStageFlags |
|---|---|---|
| `VertexInput` | (state 隐含) | `VERTEX_INPUT` |
| `VertexShader` | (state 隐含) | `VERTEX_SHADER` |
| `PixelShader` | (state 隐含) | `FRAGMENT_SHADER` |
| `DepthStencil` | (state 隐含) | `EARLY/LATE_FRAGMENT_TESTS` |
| `ColorOutput` | (state 隐含) | `COLOR_ATTACHMENT_OUTPUT` |
| `ComputeShader` | (state 隐含) | `COMPUTE_SHADER` |
| `Copy` | (state 隐含) | `TRANSFER` |
| `Host` | (state 隐含) | `HOST` |
| `Present` | `PRESENT` | `BOTTOM_OF_PIPE` |

**Layout 映射**：

| RDGTextureLayout | D3D12 (隐含在 state) | VkImageLayout |
|---|---|---|
| `Undefined` | `Common` | `UNDEFINED` |
| `General` | `UnorderedAccess` / `Common` | `GENERAL` |
| `ShaderReadOnly` | `ShaderRead` | `SHADER_READ_ONLY_OPTIMAL` |
| `ColorAttachment` | `RenderTarget` | `COLOR_ATTACHMENT_OPTIMAL` |
| `DepthStencilReadOnly` | `DepthRead` | `DEPTH_STENCIL_READ_ONLY_OPTIMAL` |
| `DepthStencilAttachment` | `DepthWrite` | `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` |
| `TransferSource` | `CopySource` | `TRANSFER_SRC_OPTIMAL` |
| `TransferDestination` | `CopyDestination` | `TRANSFER_DST_OPTIMAL` |
| `Present` | `Present` | `PRESENT_SRC_KHR` |

---

## 七、编译阶段可进行的图优化

除了基本的 barrier 计算，编译阶段还可以对图进行以下优化。
按 **投入/收益比** 排序，推荐从上到下逐步实现。

### 7.1 拓扑排序启发式优化 (Barrier Minimization)

**收益**：中-高 | **实现难度**：中

Kahn 算法中，当同入度的候选 Pass 有多个时，选择顺序影响 barrier 数量。

**策略 A：资源亲和度排序**

```
对候选集合中的每个 Pass P:
  affinityScore(P) = |P 使用的资源 ∩ 上一个执行的 Pass 使用的资源|
选择 affinityScore 最高的 P
```

**策略 B：写-读紧邻**

```
对候选集合中的每个 Pass P:
  对 P 的每个读依赖资源 R:
    if R 的写入 Pass W 刚刚执行完毕:
      优先选择 P（减少 R 的状态在中间被其他 Pass 覆盖的可能）
```

### 7.2 Barrier 批量合并

**收益**：中 | **实现难度**：低

同一个 Pass 前的所有 barrier 应合并为一次 batch 调用。
D3D12/Vulkan 都支持批量 barrier，减少 GPU 管线刷新。

进一步地，如果两个相邻 Pass 之间的 barrier 互不依赖，
可以把后一个 Pass 的部分 barrier 提前到前一个 Pass 的 batch 中。

### 7.3 读-读合并 (Read-Read Coalescing)

**收益**：中 | **实现难度**：低

同一资源被连续多个 Pass 以只读方式访问时，第一次 barrier 后续续读不再需要 barrier。

更进一步：如果资源同时被多个 Pass 以不同的只读方式访问（如 `ShaderRead` + `DepthStencilRead`），
在 RHI 层面某些组合是兼容的：
- D3D12：`PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE | DEPTH_READ` 可并存
- Vulkan：不同 stage 读取同一 layout 无需 barrier

### 7.4 D3D12 Split Barriers

**收益**：中-高 | **实现难度**：中

当一个 barrier 的 source pass 和 destination pass 之间有 N 个其他 pass 时，
可以将 barrier 拆分为 `BEGIN_ONLY`（source pass 之后）和 `END_ONLY`（dest pass 之前），
让 GPU 在中间 N 个 pass 执行期间异步完成状态转换。

```
对每个 barrier B = { srcPass, dstPass, resource }:
  gap = dstPass.executionIndex - srcPass.executionIndex
  if gap > 1:
    在 srcPass 后插入 BEGIN_ONLY barrier
    将 dstPass 前的 barrier 改为 END_ONLY
```

> 这是 D3D12 特有优化，Vulkan 无直接等价（可用 VkEvent 模拟但开销不一定更低）。
> 需要在 lowering 阶段实现，编译阶段只需标记 gap > 1 的 barrier 为 "可拆分"。

### 7.5 Vulkan Subpass 合并

**收益**：高（移动端极高） | **实现难度**：高

当连续的 GraphicsPass 使用相同或兼容的 attachment 配置时，可以将它们合并为
Vulkan 的 subpasses，使用 `VkSubpassDependency` 替代显式 barrier。

**合并条件**：
- 两个 Pass 都是 `RDGGraphicsPassNode`
- 前一个 Pass 的 Color/DepthStencil output 是后一个 Pass 的 input attachment
- 两个 Pass 的 attachment 尺寸（width/height/sampleCount）相同
- 中间没有对这些 attachment 的非 attachment 访问（如 compute shader UAV 读取）

**判定算法**：

```
对执行序列中的相邻 GraphicsPass 对 (P_i, P_{i+1}):
  sharedAttachments = P_i 的输出 attachment ∩ P_{i+1} 的输入 attachment
  if sharedAttachments 非空 && 尺寸兼容 && 无中间非 attachment 访问:
    标记为 subpass 合并候选
    将中间的 barrier 替换为 VkSubpassDependency

// 可链式合并: P1→P2→P3 如果都满足条件可合为单个 VkRenderPass
```

> 这主要对**移动端 tile-based GPU** 有巨大收益（减少 tile memory flush）。
> 桌面端收益较小但仍有正面影响。

### 7.6 异步计算重叠 (Async Compute Overlap)

**收益**：高 | **实现难度**：高

将独立的 ComputePass 调度到异步 compute queue 上，与 graphics queue 并行执行。

**候选识别**：
```
对每个 ComputePass C:
  if C 不依赖任何正在执行的 GraphicsPass 的输出:
    且 C 的输出不被紧邻的下一个 GraphicsPass 消费:
    标记为 async compute 候选
```

**需要额外产出**：
- Queue assignment 表
- 跨队列 fence signal/wait 指令
- Queue ownership transfer barrier（Vulkan 需要，D3D12 不需要）

> 当前 `GpuRuntime` 通过 `BeginAsync(QueueType, queueSlot)` 已支持多队列提交，
> `SubmitAsync` 返回的 `GpuTask` 可用于跨队列同步。
> `BarrierBufferDescriptor` / `BarrierTextureDescriptor` 的 `OtherQueue` 字段
> 已支持跨队列 ownership transfer。

### 7.7 D3D12 State Promotion/Decay 利用

**收益**：低-中 | **实现难度**：低

D3D12 中处于 `Common` 状态的资源在被只读访问时会自动 promote：
- Buffer：`Common` 可自动提升到 `Vertex`/`Index`/`CBuffer`/`ShaderRead`/`Indirect`/`CopySource`
- Texture（非 RT/DS/UAV 同时使用写的）：`Common` 可自动提升到 `ShaderRead`/`CopySource`

在 `ExecuteCommandLists` 边界，promoted 状态自动 decay 回 `Common`。

**优化**：对生命周期只在一个 submit batch 内的 Internal 资源，初始创建为 `Common` 状态，
省去第一次使用前的 barrier。

### 7.8 优化实现优先级建议

| 优先级 | 优化 | 原因 |
|--------|------|------|
| P0 | Barrier 批量合并 + 读-读合并 | 实现简单，正确性基础 |
| P1 | 拓扑排序启发式 | 减少 barrier 总数 |
| P2 | D3D12 Split Barriers | 桌面端 D3D12 性能关键 |
| P2 | D3D12 State Promotion | 减少冗余 barrier |
| P3 | Vulkan Subpass 合并 | 移动端关键，桌面端次要 |
| P3 | 异步计算重叠 | 需要较多基础设施 |
| P4 | 内存别名分析 | 需要 placed resource 支持 |

> **注**：Dead Code 消除已提升为核心编译阶段（阶段 3），不再列为可选优化。

---
