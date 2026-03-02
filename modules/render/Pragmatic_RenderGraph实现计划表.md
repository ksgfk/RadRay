# Pragmatic Render Graph (PRG) 落地实现计划表

> 本计划表基于“宏观图拓扑 + 微观指令排程 + Shader反射辅助”的混合粒度架构。
> 旨在分阶段、稳健地完成一款具备生产级可用性（抗击 Bindless 架构与 GPU-Driven 测试）的现代 Render Graph。

---

## 阶段一：基础设施与 Shader 反射层 (Weeks 1-2)

**目标：** 不碰图形 API 的前提下，实现一套独立、无副作用的 Shader Metadata 提取系统以及虚拟管线描述。

### 1.1 核心任务
*   [ ] 集成 `SPIRV-Cross` 或 DirectXShaderCompiler (DXC) 的反射模块。
*   [ ] 设计 `ShaderReflectionData` 结构，能够序列化缓存下列信息：
    *   Descriptor Bindings (Set, Binding, Type, 读/写语义)。
    *   Bindless 标记（检测到数组描述符）。
    *   Push Constants 布局。
    *   Input Attachment 支持点（识别 `[[vk::input_attachment]]` 或 SubpassData 类型）。
*   [ ] 实现 `Pipeline State Object (PSO) 缓存与工厂`：基于反射和用户传入的 Vertex/Blend 状态，自动推导出完整 PSO。

### 1.2 潜在雷区与解决方案
*   **雷区：反射提取得太迟导致主线程卡顿。** 如果每次发起 Pass 录制才去编 Shader 读反射，100 个 Pass 绝对会导致主线程 Stall。
    *   *Solution:* **离线烹饪 (Offline Cooking)** 或 **异步预热**。PSO 与 Shader 反射数据必须在游戏/引擎 Load 阶段提前生成，PRG 运行时只读取反序列化后的只读元数据结构。
*   **雷区：如何准确识别 Bindless Array？**
    *   *Solution:* 检查反射出的 Descriptor Array 长度。Vulkan 中通常为无界（`[]` 或极大值），标记此 PSO 具有 `RequiresBindlessSync` 属性。

---

## 阶段二：宏观 Pass 图拓扑与生命周期 (Weeks 3-5)

**目标：** 在不接入 Vulkan/D3D12 的前提下，完成 Render Graph 编译内核：
1. 录制期：稳定收集 `read/write` 依赖。
2. 编译期：执行 `DCE -> Topo Sort -> Lifetime`。
3. 输出：可供阶段三直接消费的 `sorted_passes + resource_lifetimes`。

**当前仓库落点：**
*   头文件：`modules/render/include/radray/render/render_graph.h`
*   实现：`modules/render/src/render_graph.cpp`
*   测试：新增 `tests/test_render_graph_phase2/`

### 2.1 范围与非目标
*   **范围内：** `VirtualResource`、`RGPassNode`、依赖边建立、整 Pass DCE、拓扑排序、生命周期采集、日志导出。
*   **范围外：** 显存别名分配、真实 API Barrier 发射、Subpass 合并、反射自动依赖注入。

### 2.2 三周里程碑与交付物

| 周次 | 里程碑 | 具体任务 | 交付物 | 验收标准 |
|---|---|---|---|---|
| Week 3 | M1：数据模型与录制接口 | 定义 `RGResourceHandle`/`VirtualResource`/`RGPassNode`/`ResourceAccessEdge`；补齐 `RGPassBuilder::read/write` 与 `RGGraphBuilder::add_pass/create_*` | 可编译的图录制骨架，能生成原始 Pass/Resource 列表 | 构建通过；能打印每个 Pass 的读写资源清单 |
| Week 4 | M2：图裁剪与排序 | 实现 Sink 驱动 DCE（逆向 BFS）；构建 Producer->Consumer 邻接表；实现 Kahn 拓扑排序与环检测 | `compile()` 产出裁剪后的有序 Pass 列表 | 无用 Pass 被剔除；出现环时返回明确错误与残图摘要 |
| Week 5 | M3：生命周期与诊断 | 基于 `sorted_passes` 计算 `[first,last]`；处理 `Persistent/External` 资源；导出 timeline 文本 | `CompiledGraph`（含 lifetimes）+ 调试日志接口 | 生命周期区间正确；Persistent 不被误剔除；关键测试全部通过 |

### 2.3 实现任务清单（按代码顺序）

*   [ ] 在 `render_graph.h` 增加阶段二核心类型：
    *   `enum class RGResourceType` / `enum class RGResourceFlags`
    *   `struct VirtualResourceDesc` / `struct VirtualResource`
    *   `enum class RGAccessMode` / `struct ResourceAccessEdge`
    *   `struct RGPassNode` / `struct ResourceLifetime` / `struct CompiledGraph`
*   [ ] 在 `RGGraphBuilder` 增加录制 API：
    *   `create_texture/create_buffer/import_external`
    *   `add_pass(name, queue_type)` 返回 `RGPassBuilder`
    *   `mark_output/force_retain/export_temporal`（仅记录标记，不做后端行为）
*   [ ] 在 `render_graph.cpp` 实现编译流水线：
    *   `collect_roots()`：收集 Backbuffer、force retain、Persistent 输出
    *   `run_dce()`：逆向 BFS 标记 `RGPassNode::is_alive`
    *   `build_adjacency()`：根据读写关系建立图边和入度
    *   `run_topo_sort()`：Kahn + 稳定排序策略（默认按 Pass 注册顺序）
    *   `calc_lifetimes()`：单次线性扫描计算 `[first,last]`
*   [ ] 新增阶段二测试集 `tests/test_render_graph_phase2/test_render_graph_phase2.cpp`：
    *   `dce_culls_disconnected_passes`
    *   `persistent_resource_survives_dce`
    *   `topo_sort_detects_cycle`
    *   `lifetime_interval_matches_usage`
*   [ ] 增加调试导出（文本即可）：Pass 顺序、每个资源生命周期、被裁剪 Pass 列表。

### 2.4 风险与控制
*   **风险：环依赖误报/漏报。**
    *   控制：在 `run_topo_sort()` 后对输出数量与 alive 节点数量做一致性断言，不一致即判定循环依赖。
*   **风险：Temporal/Persistent 被 DCE 误删。**
    *   控制：`collect_roots()` 中将 `Persistent` 资源强制纳入 Sink 集合，并加单测锁定行为。
*   **风险：生命周期错误影响阶段三显存复用。**
    *   控制：`calc_lifetimes()` 仅依赖 `sorted_passes`，并对“只写未读但被强保留”资源将 `last` 扩展到末尾 Pass。

---

## 阶段三：存储别名 (Memory Aliasing) 与 Barrier 分配 (Weeks 6-7)

**目标：** 基于生命周期分配实际显存复用；并为所有资源的转换计算出正确的 Barrier。

### 3.1 核心任务
*   [ ] 实现基于 Greedy Interval / 图着色的**显存分配器**：对于生命周期 `[start, end]` 不重叠的 `Transient` 资源，将其映射到同一块底层 Heap Offset (`vkBindImageMemory` / D3D12 `PlacedResource`)。
*   [ ] 实现**同步边界计算 (Split Barrier / Event)**：
    *   扫描每个资源的使用史。
    *   在产生 RAW/WAW 竞争的前一个 Pass 的 `execute()` 回调结尾，推入 `SignalEvent` / `CmdSetEvent` 命令。
    *   在读取该资源的当前 Pass 开始前，推入 `WaitEvent` 配合对应的 `PipelineStageFlags` 和 `AccessFlags`。

### 3.2 潜在雷区与解决方案
*   **雷区：过度复用内存导致 GPU Cache 无效化甚至崩溃。** 尤其是 RenderTexture 跟 StorageBuffer 强行别名，某些老驱动会有对齐限制或清理要求。
    *   *Solution:* 严格对齐（比如强制使用 64KB/4MB D3D12 Page 边界）。并在 Memory Aliasing Reuse 被触发时，在原前置资源的最后一个 Pass 必须下发严谨的 Invalid Access Barrier 冲刷 Cache。
*   **雷区：Barrier 插入导致同步死锁或异步掩盖退化。** Wait 如果写得太全，等于全局流水线刷新。
    *   *Solution:* 严格收紧 Pipeline Stages，比如由 Compute 写入的 Buffer 被 Fragment 读取， Wait Stage 仅为 `FragmentShader`。使用 Vulkan 1.3 `VK_KHR_synchronization2` 实现最纯正的管线分离屏障。

---

## 阶段四：Shader 自动推导配合与显式 Subpass 组 (Weeks 8-9)

**目标：** 放开“隐式”录制 API，将反射元数据同拓扑系统接轨。同时支持基于手动域的内存节约机制。

### 4.1 核心任务
*   [ ] 升级 Pass 构建器 API：提供 `exec([=](CommandList& cmd){ cmd.bind_pipeline(pso); cmd.bind(res); })`。
*   [ ] 拦截 `cmd.bind`，查找步骤一生成的 PSO Metadata，在内部往当前图节点强插依赖 `Reads/Writes` 边。
*   [ ] 实现 `subpass_group` API 隔离区：
    *   校验内层中带有 `SampledRead` 的情况并发出 Asserts。
    *   将纯组内中性的资源（如 GBuffer）的 `StoreOp` 置为 `StoreOp::DontCare`。
*   [ ] 在后端翻译层生成带有正确 `VkSubpassDependency` 的单 `VkRenderPass` 操作。

### 4.2 潜在雷区与解决方案
*   **雷区：用户的条件分支 (If-Else) 造成的反射推导失效。** 如果 `execute` 回合内部用户写了 `if (cond) cmd.bind(tex_A) else cmd.bind(tex_B)`，由于 Graph 是前置编译的（要求录制时提供完整依赖），等到 `execute` 发生时才注入依赖就**来不及**了（Graph早编译完了）。
    *   *Solution: 绝对不可逾越的规则约定。* 用户的 `read/write` 可以通过反射补充，但必须在**闭包之外（Setup 阶段）**声明完毕。即：`Pass::add_pass("A").read(tex_A).read(tex_B)` 必不可少；又或者提供类似 RDG 的 `ShaderParameters` 结构体，用户填入结构体后，系统在编译期先扒取结构体的所有的句柄进行依赖建立，后将该结构体塞入 `execute`。

---

## 阶段五：微观 Pass 内部排程调度与 GPU-Driven 适配 (Weeks 10-11)

**目标：** 榨干单 Pass 内部的 Draw/Dispatch 效率，兼容 Bindless 和 Indirect。

### 5.1 核心任务
*   [ ] 将用户的普通 `cmd.draw()` 和 `cmd.bind()` 封装为虚拟命令而不是直接调用底层。
*   [ ] 为每条虚拟绘制指令生成 `64-bit Radix Sort Key`。规则：`[PSO ID_14bit | DescriptorSet ID_16bit | Depth/Blend_32bit]`。
*   [ ] 实现 Pass 内的一键排序去重 (State Dedup)：连续的同 PSO 绑定剔除。
*   [ ] 强化 `IndirectArgs` 类型的虚拟资源，确保涉及 Indirect 的 Compute pass （生产者）和 Graphic pass（消费者）屏障绝对安全不漏。

### 5.2 潜在雷区与解决方案
*   **雷区：间接绘制参数中的最大实例数未知。** Indirect Draw 会被 CPU 传递给 GPU，但显卡还需要去执行剔除。如果不配合 `CountBuffer`（限制界限），有极大驱动挂掉的风险。
    *   *Solution:* 设计 `IndirectBuffer` 时强制要求用户伴随传入 `CountBuffer` 引用。并确保在生成的 Graph Barrier 中，两者一并进入 `IndirectCommandRead` 内存状态。
*   **雷区：Bindless 强刷缓存过猛。** 如果碰到 `BindlessSync` 每次都给大 Barrier。
    *   *Solution:* 限制 Bindless 刷新在 Pass 大类上。如 `Mesh Rendering` 和 `Deferred Lighting` 之间集中刷一次总的 Bindless 池即可，而非次次切换都刷。

---

## 阶段六：多后端翻译器与终体验收 (Weeks 12-13)

**目标：** 接通真实图形 API 发射；真机 Profiling。

### 6.1 核心任务
*   [ ] 编写 `VulkanBackend`：吃吃透 `vkCmdPipelineBarrier2`、`VkRenderPass` (兼容 Subpass)、`vkCmdDrawIndirect`。
*   [ ] 编写 `D3D12Backend`：对应 `ResourceBarrier`、`ID3D12GraphicsCommandList::ExecuteIndirect`。
*   [ ] 集成 CPU/GPU 耗时分析器（例如 `Tracy Profiler` 或直接通过 Query Time Timestamp）。检查编译开销是否稳定在 < 0.2ms。
*   [ ] 并行化 Graph 录制：允许独立线程同时调用 `add_pass`（只通过 Mutex 保护总控 DAG 节点插入操作）。

### 6.2 潜在雷区与解决方案
*   **雷区：多队列（Compute Queue / Graphics Queue）导致 Queue Ownership Transfer 出错（常见于 Vulkan）。** 并行 Compute 如果将资源交给 Graphics，必须经历 `RELEASE/ACQUIRE` 操作。
    *   *Solution:* `VirtualResource` 内部不仅维护 `[first, last]` 生命周期，还要维护当前属于的 `QueueFamily` 状态流转机。在翻译翻译出跨队列边时，后端强制抛出特殊的 Ownership Transfer Barrier，禁止逃课。
