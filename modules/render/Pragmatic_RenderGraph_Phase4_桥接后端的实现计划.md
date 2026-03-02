# Pragmatic Render Graph (PRG) 阶段四实现计划：执行引擎 (Executor) 桥接

> 在完成 Phase 1-3 纯逻辑图的编译（包含依赖拓扑、死代码移除与读写屏障状态机分析）之后。Phase 4 的核心目的是将 `CompiledGraph` 高维指令翻译为具体的图形 API （如 Vulkan/D3D12/Metal）底层渲染调用，实现在引擎真正的 Frame Loop 中驱动 GPU 的功能。此过程将构建在方向B：**"后端执行引擎桥接 (Executor)"** 的主轴上。

---

## 4.1 核心目标与架构挑战

1. **多态执行器架构（Polymorphic Executor Abstraction）**: 建立独立于后端的执行器基类 `RGExecutor`，并派生出特定后端的子类（如 `RGExecutorD3D12` 和 `RGExecutorVulkan`）。由于 `CompiledGraph` 不携带任何“如何渲染”或者实际 Backend Object 指针，我们需要在子类中构建特定后端的翻译字典。
2. **生命周期实化与物理资源分配**: 将图里虚拟的 `RGResourceHandle` 实例化出或关联到基于现有 `radray::render::Device` 的实际 `Buffer` / `Texture` 对象。如果采用对象池，需要在执行开始时分发物理资源，在结束时回收。
3. **后端原生屏障下发 (Backend-Native Barrier Emission)**: 虽然底层的 `common.h` 已经具备了一套类 DX12 风格的 `ResourceBarrier` 包装，但 DX12 和 Vulkan 的屏障哲学存在本质差异（DX12 注重 State，Vulkan 注重 Pipeline Stages 和 Access Flags）。因此，`RGExecutorVulkan` 等子类将**直接绕过粗糙的抽象包装**，将 `RGBarrier` 转化为最优化、最原生的底层调用（如 `vkCmdPipelineBarrier2`），而 `RGExecutorD3D12` 则继续映射到其对应的最佳实现。
4. **渲染回调 (Pass Callback/Lambda)**: 将物理绑定、环境就绪（屏障已打）完成的 CommandEncoder 移交给用户预先定义的逻辑闭包，使图能够在正确的时机呼叫真正的渲染指令。

---

## 4.2 任务清单与设计步骤

### Step 1: 物理注册与绑定器设计 (RGResourceRegistry)
由于 `RGGraphBuilder` 仅仅是资源 Descriptor 的声明簿。需要一个能够维持与图形 Device 上存在的物理资源关系的登记表（Registry）。

*   [ ] **设计 `RGRegistry`**:
    *   通过 `ImportPhysical(...)` 方法，允许将属于系统现有的 `SwapChain` 的 Backbuffer，或是上一帧遗留的 G-Buffer 手动注入。
    *   管理一个对象池 `Pool`，针对内部创建的瞬时（Transient）虚拟 Handle，根据其格式/尺寸/用途按需申请 `radray::render::Texture/Buffer` 并缓存。
    *   为了快速迭代，起步阶段**不追求复杂的无锁或者别名分配池**。而是通过最直白的：一个虚拟描述符要求配一个物理独立对象机制来实现。

### Step 2: 定义执行环境包裹与回调 (RGPass Execution Closure)
目前的 `RGPassNode` 只保存了 `Name` 和要干什么的边，缺少了“到了这个 Pass 该具体执行这段用 `Device` 怎么处理的 C++ 代码”。

*   [ ] **新增执行环境包裹 Context**:
    ```cpp
    struct RGPassContext {
        // 所分配对应的 CommandEncoder / CommandList
        CommandEncoder* Encoder{nullptr};
        // 允许通过虚拟句柄抓取真正的物理图元供 Shader 绑定
        RGRegistry* Registry{nullptr}; 
    };
    ```
*   [ ] **改造 Pass 记录结构**:
    在 Graph Builder 这侧加一个功能，让用户可以通过 `std::function` 向该 Pass 挂载执行逻辑（类似 `passBuilder.SetExecuteFunc([](RGPassContext& ctx) { /*...*/ })`）。

### Step 3: 在后端实现具体的指令解析编译器 (多态 RGExecutor 实现)
我们需要写一个执行引擎框架，设计为一个基类 `RGExecutor` 以及对应的后端特化子类：

*   [ ] **定义执行器接口** `virtual void RGExecutor::Execute(CommandQueue* queue, const RGGraphBuilder& graph, const CompiledGraph& compiled) = 0;`。
*   [ ] **基类通用逻辑**：
    *   **资源孵化**：扫描 `graph.GetResources()`。如果资源是外置 (`External`)，必须确认在 Registry 中是否已被注册过物理绑定。如果不是，用 `Device` 新建造相对应的物理资源，并登记。
    *   **分配与提交**：根据 Pass 分配 `radray::render::CommandEncoder`，并在结束时完成 CommandBuffer 录制，送入 `queue->Submit(commandBuffer)`。放回瞬时资源。
*   [ ] **核心特化：后端专属的屏障翻译机制** 
    为图内遍历计算好的每一组 `compiled.PassBarriers[passIndex]` 执行状态转换：
    *   **`RGExecutorD3D12`**：将 `RGBarrier` 的 `OldMode/NewMode` 翻译为 DX12 风格的 State 结合 `D3D12_RESOURCE_BARRIER`，可以直接复用或对接底层的包装。
    *   **`RGExecutorVulkan`**：彻底摒弃使用 `CommandBuffer::ResourceBarrier` 这个上层 wrapper。在执行器内部，通过暴露的 Vulkan 原生后端接口（或者在 Vulkan Backend 内部开设专用通道），将 `RGBarrier` 精确计算并生成对应的 `VkPipelineStageFlags2` 和 `VkAccessFlags2`，调用 `vkCmdPipelineBarrier2`。从而真正发挥 Vulkan 屏障的细粒度和高性能。

### Step 4: 细化 ExecutorVulkan 的特权通信与依赖打通
为了让 `RGExecutorVulkan` 可以调用类似于 `vkCmdPipelineBarrier2`，需要在现有的架构中开辟安全的后端通讯通道：

*   [ ] **Backend Downcasting 或 特权接口**:
    在 Executor 内部，当确认当前是在 Vulkan 环境下时，可以通过 dynamic_cast 类型转换为真正的 `VulkanCommandBuffer`，以获取内部的 `VkCommandBuffer` 句柄，从而直接插入高效的原生屏障（包含正确的 Layout 转换和 Queue Family 处理）。此模式不对公共抽象层 `common.h` 产生污染。

---

## 4.3 验收路径规划 (Acceptance Criteria)

开发执行引擎涉及跨模态打通，不建议一开始挑战超大号测试。我们应该从“使用 RG 绘制基本图形”开始验收。

1.  **里程碑 A：实现 `RGExecutor` 基类及其多态架构，定义 `RGRegistry` 与执行环境 `RGPassContext`。**
2.  **里程碑 B：编写 `RGExecutorVulkan`，完成从 `RGBarrier` 到原生 `vkCmdPipelineBarrier2` 的直接转换映射。**（核心验证点：不使用 Common 层的 Wrapper）
3.  **里程碑 C：编写 `RGExecutorD3D12` 作为对比与验证。**
4.  **里程碑 D：实现全新测试目标。在 `tests` 目录下新建一个专门的测试目标（如 `test_render_graph_executor`）。绝对不要覆写现有的 `test_hello_world_triangle`**。新测试将使用 Render Graph 语法完成分配与渲染闭包，并验证在不同后端下执行器均能成功编译成正确的底层调用，Validation Layer 无报错。

---

## 下一步行动建议

请直接要求 AI 执行 **里程碑 A 与 B：搭建 `RGExecutor` 基类并重点实现 `RGExecutorVulkan` 的原生屏障发射**。你需要提供目前内部真正的 Vulkan Backend 类的简单结构体定义（例如 `VulkanCommandBuffer` 及其包含原生句柄的方式），以便 AI 可以写出安全向下转型并直接调用 `vkCmd` 级别的完美 Vulkan 屏障代码。