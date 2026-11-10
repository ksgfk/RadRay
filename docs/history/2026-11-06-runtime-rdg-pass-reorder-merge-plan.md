> - 适用: 实施 `RadRay::runtime` RDG（Render Dependency Graph）的 WAR 依赖补齐、确定性贪心拓扑重排、Raster/Compute Pass 合并折叠与批量 Barrier 下发
> - 权威: 经用户确认的实施计划；不作为已实现契约，当前已实现行为见 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `modules/runtime/include/radray/runtime/rdg.h`, `modules/runtime/src/rdg.cpp`, `modules/runtime/include/radray/runtime/render_graph_pipeline.h`, `modules/runtime/src/render_graph_pipeline.cpp`, `modules/runtime/tests/test_rdg.cpp`, `examples/mesh_drawing_pipeline/pipeline.cpp`

# Runtime RDG Pass 重排与合并实现计划

## 状态、基线与范围

- 记录日期：2026-11-06
- 源码基线：`605063921e8ac76df9656d7d0783a327e145d89b`
- 状态：设计已确认，待实施
- 范围：严格限定在 `modules/runtime/`（`rdg.h`、`rdg.cpp` 及单测 `modules/runtime/tests/test_rdg.cpp`），**不向 `modules/render/`（RHI 层）外溢改动**，且保持 `RDGBuilder`、`CompiledRDG` 与 `RenderGraphFramePipeline` 现有公共 C++ API 签名兼容。

---

## 一、问题背景与图建模选型

### 1. 基线 RDG 的执行局限

在 [前序 RDG 接入计划](2026-11-05-runtime-rdg-pipeline-interface-plan.md) 落地的最小可用 RDG（[`modules/runtime/src/rdg.cpp`](../../modules/runtime/src/rdg.cpp)）中，编译器与执行器采用最简策略：
1. **仅建 RAW / WAW 依赖，固定按声明顺序执行**：每个资源只维护标量 `LastWriter`，不记录消费当前版本的 `Readers`（无 WAR 先读后写边），因此无法在编译期安全重排 Pass 顺序；
2. **每个 Pass 独立开关 RHI Encoder**：即使相邻多个 `Raster` Pass 绑定完全相同的颜色/深度附件（如教学样例 [`examples/mesh_drawing_pipeline/pipeline.cpp`](../../examples/mesh_drawing_pipeline/pipeline.cpp) 中的 `Tutorial Color` → `Tutorial Outline` → `Tutorial Transparent`），也会执行 3 次 `BeginRenderPass` / `EndRenderPass`，并在 Pass 之间插入 2 轮 `RenderTarget → RenderTarget` 与 `DepthWrite → DepthWrite` 屏障；
3. **无关 Pass 打断连续光栅化或计算**：若调用方在两个同附件 `Raster` Pass 声明之间穿插了无依赖的 `UploadBuffer`（如按需上传 `Outline` 的 `passUbo1`）或独立 `Compute` Pass，固定声明顺序会强制结束当前 RenderPass、下发传输/计算命令，再重新开启 RenderPass；
4. **逐资源单条下发 `ResourceBarrier`**：[`TransitionTexture`](../../modules/runtime/src/rdg.cpp) / [`TransitionBuffer`](../../modules/runtime/src/rdg.cpp) 每次状态转换均单独调用一次 `Command.ResourceBarrier(std::span{&barrier, 1})`，未按 Pass 或合并组聚合成单次批量调用。

### 2. 图建模对照：显式二分版本图 vs Pass 节点 + 版本化 `(LastWriter, Readers)` 状态槽

将资源也作为节点的经典建模是**版本化资源二分有向无环图（Bipartite SSA DAG）**：每次写入产出新的资源版本节点 $R_k^{(v+1)}$，读操作连边 $R_k^{(v)} \to P_i$，写操作连边 $P_i \to R_k^{(v+1)}$，局部写/读改写连边 $R_k^{(v)} \to P_i \to R_k^{(v+1)}$。

对照 RadRay 当前边界（单队列、每帧在 `FrameSlot::GetArena()` 动态构建、暂不做跨生命周期物理内存别名重命名）：
- 若显式实例化 `ResourceVersion` 图节点，节点与边数量会膨胀 $2 \sim 4$ 倍，且到了 `CompiledRDG::Execute` 阶段仍需把存活版本节点重新折叠回底层物理资源状态槽 [`RDGPhysicalResource`](../../modules/runtime/src/rdg.cpp) 推导 `before → after` Barrier；
- 反之，在每个逻辑资源上同时维护当前版本的 **`LastWriter`（唯一 Producer Pass）** 与 **`Readers`（消费当前版本的 Consumer Pass 列表）**，即可在保持“纯 Pass 节点图”紧凑内存布局的同时，以 $\Theta(A)$ 线性复杂度完整表达二分版本图的 **RAW（`LastWriter → Reader`）、WAW（`LastWriter → Writer`）与 WAR（`Readers → NextWriter`）** 全部有向约束。

因此本计划采用 **Pass 节点图 + 资源版本状态槽 `(LastWriter, Readers)` + 编译期合并分组（`RDGExecutionGroup`）** 的建模方案。

---

## 二、已确认的设计决策

| 编号 | 决策项 | 确认方案 | 约束与设计理由 |
|---|---|---|---|
| **D1** | 完整依赖建图与裁剪边界分离 | 每个资源维护 `(LastWriter, Readers)`，分别建立**数据流依赖边（`DataDependencies`：RAW 与保留写 WAW）**和**执行序依赖边（`OrderDependencies`：WAR 与同缓冲后续访问）**。**反向可达性裁剪仅沿 `DataDependencies` 遍历**；**拓扑重排同时遵守两类依赖** | 若裁剪也沿 WAR 边反向遍历，一个写入 Output 资源的新 Writer 会把此前读取旧版本的死 Reader Pass 错误保活。将“保活数据流边”与“存活节点间的先后序边”分离，既保证死 Reader 被裁剪，又保证存活 Reader 必然在覆写 Writer 之前执行 |
| **D2** | 确定性贪心拓扑重排（Greedy Topological Clustering） | 在存活 Pass 诱导子图上运行 Kahn 就绪队列（`inDegree == 0`），按 3 级优先级选择下一 Pass（同优先级始终按**原始声明下标升序**打破平局）：<br/>1. 延续当前已打开的 `Raster` / `Compute` 合并段；<br/>2. 未打开合并段时优先聚拢 `Upload` / `Copy` Pass；<br/>3. 原始声明下标升序兜底 | 聚拢前置 `Upload` / `Copy` 可避免传输操作插在两个同附件 `Raster` Pass 之间打断 RenderPass；贪心延续当前合并段可最大化单次 `BeginRenderPass` / `BeginComputePass` 覆盖范围；声明下标兜底保证重排结果 100% 确定且可复现 |
| **D3** | `Raster` Pass 合并条件、组入口屏障前移（Hoisting）、折叠规则与模板初态隔离 | 满足「颜色与深度附件 `(TextureHandle, ExplicitView)` 及深度 `TextureState` 完全相同 + 前 Pass 全 `Store` / 后 Pass 全 `Load` + 后 Pass 与组内已选 Pass 之间无非附件资源冲突（未被组内前序子 Pass 访问的资源屏障可前移到组入口；已被组内前序子 Pass 访问的非附件资源须同为相同状态的只读访问）」的相邻存活 `Raster` Pass 合并进单次 `BeginRenderPass` / `EndRenderPass`。首 Pass 决定 `LoadAction`/`ClearValue`，末 Pass 决定 `StoreAction`，`AllowUavWrites` 取逻辑或；每个子 Pass 回调前自动调用 `encoder.SetStencilReference(0)` 并包裹 `PushDebugGroup`/`PopDebugGroup` | RHI [`CommandBufferValidation::Recording`](../../modules/render/src/rhi_validation.cpp) 禁止在 Encoder 作用域内调用 `ResourceBarrier`，且 [`GraphicsEncoderValidation`](../../modules/render/src/rhi_validation.cpp) 在 `BeginRenderPass` 时固定附件签名与 `DepthUsage`；未被组内前序子 Pass 访问的资源（如教学样例中仅被 `Outline` 读取的 `constants[1]`）其前置生产者在组开启前已完成，将其屏障前移（Hoist）至组入口即可消除伪阻断；同状态共享附件由硬件 ROP / Depth Test 按 API 绘制顺序保序；显式重置 `SetStencilReference(0)` 对齐 RHI [模板参考值契约](../architecture/render-rhi.md#模板参考值) |
| **D4** | `Compute` Pass 合并条件与组入口屏障前移 | 重排后相邻的存活 `Compute` Pass，若后一 Pass 访问的资源与组内前序子 Pass 无组内屏障冲突（未被组内前序子 Pass 访问的资源可将屏障前移至 `BeginComputePass` 之前；已被组内前序子 Pass 访问的资源须同为相同状态的只读访问，无状态转换且无组内 `PendingWrite` / UAV 屏障），则合并共享同一次 `BeginComputePass` / `EndComputePass`，各子 Pass 回调分别由 `PushDebugGroup`/`PopDebugGroup` 包裹 | 遵守 RHI 禁止在 `ComputeCommandEncoder` 内下发 `ResourceBarrier` 的约束，将组外已就绪资源的转换前移到组入口，在连续独立计算派发之间省去重复创建/销毁 Encoder |
| **D5** | 批量 `ResourceBarrier` 下发 | 以每个合并执行组（`RDGExecutionGroup`）为单位，在组开启前遍历组内全部子 Pass，将各资源首次进入该组时所需的 `ResourceBarrierDescriptor` 收集到连续缓冲区，单次调用 `command.ResourceBarrier(span)`；图末所有存活导入资源的 `finalState` 收尾屏障同样合并为单次调用 | 消除逐资源单条虚函数调用、RHI 校验循环与驱动命令录制开销，并统一承载组内后续子 Pass 前移（Hoisted）的资源转换屏障 |

---

## 三、分项详细实现设计

### 1. 内部数据结构演进（`modules/runtime/src/rdg.cpp`）

在 `radray::detail` 内部结构中扩展依赖边、资源当前版本 `Readers` 与编译产出的执行分组 `RDGExecutionGroup`（不改变 `rdg.h` 公共类型大小与 ABI）：

```cpp
struct RDGPass {
    RDGPassKind Kind;
    string Name;
    vector<RDGAccess> Accesses{};
    vector<MeshPassId> MeshPasses{};
    // 数据流依赖（Pass -> Predecessor）：用于反向可达性裁剪（RAW + 保留写 WAW）
    vector<uint32_t> DataDependencies{};
    // 附加顺序依赖（Pass -> Predecessor）：仅在两节点均存活时约束拓扑顺序（WAR）
    vector<uint32_t> OrderDependencies{};
    vector<RDGColorAttachment> Colors{};
    std::optional<RDGDepthAttachment> Depth{};
    Nullable<void*> Callback{nullptr};
    Nullable<bool (*)(void*, RDGRasterPassContext&)> RasterExecute{nullptr};
    Nullable<bool (*)(void*, RDGComputePassContext&)> ComputeExecute{nullptr};
    Nullable<bool (*)(void*, RDGCopyPassContext&)> CopyExecute{nullptr};
    std::span<const byte> UploadBytes{};
    uint64_t UploadOffset{0};
    uint32_t Width{1}, Height{1}, Layers{1};
    bool NeverCull{false};
    bool Alive{false};
};

struct RDGTexture {
    render::TextureDescriptor Desc;
    Nullable<render::Texture*> Imported{nullptr};
    render::TextureState InitialState{render::TextureState::Undefined};
    std::optional<render::TextureState> FinalState{};
    std::optional<uint32_t> LastWriter{};
    vector<uint32_t> Readers{};  // 当前版本（LastWriter 之后）的所有只读/采样访问 Pass
    bool Output{false};
    bool Alive{false};
};

struct RDGBuffer {
    render::BufferDescriptor Desc;
    Nullable<render::Buffer*> Imported{nullptr};
    render::BufferState InitialState{render::BufferState::Undefined};
    std::optional<render::BufferState> FinalState{};
    std::optional<uint32_t> LastWriter{};
    vector<uint32_t> Readers{};  // 当前版本（LastWriter 之后）的所有只读访问 Pass
    bool Output{false};
    bool Alive{false};
};

struct RDGExecutionGroup {
    RDGPassKind Kind{RDGPassKind::Raster};
    vector<uint32_t> Passes{};  // 组内按重排后顺序执行的 Pass 下标（Upload/Copy 固定为 1 个，Raster/Compute 可为多个）
};

struct RDGData {
    vector<RDGTexture> Textures;
    vector<RDGBuffer> Buffers;
    vector<RDGPass> Passes;
    vector<uint32_t> ScheduledPasses;       // 拓扑重排后的存活 Pass 序列
    vector<RDGExecutionGroup> Groups;       // 合并后的执行单元序列
    vector<MeshPassId> AliveMeshPasses;
    bool Failed{false};
    bool Sealed{false};
};
```

---

### 2. 编译期阶段一：完整依赖建图（RAW + WAW + WAR）

在 [`RDGBuilder::Compile()`](../../modules/runtime/src/rdg.cpp) 按声明顺序扫描 `index = 0 .. _data.Passes.size() - 1` 时，对每条 `access` 维护目标资源的 `(lastWriter, readers)`：

1. **$O(1)$ 边去重与 `readers` 单调追加**：
   - 由于 [`AddAccess`](../../modules/runtime/src/rdg.cpp) 已保证单个 Pass 内对同一资源的访问记录唯一，且 `Compile()` 按 `index` 严格升序扫描 Pass，向 `readers` 追加当前 Pass 只需直接 `readers.push_back(index)`（天然严格递增且无重复，无需线性查重）；
   - 向 `pass.DataDependencies` 与 `pass.OrderDependencies` 添加前驱节点时，**两类依赖必须各自独立去重**（分别维护 `seenDataStamp[target] == passIndex` 与 `seenOrderStamp[target] == passIndex`，严禁共用同一个标记数组，否则同一对 Pass 之间先遇到的 WAR `OrderDependencies` 边会抑制后遇到的 RAW `DataDependencies` 边，导致上游生产者在裁剪阶段被误删）；两类边仅在阶段三构建存活子图拓扑入度时合并去重。
2. **未初始化读取检查（保持现有规则）**：
   - 若 `access.Read && !lastWriter && (!imported || undefined)`，报告 `"resource read before initialization"`。
3. **根据访问模式连接 `DataDependencies` 与 `OrderDependencies`**：
   - **情况 A：纯读取（`access.Read && !access.Write`）**
     - 若 `lastWriter` 存在：`AddUniqueEdge(pass.DataDependencies, *lastWriter, index)`（RAW 数据流依赖）；
     - 将当前 Pass 记入当前版本读者集：`readers.push_back(index)`。
   - **情况 B：保留旧内容的写入（`access.Write && (access.Read || access.Preserve)`，包括 `ReadWrite*`、普通 `WriteBuffer`/`WriteTexture`、分段 `UploadBuffer`、带 `LoadAction::Load` 的颜色或深度/模板附件）**
     - 若 `lastWriter` 存在：`AddUniqueEdge(pass.DataDependencies, *lastWriter, index)`（RAW / 保留写 WAW 数据流依赖，确保上游生产者在下游存活时一并保活）；
     - 对 `readers` 中的每个 `readerPass`：`AddUniqueEdge(pass.OrderDependencies, readerPass, index)`（WAR 顺序依赖：必须等旧版本的所有存活读者读完，才能原位修改该资源）；
     - 版本推进：`lastWriter = index; readers.clear();`。
   - **情况 C：全量覆盖写（`access.Write && !access.Read && !access.Preserve`，即 `LoadAction::Clear` 或 `LoadAction::DontCare` 的颜色及双 aspect 全覆盖深度/模板附件）**
     - 不向 `lastWriter` 建立 `DataDependencies`（允许被完全覆盖且无读者消费的前序死写入被裁剪）；
     - 若 `lastWriter` 存在：`AddUniqueEdge(pass.OrderDependencies, *lastWriter, index)`（若前序 Writer 因其他输出分支而存活，全量覆盖写仍须排在它之后，防止重排把旧写入放到新 Clear 之后反向覆盖新结果！）；
     - 对 `readers` 中的每个 `readerPass`：`AddUniqueEdge(pass.OrderDependencies, readerPass, index)`（WAR 顺序依赖：防止 Clear 覆写被重排到旧版本的读者之前）；
     - 版本推进：`lastWriter = index; readers.clear();`。

> **关键边界说明（为什么全量覆盖写仍需对旧 `lastWriter` 建立 `OrderDependencies`）**：
> 假设 `Pass 0` 写入 `texA` 和 `texB`，`Pass 1` 读取 `texB`（保活了 `Pass 0`），而 `Pass 2` 用 `LoadAction::Clear` 覆写 `texA` 并作为最终输出。`Pass 2` 不需要 `Pass 0` 来保活（属于 `OrderDependencies` 而非 `DataDependencies`），但当 `Pass 0` 和 `Pass 2` **同时存活**时，`Pass 2` 必须排在 `Pass 0` 之后执行，否则 `Pass 0` 后执行会覆写掉 `Pass 2` 的结果。将旧 `lastWriter` 放入 `OrderDependencies` 即可完美解决这一 WAW 顺序约束，同时不影响 `Pass 0` 在无人消费时的死代码裁剪。

---

### 3. 编译期阶段二：仅沿 `DataDependencies` 的反向可达性裁剪

与现有 [`roots()`](../../modules/runtime/src/rdg.cpp) 逻辑保持一致，仅遍历 `pass.DataDependencies`（**不遍历 `OrderDependencies`**）：
1. 遍历全部 `Textures` 与 `Buffers`：对 `resource.Output == true` 的资源**先直接标记 `resource.Alive = true`**（保证即使图中没有任何 Pass 访问某个导入 Output 资源，图末仍会对其执行 `FinalState` 收尾屏障），并将其最终 `LastWriter`（若存在；非导入瞬态 Output 无 `LastWriter` 则报告 `"transient output has no writer"` 编译失败）与所有 `NeverCull` 的 Pass 一起入栈 `pending`；
2. 反向 DFS/BFS 标记 `pass.Alive = true`，并将 `pass.DataDependencies` 压入 `pending`；
3. 遍历所有 `pass.Alive == true` 的 Pass，将其访问的资源标记为 `resource.Alive = true`。

---

### 4. 编译期阶段三：组内资源访问追踪、确定性贪心拓扑重排与合并分组

由于 RHI 禁止在 `BeginRenderPass` / `EndRenderPass` 与 `BeginComputePass` / `EndComputePass` 内部调用 `ResourceBarrier`，判断一个就绪候选 Pass（`inDegree == 0`）能否并入当前已打开的合并组 `openGroup`，核心在于区分**“可在组入口前移（Hoist）完成的屏障”**与**“必须在组内两个子 Pass 之间插入的阻断性屏障”**：

#### 4.1 组入口屏障前移（Group Prologue Barrier Hoisting）原理
当候选 Pass `candidatePass` 处于拓扑就绪状态（`inDegree == 0`）时，它的所有前驱 Pass 均已在 `openGroup` 开启之前（或就是 `openGroup` 内的前序子 Pass）调度完毕。对 `candidatePass` 访问的任意资源 $R$：
- **情况 1：$R$ 尚未被 `openGroup` 内的任何已选子 Pass 访问过**
  - 说明 $R$ 的最后一次状态变更或写入发生在 `openGroup` 开启**之前**，且 `openGroup` 内已有的前序子 Pass 完全不触碰 $R$（例如教学样例 [`pipeline.cpp`](../../examples/mesh_drawing_pipeline/pipeline.cpp) 中，`constants[1]` 在帧初完成 `UploadBuffer` 后保持 `HostWrite`，`Tutorial Color` 只读 `constants[0]` 不碰 `constants[1]`，直到 `Tutorial Outline` 才首次以 `CBuffer` 状态读取 `constants[1]`）。
  - 因此，$R$ 进入 `candidatePass` 所需的 `ResourceBarrier`（如 `constants[1]` 的 `HostWrite → CBuffer`）**可以安全前移（Hoist）到 `openGroup` 的组入口（即 `BeginRenderPass` / `BeginComputePass` 之前）**统一完成，不会产生组内中间屏障，**不阻断合并**！
- **情况 2：$R$ 已被 `openGroup` 内的某个前序子 Pass 访问过（记组内已有访问为 `inGroupAccess`）**
  - 若 $R$ 是当前 `Raster` 合并组绑定的颜色附件（均为 `TextureState::RenderTarget`）或深度/模板附件（均为同一 `Depth->State`）：由硬件光栅化流水线（ROP 混合与 Depth/Stencil Test）按 API 绘制顺序保证连续读写顺序，无需 `ResourceBarrier`，**允许合并**；
  - 否则（$R$ 为 Buffer、非当前附件的 Texture，或被以非附件状态如 `ShaderRead` / `UnorderedAccess` 访问）：只有当**组内已有访问与本次访问均为只读且状态完全一致（`!inGroupAccess.Write && !access.Write && inGroupAccess.State == access.State`）**时，组入口前移转换后两个子 Pass 之间才无需任何中间 `ResourceBarrier`，**允许合并**；一旦任一方为写（`inGroupAccess.Write || access.Write`，如组内 UAV 写后读/写）或两者状态不同（`inGroupAccess.State != access.State`），就必须在两个子 Pass 之间下发屏障，此时**拒绝合并**。

#### 4.2 Raster / Compute 可合并性判定函数

给定当前正在构建的合并组 `openGroup`（其最后一个已选入的 Pass 为 `prevPass = Passes[openGroup.Passes.back()]`，组内已汇总的资源访问表为 `openGroupAccesses`）与一个当前 `inDegree == 0` 的就绪候选 Pass `candidatePass`：

1. **`CanMergeRasterPass(prevPass, candidatePass, openGroupAccesses)`**：
   - `prevPass.Kind == RDGPassKind::Raster && candidatePass.Kind == RDGPassKind::Raster`；
   - **颜色附件匹配**：`prevPass.Colors.size() == candidatePass.Colors.size()`，且对每个槽位 $i$：
     - `prevPass.Colors[i].Slot == candidatePass.Colors[i].Slot`；
     - `prevPass.Colors[i].Texture == candidatePass.Colors[i].Texture`；
     - `prevPass.Colors[i].View == candidatePass.Colors[i].View`；
     - `prevPass.Colors[i].Store == render::StoreAction::Store` 且 `candidatePass.Colors[i].Load == render::LoadAction::Load`；
   - **深度/模板附件匹配**：`bool(prevPass.Depth) == bool(candidatePass.Depth)`，且若存在深度附件：
     - `prevPass.Depth->Texture == candidatePass.Depth->Texture`；
     - `prevPass.Depth->View == candidatePass.Depth->View`；
     - `prevPass.Depth->State == candidatePass.Depth->State`（保证 `DepthUsage` 与原生 Layout / DSV 标志不变）；
     - 对该纹理格式具备的每个 aspect（Depth，以及含 Stencil 格式的 Stencil）：`prevPass` 的对应 `StoreAction` 必须为 `StoreAction::Store`，且 `candidatePass` 的对应 `LoadAction` 必须为 `LoadAction::Load`；
   - **组内非附件零冲突检查**：
     - 遍历 `candidatePass.Accesses`：
       - 若该 `access` 尚未出现在 `openGroupAccesses` 中：其转换屏障可前移至 `openGroup` 入口，放行；
       - 若该 `access` 已出现在 `openGroupAccesses` 中（记为 `inGroup`）：
         - 若它是当前绑定的颜色附件（`inGroup.State == TextureState::RenderTarget && access.State == TextureState::RenderTarget`）或当前绑定的深度附件（`inGroup.State == candidatePass.Depth->State && access.State == candidatePass.Depth->State`），放行；
         - 否则，要求 `!inGroup.Write && !access.Write && inGroup.State == access.State`，违则返回 `false`（不可合并）。
   - 全部通过则返回 `true`。

2. **`CanMergeComputePass(prevPass, candidatePass, openGroupAccesses)`**：
   - `prevPass.Kind == RDGPassKind::Compute && candidatePass.Kind == RDGPassKind::Compute`；
   - 遍历 `candidatePass.Accesses`：
     - 若该资源尚未出现在 `openGroupAccesses` 中：其转换屏障可前移至 `openGroup` 入口，放行；
     - 若该资源已出现在 `openGroupAccesses` 中（记为 `inGroup`）：要求 `!inGroup.Write && !access.Write && inGroup.State == access.State`（无组内状态变化且无 UAV 写同步），违则返回 `false`；
   - 全部通过则返回 `true`。

#### 4.3 确定性贪心拓扑调度循环（Kahn Clustering）

1. 对所有 `pass.Alive == true` 的节点 $u$：
   - 汇总其 `DataDependencies` 与 `OrderDependencies` 中同样满足 `dep.Alive == true` 的前驱节点 $p$（去重），建立正向邻接表 `dependents[p].push_back(u)` 并递增 `inDegree[u]`；
2. 初始化就绪队列 `ready`：按原始声明下标 `0 .. N-1` 升序压入所有 `Alive && inDegree == 0` 的 Pass；
3. 循环直至 `ready.empty()`：
   - 从 `ready` 中按以下 3 级优先级挑选最佳候选 `bestIdx`（同优先级取**原始声明下标最小**者）：
     - **优先级 1（延续当前打开的合并组）**：若存在未关闭的 `openGroup`（`Raster` 或 `Compute`），在 `ready` 中查找第一个满足 `CanMergeRasterPass` 或 `CanMergeComputePass` 的 Pass；若找到，直接选定它并归入 `openGroup`，将其 `Accesses` 合并记入 `openGroupAccesses`；
     - **优先级 2（关闭无法延续的组后，聚拢前置 `Upload` / `Copy`）**：若无 `openGroup` 或 `ready` 中没有任何节点能延续当前 `openGroup`，则先将 `openGroup` 封存关闭并清空 `openGroupAccesses`；随后在 `ready` 中查找第一个 `Kind == RDGPassKind::Upload || Kind == RDGPassKind::Copy` 的 Pass；
     - **优先级 3（原始声明顺序兜底）**：取 `ready` 中原始声明下标最小的 Pass；若该 Pass 为 `Raster` 或 `Compute`，以其为起点开启新的 `openGroup` 并初始化 `openGroupAccesses`（若为 `Upload` 或 `Copy` 则单独成单 Pass 的 `RDGExecutionGroup`）。
   - 将选出的 `bestPass` 从 `ready` 移除，追加到 `_data.ScheduledPasses`；
   - 遍历 `dependents[bestPass]` 中的每个后继 $w$：若 `--inDegree[w] == 0`，将 $w$ 插入 `ready`。
4. 校验 `_data.ScheduledPasses.size() == alivePassCount`（由于所有依赖只指向声明下标更小的节点，有向图天然无环，必然 100% 调度完毕）。
5. **严格按原始声明顺序**遍历 `_data.Passes` 中 `pass.Alive == true` 的 Pass，汇总去重的 `_data.AliveMeshPasses`（保持 [`CompiledRDG::GetAliveMeshPasses()`](../../modules/runtime/include/radray/runtime/rdg.h) 与 `ExtractMeshPass` 的调用顺序与现有声明序契约一致，不因 GPU 执行重排而改变 CPU 提取顺序）。

---

### 5. 执行期阶段四：按 `RDGExecutionGroup` 批量下发前移 Barrier 与合并录制（`CompiledRDG::Execute`）

在 [`CompiledRDG::Execute`](../../modules/runtime/src/rdg.cpp) 中，遍历 `_data.Groups`：

1. **收集并批量下发组前 Barrier（含组内后续子 Pass 的前移 Barrier）**：
   - 创建局部 `vector<render::ResourceBarrierDescriptor> groupBarriers`；
   - 按顺序遍历当前组 `group.Passes` 中的每个子 Pass，再遍历该子 Pass 的 `Accesses`：
     - 若该资源**在当前 `group` 内是首次被访问**（包括在 `group.Passes[1..k]` 中才首次登场的资源，如教学样例 `Tutorial Outline` 的 `constants[1]`）：对比 `execution.Textures` / `execution.Buffers` 的真实物理 `(CurrentState, PendingWrite)`，将需要生成的 `BarrierTextureDescriptor`、`BarrierBufferDescriptor` 或 `BarrierUavDescriptor` 追加到 `groupBarriers`，并更新物理 `(CurrentState, PendingWrite)`（该转换在打开本组 Encoder 前统一前移完成）；
     - 若该资源**已被当前 `group` 内的前序子 Pass 访问过**：由 4.2 节合并条件保证，它要么是同状态连续光栅化的共享颜色/深度附件（由硬件光栅化流水线保序，省略中间 `RenderTarget → RenderTarget` / `DepthWrite → DepthWrite` 屏障），要么是同状态连续只读的非附件资源（已在组入口完成转换且 `PendingWrite == false`），因此**不生成组内屏障**，仅更新 `PendingWrite |= access.Write`；
   - 若 `!groupBarriers.empty()`，在进入本组 Encoder 前执行**单次** `command.ResourceBarrier(groupBarriers)`。

2. **按组类型执行录制**：
   - **`RDGPassKind::Upload` / `RDGPassKind::Copy`（单 Pass 组）**：
     - 与现有逻辑相同，`PushDebugGroup(pass.Name)` → 执行 `ExecuteUpload` 或 `CopyExecute` → `PopDebugGroup()`。
   - **`RDGPassKind::Compute`（1 或多个子 Pass 合并组）**：
     - 调用 1 次 `auto encoder = command.BeginComputePass();`；
     - 依次遍历组内每个子 Pass `passIndex`：
       - `execution.Pass = &pass;`
       - `command.PushDebugGroup(pass.Name);`
       - `bool ok = pass.ComputeExecute.Get()(pass.Callback.Get(), ctx);`
       - `command.PopDebugGroup();`
       - 若 `!ok`，立即 `command.EndComputePass(encoder.Release());` 并报错返回 `false`；
     - 组内全部子 Pass 完成后，调用 1 次 `command.EndComputePass(encoder.Release());`。
   - **`RDGPassKind::Raster`（1 或多个子 Pass 合并组）**：
     - 以首 Pass `firstPass = _data.Passes[group.Passes.front()]` 和末 Pass `lastPass = _data.Passes[group.Passes.back()]` 折叠构造 `RenderPassBeginDescriptor`：
       - 每个颜色槽 $i$：使用 `firstPass.Colors[i]` 的 `View`（或 `ResolveTextureView`）、`Load` 与 `Clear`，以及 **`lastPass.Colors[i].Store`**；
       - 若有深度/模板附件：使用 `firstPass.Depth` 的 `View`、`DepthLoad`、`StencilLoad`、`Clear`，以及 **`lastPass.Depth->DepthStore`** 与 **`lastPass.Depth->StencilStore`**；
       - `AllowUavWrites`：若组内**任意**子 Pass 含有 UAV 写访问则为 `true`；
       - `Name`：取 `firstPass.Name`。
     - 调用 1 次 `auto encoder = command.BeginRenderPass(beginDesc);`；
     - 依次遍历组内每个子 Pass `passIndex`：
       - `execution.Pass = &pass;`
       - `command.PushDebugGroup(pass.Name);`
       - **`encoder->SetStencilReference(0);`**（重置模板参考值初态为 0，隔离前序子 Pass 的修改）；
       - `bool ok = pass.RasterExecute.Get()(pass.Callback.Get(), ctx);`
       - `command.PopDebugGroup();`
       - 若 `!ok`，立即 `command.EndRenderPass(encoder.Release());` 并报错返回 `false`；
     - 组内全部子 Pass 完成后，调用 1 次 `command.EndRenderPass(encoder.Release());`。

3. **图末 Epilogue 屏障批量下发**：
   - 收集所有存活导入资源的 `FinalState` 转换屏障到单个 `epilogueBarriers` 数组，若非空则单次调用 `command.ResourceBarrier(epilogueBarriers)`；
   - 将存活瞬态资源的最终 `CurrentState` 提交回 `pool.CommitTextureState` / `CommitBufferState`。

---

## 四、性能开销与收益量化评估

设一帧 RDG 声明的 Pass 数为 $V$，资源数为 $R$，Pass 对资源的总访问声明次数为 $A$（典型实时渲染帧：$V \approx 20 \sim 60$，$R \approx 20 \sim 50$，$A \approx 80 \sim 300$；重度管线：$V \approx 150$，$A \approx 600$）。

### 1. 编译期（`RDGBuilder::Compile`）新增 CPU / 内存开销（基于图规模的理论量级估算）

| 新增步骤 | 算法复杂度 | 渐近上界说明 | 典型帧（50 Pass / 200 Access）理论耗时量级 |
|---|---|---|---|
| **WAR 边构建**（维护 `Readers`） | $\Theta(A)$ | `AddAccess` 已保证单 Pass 内资源唯一，`readers.push_back(index)` 严格递增无需查重；每次写访问遍历 `Readers` 并以 `seenStamp` $O(1)$ 去重连边后立即 `clear()`。每个只读访问至多被消费一次，故 $|E_{\text{WAR}}| \le A_{\text{read}} < A$。 | **$< 1.5\ \mu\text{s}$** |
| **Kahn 贪心拓扑重排** | $\Theta(V_{\text{alive}} \cdot \overline{|Q|} + E_{\text{alive}})$ | $E_{\text{alive}} \le 2A$；渲染图受附件链与数据流约束，任意时刻入度为 0 的就绪候选集 $|Q|$ 通常仅 $1 \sim 8$ 个 Pass，在 $|Q|$ 中线性挑选最高优先级节点的常数极小。 | **$1.5 \sim 4.0\ \mu\text{s}$** |
| **合并兼容性预判** | $\Theta(V_{\text{alive}} \cdot \overline{|Q|} \cdot \bar{a})$ | 仅在当前存在打开的合并段时，对 `ready` 中同类型候选 Pass 检查颜色槽（$\le 8$）、深度槽（$1$）及该 Pass 的 `Accesses`（$\bar{a} \approx 4$）是否与 `openGroupAccesses` 冲突。 | **$0.5 \sim 1.5\ \mu\text{s}$** |
| **新增内存占用** | $\Theta(V + R + A)$ | 约数百个 `uint32_t` 下标（总计 **$2 \sim 4\text{ KiB}$**），完全驻留在 CPU L1 Data Cache（$32 \sim 48\text{ KiB}$）内。 | 帧末随 Slot Arena 统一回收 |

- **`Compile()` 新增 CPU 理论耗时量级**：常规帧（$30 \sim 60$ 个 Pass）约 **$2 \sim 7\ \mu\text{s}$**；重度帧（$150$ 个 Pass）约 **$15 \sim 25\ \mu\text{s}$**。

### 2. 执行期（`CompiledRDG::Execute`）与 GPU 侧净收益对冲

1. **CPU 录制期节省（直接抵消甚至超过 `Compile` 新增耗时）**：
   - **省去重复 Encoder 分配、RHI 校验与原生 RenderPass 开关**：每次 `BeginRenderPass` / `EndRenderPass` 涉及 `unique_ptr<GraphicsCommandEncoder>` 分配、RHI 校验层 [`ValidateRenderScope`](../../modules/render/src/rhi_validation.cpp) 与附件 View 查找/Pin、Vulkan `VkRenderPass`/`VkFramebuffer` 查找与 `vkCmdBeginRenderPass2`（或 D3D12 `BeginRenderPass`）。每合并掉 1 个 Raster Pass 即可省下约 **$2 \sim 8\ \mu\text{s}$** 的 CPU 录制时间。在教学样例中（`Color` + `Outline` + `Transparent` 合并为 1 个原生 RenderPass），仅减少 2 次 RenderPass 开关即可对冲编译期拓扑重排的全部开销。
   - **批量 `ResourceBarrier` 下发**：将一帧数十次单元素 `Command.ResourceBarrier(span{&barrier, 1})` 虚调用压缩为按合并组的少量批量调用。
2. **GPU 执行期节省（核心收益）**：
   - 消除合并 Raster Pass 之间原本强制插入的 `RenderTarget → RenderTarget` 与 `DepthWrite → DepthWrite` 屏障，避免 GPU 在同一帧缓冲的连续绘制阶段（如 Opaque → Outline → Transparent）之间反复排空光栅化流水线和 ROP / Depth Cache。

---

## 五、实施文件清单与验收条件

| 文件路径 | 拟改动内容 |
|---|---|
| `modules/runtime/src/rdg.cpp` | 1. 为 `RDGTexture` / `RDGBuffer` 增加 `Readers`，为 `RDGPass` 区分 `DataDependencies` 与 `OrderDependencies`；<br/>2. 在 `Compile()` 中实现 WAR 边收集、仅沿 `DataDependencies` 裁剪（保留 `Output` 资源直接标记 `Alive`）、基于 `openGroupAccesses` 组入口屏障前移（Hoisting）的 Kahn 贪心拓扑重排与 `RDGExecutionGroup` 合并分组，并严格按原始声明顺序生成 `AliveMeshPasses`；<br/>3. 在 `Execute()` 中实现按 `RDGExecutionGroup` 汇总全组子 Pass 首次访问资源并批量下发 `ResourceBarrier`、Raster Pass Load/Store 折叠与子 Pass `SetStencilReference(0)` 重置、Compute Pass 合并及图末批量 Epilogue 屏障 |
| `modules/runtime/tests/test_rdg.cpp` | 新增单元测试覆盖：<br/>1. **同附件 Raster Pass 合并、折叠与组入口屏障前移（Hoisting）**：`Color(Clear/Store，读 UBO0)` + `Outline(Load/Store，首次读 UBO1)` + `Transparent(Load/Discard，读 UBO0)` 合并为单次 `BeginRenderPass(Clear/Discard)`，`UBO1` 的 `HostWrite → CBuffer` 屏障前移至组入口批量下发，中间零 `RenderTarget → RenderTarget` / `DepthWrite → DepthWrite` 屏障，且每个子 Pass 入口重置 `StencilReference = 0`；<br/>2. **穿插无关 Upload / Compute 时的跨节点重排合并与 `GetAliveMeshPasses()` 保序**：在两个可合并 Raster Pass 之间声明无关的 `UploadBuffer` 或独立 `Compute` Pass（或另一套附件的 Raster Pass），重排将其移出并合并同附件 Raster Pass，同时验证 `GetAliveMeshPasses()` 与 `ExtractMeshPass` 仍严格保持原始声明顺序；<br/>3. **WAR（先读后写）防越界重排、死 Reader 裁剪与同前驱 WAR+RAW 共存**：Pass A 读资源 X，Pass B 用 `Clear` 或 `Write` 覆写资源 X；当 A 与 B 均存活时 B 绝不被重排到 A 之前；当 A 是无输出的死分支时，B 的 WAR 边不会错误保活 A；当 Pass A 同时「读 X + 写 Y」而 Pass B 按「先声明写 X（产生对 A 的 WAR `Order` 边）、后声明读 Y（产生对 A 的 RAW `Data` 边）」顺序声明时，验证 `Order` 边不会抑制 `Data` 边，Pass A 仍被正确保活；<br/>4. **不可合并边界拒绝**：深度状态从 `DepthWrite` 变为 `DepthRead`、中间含 `LoadAction::Clear`、或两个子 Pass 对同一非附件资源存在组内写后读/写或不同状态访问时，必须切分为独立 `BeginRenderPass` 并正确插入 Barrier；<br/>5. **相邻零冲突 Compute Pass 合并（含独立资源屏障前移）与有组内 UAV 依赖切分**：无组内冲突的相邻 Compute Pass 共享单次 `BeginComputePass`，存在组内 `UAV → UAV` 写依赖时自动切分并插入 `BarrierUavDescriptor`；<br/>6. **批量 Barrier 聚合验证**：验证同一执行组进入前的多个 Buffer/Texture 转换合并为单次 `CommandBuffer::ResourceBarrier` 调用 |
| `docs/architecture/runtime.md` | 代码实施落地时同步更新 `## RDG 与 RenderGraphFramePipeline` 正文契约 |

### 验收条件
1. 构建并通过 `test_radray_runtime` 全部单元测试（Debug / Release）；
2. 运行 `radray_mesh_drawing_sample --backend d3d12` 与 `--backend vulkan`，确认 `Color`、`Outline`、`Transparent` 在单次原生 RenderPass 内完成录制，输出图像与基线一致，D3D12 Debug Layer 与 Vulkan Synchronization Validation 零报错；
3. 运行 `python tools/check_docs.py` 与 `git diff --check` 零错误。
