# Pragmatic Render Graph (PRG) 阶段三实现计划：资源屏障与内存别名分配

> 此文档基于 Phase 2（宏观拓扑、死代码剔除与生命周期采集）的完备实现。本阶段的主要目标是让 Render Graph 能够追踪子资源的访问历史，以此下发精确的资源屏障（Barrier），并在架构上为未来的动态内存分配（Aliasing Allocator）预留接入点。

---

## 3.1 核心目标 

1. **自动前置屏障生成（Barrier Generation）**：通过前置拓扑采集的原子子资源区域（Atomic Subresource Range）的历史，在每个执行的 Pass 前，构建从旧状态到新状态的转移映射表。支持同类型写屏障冲刷（UAV Barrier / Memory Barrier）。
2. **多片段屏障联调合并（Barrier Aggregation）**：鉴于原子细分会导致连续区间的 Mips 或 Layers 被打散产生多条细碎的转换屏障，框架应该在输出前自动聚合拥有相同参数且相邻的子区间，大幅降低给下层图形 API (D3D12/Vulkan) 派发的开销。
3. **外部资源初始状态注入**：允许从外部导入持久化或上一帧传入的资源提前带有 `InitialMode`（如从 Swapchain 返回的内容默认为 Present 状态），保证计算首道 Barrier 时不会从 `Unknown` 起步。
4. **内存别名预留桩（Aliasing Stub）**：在图编译流程中占位。当前所有瞬时资源（Transient Resources）的生命周期已推导就绪，计划在此环节留下回调函数或者接口位，方便以后做基于堆的复用贪心着色切分。

---

## 3.2 任务清单与代码修改指引

### Step 1: 补充基础数据结构 (render_graph.h)
需要在 `radray::render` 命名空间中定义与状态转换强相关的结构组件。

*   [ ] 设计 `RGBarrier` 结构：包含前置状态、请求状态、目标资源句柄，及受影响的具体访问范围。
    ```cpp
    struct RGBarrier {
        RGResourceHandle Handle{};
        RGSubresourceRange Range{};
        RGAccessMode StateBefore{RGAccessMode::Unknown};
        RGAccessMode StateAfter{RGAccessMode::Unknown};
    };
    ```
*   [ ] 更新 `VirtualResource`：在成员中添个选填字段 `RGAccessMode InitialMode = RGAccessMode::Unknown;` 。
*   [ ] 在 `RGGraphBuilder` 对外导入的方法（如 `ImportExternalTexture`, `ImportExternalBuffer`）中加入一个接受 `InitialMode` 的重载或者参数，并允许它赋值到新建的 VirtualResource 中。
*   [ ] 在 Phase 3 的结果产出结构 `CompiledGraph` 中，引入占位属性：
    ```cpp
    struct RGResourceAllocation {
        // (尚未实现内容) 用于在后续存储对应资源的所绑定池及 Offset
    };
    
    // ...
    // 在旧版结构的基础上附加两行：
    vector<vector<RGBarrier>> PassBarriers{}; // 大小需与 SortedPasses.size() 并排同步对齐
    vector<RGResourceAllocation> Allocations{};// 留作 Phase3 下半程 Aliasing 的占位
    ```

### Step 2: 在编译管线中实施按序屏障发生器 (render_graph.cpp)
在 `RGGraphBuilder::Compile()` 返回 `compiled` 前，加装 Phase 3 处理管线逻辑：
*   [ ] **初始化状态机查表**：为所有原子资源块 (`atomicRangesByResource`) 生成一张用于存放其即时访问状态的追踪表。对于导入进来的资源，如果它们设置了 `InitialMode` 则填为 `InitialMode` 作为预定前置。
*   [ ] **内存池分配 Stub (留白)**：空出用于之后给 `compiled.Allocations` 的生成范围赋值块。
*   [ ] **追踪计算 Edge**：按照已排好序的拓扑数组 `SortedPasses` 迭代遍历所有节点的读流/写流。将边（Edge）的所需范围与原子区块表作逐重叠比对，得出对应部分之前的 `StateBefore`。
*   [ ] **判断是否生成 Barrier 的断言逻辑**：
    *   两者的 `Mode` 不同（如 `StorageRead` 转交 `ColorAttachmentWrite`）。
    *   两者 `Mode` 完全一致且目标 Mode 支持写操作（如连续发生两次 `StorageWrite` 必须产出一条 `Old=StorageWrite, New=StorageWrite` 以代表 Cache 冲刷/ UAV 同步）。
    *   如果符合以上任意，推入对应 Pass 序号队列 `PassBarriers[i]` 并更新当前状态记录表。

### Step 3: 加装相邻屏障块自动聚拢优化 (Barrier Merging)
实现一个方法专门处理针对当前 `passBarriers[i]` 收集完毕的列表，遍历处理并进行向上区间合并算法。
*   [ ] 按照句柄 -> Type -> 之前模式 -> 目标模式进行粗略分类。
*   [ ] 对于被拆碎且数值上彼此临近的 Buffer 片段 (比如 `[0, 50]` 与 `[50, 100]`) 聚合为 `[0, 100]`。对于纹理的贴图区块（如连续 Mips、连续 ArrayLayers）也尝试逆推融合。
*   [ ] 将合并后的大区块用以覆盖输出。

### Step 4: 增强调试导出
*   [ ] 在 `RGGraphBuilder::DumpCompiledGraph` 末端输出 `PassBarriers` 日志：包括每一层 Pass 执行前，系统注入的额外 Barrier 清单，呈现 `[Pass N] Barrier: handle=?, Range=?, From -> To` 的文字输出。

---

## 3.3 验收与测试用例规划 

编写并新建对应的 G-Test 验证机制，确保图内发出的指令既不缺失也无多余。重点验证点如下：

1.  **Test: 初见 Barrier（测试 ImportState）**：当用户用 `StorageRead` 的 `InitialMode` 导入外部纹理且将其写作 `ColorAttachmentWrite` 时，检查是否在第一处 `PassBarriers` 里得到了对应确切的首个转换。
2.  **Test: 同操作内存刷新（测试 WAW Flush）**：构造一个连续的 PassA 跑 `StorageWrite` 和 PassB 再跑同一资源的 `StorageWrite`，检查是否有生成一条同类之间的同步 Barrier。
3.  **Test: 相接碎片化还原（测试合并聚拢）**：分两个相邻操作分别读 Mip0 和 Mip1，后续在一个新 Pass 里一口气作为写图，查验该 Pass 收到的屏障**只包含一条覆盖 [0..2) 范围** 的指令，而不是分散成两条指令发出。

---

## 下一步建议

完成文档审视后，您可直接通知 AI 按此阶段拆解的清单立刻实行 **Step 1 (基本数据结构打底)** 与 **Step 2 (状态机推导迭代)** 到现有的源代码中。