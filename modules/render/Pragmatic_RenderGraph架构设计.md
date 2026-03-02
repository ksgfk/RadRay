# Pragmatic Render Graph：宏观与微观的混合粒度架构设计

> 核心思想：摒弃纯理论的“全局指令级IR编译”，引入**“宏观维图，微观排程” (Macro for Topology, Micro for Execution)** 的混合架构。
> 在保留 Shader 自动反射、极简 API、内存复用等优良特性的前提下，完美融合现代 Bindless 架构与 GPU-Driven 管线。

---

## 一、架构哲学的破与立

在此前的纯 IR 指令流 Render Graph 中，我们试图让 CPU 扮演“全知全能的上帝”，通过扁平化十万级别的 DrawCall 来推导 Subpass、提取全局 Barrier。但这在工业界引发了严重的**复杂度爆炸**和**硬件启发式崩溃**。

新架构采用**混合粒度**：
1. **Pass 级别（宏观）**：负责拓扑排序、生命周期分析（Memory Aliasing）、Barrier 插入、队列调度（Async Compute/Transfer）。保持节点数在 100~300 个，确保 CPU 编译耗时低于 `0.15ms`。
2. **Draw/Dispatch 级别（微观）**：仅在各个独立 Pass **内部**运行。仅用于 PSO 状态排序、Multi-Draw-Indirect 合并，绝不跨 Pass 污染依赖图。

---

## 二、六大核心机制设计解析

### 2.1 融合 Shader 反射的“半隐式”依赖追踪

纯手动写 `builder.read/write` 很容易出错，且增加用户心智负担。我们利用 Shader 反射，但不迷信反射。

* **常规资源（Render Targets / 具名 Buffer / 独立 Texture）**：
  在 Pass 录制时，只需要绑定 PSO，系统提取 SPIR-V/DXIL 中的绑定点信息。当用户 `pass.bind(0, my_texture)` 时，内部自动根据反射出的 `SRV` 或 `UAV` 属性向 Graph 注册 `Read` 或 `Write` 依赖。
* **妥协 Bindless（无绑定资源池）**：
  对于 `Texture2D BindlessArray[]`，不进行对象级追踪。引入一个全局的 `graph.require_bindless_sync()`。Graph 仅保证在有资源更新进 Bindless 堆时，向后续所有的 Draw 插入合适的粗粒度 `Execution Barrier`。

### 2.2 显式声明与隐式校验结合的 Subpass

**摒弃危险的“全自动 Subpass 推导”**，转为**“用户显式开启 + 编译器安全校验”**。通过人工隔离可能导致 Tile Memory 溢出的超大型 Pass。

* **API 设计**：用户显式调用 `graph.begin_subpass_group()` 将 GBuffer、Decal 和 Deferred Light 框在一起。
* **编译器行为**：
  1. 通过反射校验：如果在 Subpass 组中后续 Pass 试图通过 `SampledRead`（非像素局部）读取前置 Pass 的内容，直接报错中断，而非隐式打断去走 DRAM。
  2. 自动 Load/Store 分析：Graph 仍会根据资源是否在 Subpass 组外部被使用，自动将内部 Target 设置为 `StoreOp::DontCare` 和 `Memoryless/Transient`，自动达成带宽归零。

### 2.3 粗粒度但精准的 Split Barrier 策略

不再于每条指令后插入零碎的 Pipeline Barrier 打断 GPU Wavefront，而是：
1. **Pass 边界汇聚**：收集两个连通的 Pass 之间所有的资源屏障冲突。
2. **Split-Barrier（栅栏分离）**：
   在资源最后一次写入所在的 Pass **结束时** 发出 `vkCmdSetEvent / Signal` （尽早开始 Transition）。
   在资源第一次被读所在的 Pass **开始前** 发起 `vkCmdWaitEvent / Wait` （尽晚等待）。这隐式实现了异步掩盖。

### 2.4 拥抱 GPU-Driven：禁止跨界的 DCE

在纯 CPU IR 模型中，DCE（死代码消除）会因为不知道 GPU 生成的 Indirect Argument 数量而失效。

* **新架构方案**：将 `IndirectBuffer` 提升为一等公民资源。
  任何输出了 `IndirectBuffer` 的 Compute Pass，以及消费了该 Buffer 的 Draw Pass，其依赖连线是**坚不可摧**的。DCE 仅在“最终输出的 RenderTarget (如 Backbuffer) 完全未连接”时进行基于整树的 Pass 裁剪，彻底放弃针对单条 Draw 指令的 DCE 猜测。

### 2.5 吸收 Kajiya 大杀器：Temporal 资源的一等公民待遇

在传统架构中（包含我们的上一版设计），“跨帧资源”（比如 TAA 的 History Buffer、光照缓存的上一帧结果）通常被作为特例（`External Resource`）处理。这就导致它们丢失了 Graph 的屏障管理和 Layout 追踪保护。

我们在混合架构中正式引入 Temporal State Machine：
* 跨帧资源不再是 External，图在结束前调用 `graph.export_temporal(res)`。
* 编译器自动缓存该资源在**本帧结束时的 Final Access State**（例如 `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE`）。
* 下一帧通过 `graph.import_temporal(res)` 时，系统能够完美衔接上一个状态，**补全缺失的跨帧首个 Barrier 转换**。这极大降低了编写 Temporal 算法时常见的“画面闪烁/老数据同步错误”。

### 2.6 跨队列协同（Async Compute / Transfer）

IR 架构很难良好表达异构 Queue。Pass-based 架构则游刃有余。
系统提供 `QueueType` 标签，图编译器自动发现两个无依赖的 Compute Pass 和 Graphics Pass，分配到并行的 Command Buffer 中，并在适当的交汇点自动插入 Timeline Semaphore 进行 GPU 端粗粒度同步。

### 2.7 C# 级别编译期屏障：强类型的 Context 注入 (致敬 Unity SRP)

虽然我们底层采用 C++，但我们在 API 约束上吸收 Unity 6 SRP 对于 CommandBuffer 的隔离手段，从**类型系统**截断非法调用。

* 设计独立接口的 Context，例如：
  `RasterGraphContext` (只包含 Draw API，没有 Dispatch)。
  `ComputeGraphContext` (只包含 Dispatch API，没有 Draw)。
* 用户指定 `QueueType` 时，回调的签名字段会被强校验约束到对应的 Context 上：
  避免出现：在一个 Compute Pass （异步计算阶段）内部，引擎开发人员随手写下了一个拉起并执行一个管线状态为绑定的光栅化三角形的不合矩操作。这些会在 Compile-Time 而不是 Run-Time (驱动挂死时) 就直接报错解决。

---

## 三、用户侧录制范式（API 演示）

新的 API 设计极度清爽且性能可靠：

```cpp
// 1. 初始化绑定全局 Bindless 资源
auto bindless_pool = graph.create_bindless_pool();

// 2. 传统 Compute 阶段 (例如 GPU Cull)
graph.add_compute_pass("GPU_Culling", QueueType::AsyncCompute)
    .read(scene_bvh)
    .write(indirect_draw_args) // 注册为第一类依赖资源
    .execute([](ComputeGraphContext& cmd, PassResources& res) {
        cmd.bind_pipeline(cull_pso); // 反射出 UAV，自动验证与 write 匹配
        cmd.dispatch(1024, 1, 1);
    });

// 3. 显式 Subpass 组（取代危险的全自动合并）
auto subpass_group = graph.begin_subpass_group("Deferred_Pipeline", {
    .width = 1920, .height = 1080 
});

// 3.1 组内 Pass A: GBuffer
subpass_group.add_raster_pass("GBuffer")
    .read_indirect(indirect_draw_args)
    .write(albedo, Format::RGBA8)
    .write(depth, Format::D32)
    .execute([](RasterGraphContext& cmd, PassResources& res) {
        cmd.bind_pipeline(gbuffer_pso);
        // 微观级别：cmd 内部收集所有 Draw，在 Pass 结束时进行自动的 State Sort 和 MDI 合并
        cmd.draw_indexed_indirect(res.get(indirect_draw_args));
    });

// 3.2 组内 Pass B: Deferred Lighting
subpass_group.add_raster_pass("Lighting")
    .read_subpass(albedo) // 必须用专用的 Subpass 读标记
    .read_subpass(depth)  // 编译器通过反射校验 PSO 是否真配有 [[vk::input_attachment]]
    .write(hdr_color)
    .execute([](RasterGraphContext& cmd, PassResources& res) {
        cmd.bind_pipeline(light_pso);
        cmd.bind_bindless(bindless_pool); // 声明使用全局 Bindless
        cmd.draw(3, 1, 0, 0); // Fullscreen triangle
    });

graph.end_subpass_group();

// 编译系统：
// 1. 发现 albedo 仅在 subpass 内流转，外部无消费 → 配置 Memoryless 内存，0带宽。
// 2. 发现 indirect_args 存在跨队列跨 Pass 依赖 → 在 Cull 结束后抛出 Event，GBuffer 开始前等待。
// 3. 在 GBuffer 回调录制后，针对数万条指令执行单 Pass 内部的快速 Radix Sort。

// Kajiya 的 Temporal 系统融合：导出历史缓冲留待下一帧首尾相接
graph.export_temporal(hdr_color);

graph.compile();
graph.execute();
```

---

## 四、与前一版 IR 指令架构的详细对比

| 对比维度 | 前版（指令级 IR 架构） | 本版（混合粒度架构） | 结果分析 |
|---------|--------------------|-------------------|---------|
| **编译耗时 (CPU)** | `O(N^2)`, N为数万条 DrawCall，轻易超过 3-5ms | `O(M)`, M为一两百个 Pass，稳定在 <0.1ms | 彻底根除 CPU 瓶颈 |
| **Subpass 生成** | 依据指令依赖+启发式魔法，强制合并相邻指令 | **显式** 编组，Graph 和反射仅负责**校验**和内存优化 | 避免移动端 Tile 爆存、崩溃 |
| **Barrier 粒度** | 穿插在单条 Draw/Dispatch 指令之间 | **Pass 边界** 配合 Event/Semaphore 的 Split Barrier | 契合现代驱动的期望，避免阻塞 |
| **Dead Code 消除** | 细粒度指令消除（与 Indirect Draw 矛盾冲突） | 宏观整 Pass 剔除（尊重 GPU-Driven） | 逻辑严密自洽不会出错 |
| **内存/带宽复用** | 指令级精准 (完美但不可行) | 宏观生命周期覆盖 (极高性价比) | 用 5% 的精度损失换取 95% 性能 |
| **管线状态优化** | 全局范围重排序指令 | **Pass 内** 基于 Radix Hash 的局部状态聚合 | 高度并发友好，无锁开销 |

## 五、结语
引擎底层架构的美感应当来源于 **“恰到好处的控制力”**。
通过这套“混合粒度 Pragmatic Render Graph”，我们取了纯 IR 和传统 Pass Graph 的交集：利用 Shader 反射做无痛的开发接口，利用 Pass 做稳健低开销的宏观拓扑防爆，再利用 Pass 内局部指令缓存执行极致的微观压榨。这才是目前面对大尺度渲染（GPU-Driven + Bindless）唯一可靠的现代解法。
