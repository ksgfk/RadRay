# RadRay 绑定系统重构草稿

## 背景

RadRay 当前是一个实验性渲染工具集，后端至少涉及 D3D12 与 Vulkan。两者在资源绑定模型上存在明显差异：

- **D3D12**
  - shader 资源绑定点由 `register type + register index + space` 决定
  - root signature 可以选择不同承载方式：
    - root constants
    - root descriptor
    - descriptor table

- **Vulkan**
  - shader 资源绑定点最终对应 `set + binding`
  - pipeline layout / descriptor set layout 必须与 shader 接口匹配
  - push constants 是单独的一类机制

如果在 RHI 层直接暴露这些底层概念，会导致前端接口被平台模型绑死，难以统一，也不利于后续优化与维护。

因此，绑定系统需要进行一次更系统的重构。

---

## 目标

本次重构的目标是建立一套统一的绑定抽象，使上层尽量不感知 Vulkan 和 D3D12 的差异，同时保留后端针对不同平台做布局优化的空间。

具体目标如下：

1. **以 shader 为绑定事实来源**
   - 避免维护多份重复绑定描述
   - 减少 shader 与 C++ 手工同步成本

2. **统一前端绑定模型**
   - 不直接暴露 Vulkan descriptor set / push constants
   - 不直接暴露 D3D12 root signature parameter 类型

3. **后端负责平台特化**
   - Vulkan 后端生成 descriptor set layout / pipeline layout / push constant range
   - D3D12 后端生成 root signature / descriptor table / root descriptor / root constants

4. **保留显式 ABI 控制能力**
   - 允许 shader 作者直接通过 HLSL 的标准机制表达绑定意图
   - 让性能与布局意图可以由上层控制，而不是完全依赖黑盒推断

---

## 核心设计原则

### 1. HLSL 作为主要 shader 输入语言

当前设计假设 HLSL 是主要的 shader 源语言，并作为绑定 ABI 的主要输入来源。

这意味着：

- 手写 `register(...)` 与 `space` 是允许且合理的
- 它们不是“泄露平台细节”，而是正式 ABI 的组成部分
- Vulkan 侧也可通过 DXC 编译出的 SPIR-V 配合反射进行统一处理

---

### 2. 绑定信息主要由 shader 反射获得

绑定布局信息原则上以 shader 反射结果为准，主要信息来源包括：

- HLSL `register(type, index, space)`
- 可选的 HLSL `[[vk::binding]]`
- 反射获得的：
  - 资源名
  - 资源类型
  - 数组大小
  - shader stage 可见性
  - 常量块大小等信息

推荐的总体流程为：

1. 编译 HLSL
2. 提取 shader reflection 信息
3. 生成统一的绑定布局描述
4. 由后端编译为具体平台布局

其中：

- **D3D12** 可通过 DXC reflection 获取资源信息
- **Vulkan** 可通过 DXC 输出 SPIR-V，再由 SPIRV-Cross / SPIRV-Reflect 获取资源信息

---

### 3. 显式 ABI 优于隐式推断

当前设计倾向于承认 shader ABI 是显式且可控的，而不是完全依赖运行时或工具自动推测：

- 哪些资源属于 frame / material / object
- 哪些资源应走 root constants
- 哪些资源应走 descriptor table

相较于复杂且不稳定的自动推断，显式 ABI 更容易保证：

- 行为稳定
- 调试简单
- 性能可预测
- shader 作者拥有控制权

---

### 4. `register/space` 可表达绑定优先级

当前讨论中，倾向约定：

- `space` 和 `register` 越靠前，绑定越“热”
- 越靠前的绑定，越值得走更低开销的后端路径

这里更准确的概念不是单纯“更新频率”，而是：

- **binding hotness**
- **optimization priority**

也就是说：

- 较早的 `space`
- 较小的 `register index`

表示该资源更值得被后端优先优化，而不是仅表示它“更新得更频繁”。

---

## 统一绑定抽象

当前重构方向倾向引入如下统一对象体系：

- `BindingSetLayout`
- `BindingSet`

这套抽象用于统一描述和管理一组 shader 可见绑定，而不是直接等价于某个具体平台对象。

---

### `BindingSetLayout`

`BindingSetLayout` 表示一份统一的绑定布局描述。

其职责是：

- 描述某个 shader / pipeline 所需的绑定结构
- 持有从 shader 反射得到的布局信息
- 作为运行时创建和绑定 `BindingSet` 的依据
- 为后端编译 Vulkan / D3D12 平台布局提供输入

它是逻辑布局，不直接等价于：

- `VkDescriptorSetLayout`
- `VkPipelineLayout`
- `ID3D12RootSignature`

这些都属于后端编译产物。

---

### `BindingSet`

`BindingSet` 表示一份可被绑定到 pipeline 的绑定实例。

它的语义应理解为：

- 基于某个 `BindingSetLayout`
- 持有一组绑定内容
- 可被命令列表统一绑定
- 不要求直接暴露底层 Vulkan/D3D12 细节

它不应简单理解为：

- Vulkan 的 `VkDescriptorSet`
- D3D12 的某种单一原生对象

更准确地说，`BindingSet` 是 RHI 层的统一绑定实例，而具体如何在后端物化，交由后端决定。

---

## BindingSet 的创建与管理

当前阶段不强制公开单独的 allocator/pool 概念，而是只要求系统具备以下能力：

- 基于 `BindingSetLayout` 创建 `BindingSet`
- 为 `BindingSet` 提供底层 backing 与运行时管理
- 在不同后端中吸收 Vulkan 与 D3D12 的实例化差异

也就是说：

- 对前端而言，重点是“有 layout，有 set，可绑定”
- 对后端而言，仍然需要实现 descriptor / staging / backing 的管理逻辑
- ���些资源管理设施可以以后表现为 allocator、pool、arena，或保持为内部实现细节

因此，当前文档不将 `BindingSetAllocator` 视为必须公开的核心对象，但保留该方向作为实现层可能的演化空间。

---

## 后端实现策略

### Vulkan 后端

在 Vulkan 后端中，统一绑定抽象大概率会主要落成：

- `VkDescriptorSetLayout`
- `VkPipelineLayout`
- `VkDescriptorSet`
- push constant range

也就是说，Vulkan 后端的实现仍然大体符合 Vulkan 原生模型，但这些对象和流程不直接暴露给上层。

---

### D3D12 后端

在 D3D12 后端中，统一绑定抽象会被编译为：

- root signature
- descriptor ranges
- descriptor tables
- root constants
- root descriptors

同时，运行时还需要处理：

- CPU 侧 descriptor/staging 数据
- 绑定时的 descriptor heap 写入或复制
- root parameter 设置

因此 D3D12 下的 `BindingSet` 不对应某个单一 native object，而更像：

- 一份绑定内容描述
- 一份可在 bind 时被物化成 root/table 命令的数据集合

---

## 对平台差异的处理原则

本次重构的核心原则之一是：

> Vulkan 与 D3D12 的绑定差异不是上层接口的一部分，而是后端编译与物化逻辑的一部分。

也就是说：

### 前端不直接看到
- Vulkan descriptor set update
- Vulkan push constants
- D3D12 root descriptor
- D3D12 descriptor table
- D3D12 root constants

### 前端只看到
- 统一布局
- 统一实例
- 统一绑定入口

---

## 关于现有 `DescriptorSet` 抽象

RadRay 当前已经有 `DescriptorSet` 抽象，这并不一定需要立刻废弃。

更合理的演进方式是：

- 保留其存在价值
- 逐步提升其语义
- 让它从“更像 Vulkan descriptor set 的对象”演化为“统一绑定实例抽象”的一部分

如果后续语义进一步明确，也可以平滑过渡到 `BindingSet` 体系。

---

## 关于 RHI “薄” 的重新理解

当前讨论后的共识是：

- **前端接口应尽量薄**
- **内部实现不应追求简单薄封装**

因为一旦真正统一 Vulkan 与 D3D12 的绑定模型，内部就必然需要承担：

- shader reflection 整理
- 布局编译
- 平台特化 lowering
- 运行时实例管理
- 绑定时物化

因此更准确的说法应是��

> 这是一个“前端抽象薄、内部实现厚”的绑定系统。

---

## 推荐的整体流程

### 1. Shader 编译与反射
- 输入 HLSL
- 提取资源绑定信息
- 获得统一布局所需元数据

### 2. 构建统一布局
- 基于 shader reflection 构建 `BindingSetLayout`
- 保留 ABI 信息，如 `register/space`

### 3. 创建绑定实例
- 基于 `BindingSetLayout` 创建 `BindingSet`
- 向其中设置资源或常量数据

### 4. 后端编译与绑定
- Vulkan 后端生成并使用 descriptor sets / pipeline layout / push constants
- D3D12 后端生成并使用 root signature / descriptor tables / root parameters

---

## 当前阶段的设计结论

当前可以形成的设计结论如下：

1. **绑定系统应以 shader reflection 为核心输入**
2. **HLSL `register/space` 是合法且重要的显式 ABI**
3. **`register/space` 的前后顺序可作为绑定热度与优化优先级的表达方式**
4. **前端不应直接暴露 Vulkan 与 D3D12 的底层绑定机制**
5. **应建立统一的 `BindingSetLayout / BindingSet` 抽象**
6. **push constants、descriptor table、root descriptor、root constants 等差异应由后端吸收**
7. **BindingSet 的创建与底层 backing 管理由运行时统一负责，不要求当前阶段公开独立 allocator**
8. **RadRay 的绑定系统应被理解为一个统一绑定编译与物化系统，而不是简单的平台 API 包装**

---

## 一句话总结

> RadRay 的绑定系统重构方向，是以 HLSL 反射与显式 `register/space` ABI 为核心输入，建立统一的 `BindingSetLayout / BindingSet` 抽象，由后端根据 shader 反射和既���规则，将其分别编译和物化为 Vulkan 与 D3D12 所需的具体绑定布局与运行时绑定命令。

---

## 暂不纳入本稿的后续计划

以下内容属于后续进一步细化阶段，暂不纳入本次重构草稿：

- `BindingField`
- `BindingEntry`
- 更细粒度的单项复用机制
- 更细的实例缓存策略
- 生命周期分层
- 原生对象互操作扩展（如显式导入/导出 `VkDescriptorSet`）

这些内容可以在本次统一绑定框架落定后，再作为第二阶段设计继续展开。