# D3D12 / Vulkan / Metal 光线追踪 API 总结

## 1. 共同工作原理

三种图形 API 的光线追踪在核心流程上是一致的：

1. 构建加速结构（Acceleration Structure, AS），通常分为底层几何结构（BLAS）和顶层实例结构（TLAS）。
2. 发射射线后，由硬件遍历与求交单元执行高效遍历与相交测试。
3. 命中或未命中后，进入可编程着色阶段（如 miss / closest-hit / any-hit / intersection / callable 或其等价机制）。
4. 既可以使用完整 RT 管线（专用调度命令 + SBT/等价表），也可以在普通着色器中使用 inline query（RayQuery / intersector）。

---

## 2. D3D12（DXR）

### 2.1 能力与入口

- 通过 `CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5)` 查询 `RaytracingTier`。
- 在命令列表中使用 `ID3D12GraphicsCommandList4` 的 DXR 相关接口。

### 2.2 加速结构构建

- 使用 `BuildRaytracingAccelerationStructure`。
- 描述体核心为 `D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC`，包含输入几何、目标地址、scratch 地址等。

### 2.3 RT 管线与着色器组织

- 通过 `D3D12_STATE_OBJECT_DESC` 创建 RT state object。
- 由 subobject 组合：DXIL libraries、hit groups、shader/pipeline config、global/local root signature。

### 2.4 Shader Table 与调度

- 通过 `ID3D12StateObjectProperties::GetShaderIdentifier` 获取 shader identifier。
- 按 shader record 组织 raygen/miss/hit/callable table。
- 使用 `DispatchRays` + `D3D12_DISPATCH_RAYS_DESC` 发射。

### 2.5 Inline 模式

- DXR 1.1 支持 `RayQuery::TraceRayInline()`，可在常规 shader 中做光线查询。

---

## 3. Vulkan（VK_KHR Ray Tracing）

### 3.1 扩展与特性

常用扩展组合：

- `VK_KHR_acceleration_structure`
- `VK_KHR_ray_tracing_pipeline`
- `VK_KHR_ray_query`

可配合：`VK_KHR_pipeline_library`、`VK_KHR_deferred_host_operations`。

### 3.2 设备特性启用

- 使用 `vkGetPhysicalDeviceFeatures2` 查询。
- 在 `VkDeviceCreateInfo::pNext` 链中启用：
  - `VkPhysicalDeviceAccelerationStructureFeaturesKHR`
  - `VkPhysicalDeviceRayTracingPipelineFeaturesKHR`
  - `VkPhysicalDeviceRayQueryFeaturesKHR`

### 3.3 加速结构构建

- `vkGetAccelerationStructureBuildSizesKHR` 获取大小。
- `vkCreateAccelerationStructureKHR` 创建对象。
- `vkCmdBuildAccelerationStructuresKHR` 进行构建。

### 3.4 RT 管线与调度

- 用 ray tracing pipeline 创建对应阶段（raygen/miss/hit/callable）。
- 使用 `vkCmdTraceRaysKHR` 调度。
- 通过 4 个 `VkStridedDeviceAddressRegionKHR` 指定 raygen/miss/hit/callable 的 SBT 区域。

### 3.5 同步与绑定要点

- Vulkan 对 AS 构建与读写同步要求更显式，需自行插入 barrier。
- AS 作为描述符资源，类型为 `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`。

### 3.6 Inline 模式

- `VK_KHR_ray_query` 允许在普通 shader 中执行光线查询，不需要单独 trace rays 调度。

---

## 4. Metal Ray Tracing

### 4.1 核心模型

- 同样使用加速结构 + 硬件求交。
- 在 Metal 中，光追能力更强调与 compute/render shader 的结合。

### 4.2 加速结构

- 几何层：`MTLPrimitiveAccelerationStructureDescriptor`
- 实例层：`MTLInstanceAccelerationStructureDescriptor`
- 由 `MTLAccelerationStructureCommandEncoder` 构建。

### 4.3 几何与可编程求交

- 支持三角形与 AABB。
- 自定义几何求交通过 intersection function 与 `MTLIntersectionFunctionTable`。

### 4.4 函数表机制

- 通过 `MTLVisibleFunctionTable` / `MTLIntersectionFunctionTable` 做动态函数分发与求交逻辑组织。
- 与 DXR 的阶段化 SBT 模型不完全同构，但可表达类似材质与命中分发需求。

### 4.5 DXR 迁移路径

- Apple 的 Metal Shader Converter 提供了从 DXR 语义到 Metal 执行模型的映射（含 SBT buffer 与 visible functions 的结合方式）。

---

## 5. 三种 API 综合对比

### 5.1 编程模型

- D3D12（DXR）：阶段最直观，State Object + Shader Table + DispatchRays。
- Vulkan：语义接近 DXR，但扩展和同步更显式，灵活性最高、复杂度也更高。
- Metal：原生模型更偏 compute/render + intersector + function table；DXR 风格可通过转换/适配实现。

### 5.2 资源与调度抽象

- DXR/Vulkan：SBT 是一等公民。
- Metal：原生接口更偏函数表与 buffer 编排，SBT 语义通常由上层抽象或转换层实现。

### 5.3 工程复杂度与可移植性

- Vulkan：可控性最高，工程门槛也最高（特性链、同步、布局更复杂）。
- DXR：接口统一性较好，Windows 平台开发效率高。
- Metal：苹果平台表现与生态最优，但跨平台引擎通常需要单独后端抽象。

### 5.4 性能调优共性

- 三者都高度依赖 AS 策略（重建 vs refit、compaction、并行构建）。
- 减少无效 traversal、控制 payload 大小、合理 TLAS/BLAS 划分通常比局部 shader 优化更关键。

---

## 6. 跨 API 抽象建议

如果引擎需要统一 DXR/Vulkan/Metal，建议将后端抽象拆为以下模块：

1. AS 生命周期接口：创建、更新、重建、压缩、销毁。
2. RT 调度接口：全管线 trace 与 inline query 双路径。
3. 材质与命中分发接口：统一 hit group / function table 的上层语义。
4. SBT/函数表记录布局抽象：统一 shader record 元数据与资源绑定模型。
5. 平台能力查询：按特性位启用功能（如程序化求交、ray query、动态栈深等）。

这样可以做到：Windows/Linux 走 DXR/Vulkan，macOS/iOS 走 Metal，同时保持渲染逻辑层代码一致。

---

## 7. 参考资料（官方）

- DXR Functional Spec: https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html
- D3D12 Build AS: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist4-buildraytracingaccelerationstructure
- D3D12 State Object: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_state_object_desc
- D3D12 DispatchRays: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist4-dispatchrays
- Vulkan Ray Tracing Guide: https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html
- Vulkan `vkCmdTraceRaysKHR`: https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdTraceRaysKHR.html
- Vulkan `vkCmdBuildAccelerationStructuresKHR`: https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBuildAccelerationStructuresKHR.html
- Metal WWDC23（Your guide to Metal ray tracing）: https://developer.apple.com/videos/play/wwdc2023/10128/
- Metal Shader Converter: https://developer.apple.com/metal/shader-converter/
