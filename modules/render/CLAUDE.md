# modules/render — Render RHI 层说明

## 目录结构

```
modules/render/
├── CMakeLists.txt
├── include/radray/render/
│   ├── common.h          # 核心接口定义 (~1526 行)
│   ├── dxc.h             # HLSL 编译器封装
│   ├── spvc.h            # SPIR-V Cross 封装
│   ├── msl.h             # Metal Shading Language 工具
│   ├── bind_bridge.h     # 跨后端资源绑定抽象
│   ├── gpu_resource.h    # GPU 资源辅助类
│   ├── render_utility.h
│   ├── debug.h
│   └── backend/
│       ├── d3d12_impl.h / d3d12_helper.h
│       ├── vulkan_impl.h / vulkan_helper.h
│       └── metal_impl.h / metal_impl_cpp.h / metal_helper.h
└── src/
    ├── common.cpp / dxc.cpp / spvc.cpp / msl.cpp
    ├── bind_bridge.cpp / gpu_resource.cpp
    ├── d3d12/
    │   ├── d3d12_impl.cpp
    │   └── d3d12_helper.cpp
    ├── vk/
    │   ├── vulkan_impl.cpp
    │   ├── vulkan_helper.cpp
    │   └── vulkan_macos_surface.mm
    └── metal/
        ├── metal_impl.mm
        └── metal_helper.mm
```

## 核心架构

### 虚接口层 (common.h)

所有 GPU 对象继承自 `RenderBase`（禁止拷贝/移动，RAII）：

```cpp
class RenderBase {
    virtual RenderObjectTags GetTag() const noexcept = 0;
    virtual bool IsValid() const noexcept = 0;
    virtual void Destroy() noexcept = 0;
};
```

**Device** 是所有 GPU 对象的工厂，继承 `enable_shared_from_this<Device>`。
入口：`CreateDevice(DeviceDescriptor)`，其中：
```cpp
DeviceDescriptor = std::variant<D3D12DeviceDescriptor, MetalDeviceDescriptor, VulkanDeviceDescriptor>
```

所有创建方法返回 `Nullable<unique_ptr<T>>`：
- `.HasValue()` — 检查是否有值
- `.Unwrap()` — 提取（空时抛异常）
- `.Release()` — 无异常提取

### 类层次结构

| 类别 | 类 |
|------|-----|
| 命令 | `CommandQueue`, `CommandBuffer` |
| 编码器 | `CommandEncoder` → `GraphicsCommandEncoder` / `ComputeCommandEncoder` / `RayTracingCommandEncoder` |
| 资源 | `Resource` → `Buffer`, `Texture`, `AccelerationStructure` |
| 资源视图 | `ResourceView` → `BufferView`, `TextureView`, `AccelerationStructureView` |
| 管线 | `Shader`, `RootSignature`, `PipelineState` → `GraphicsPipelineState` / `ComputePipelineState` / `RayTracingPipelineState` |
| 绑定 | `DescriptorSet`, `Sampler`, `BindlessArray` |
| 同步 | `Fence`, `Semaphore`, `SwapChain` |

## Shader 编译流程

```
HLSL
 ├─[DXC]──→ DXIL        (D3D12)
 ├─[DXC]──→ SPIR-V      (Vulkan)
 └─[DXC]──→ SPIR-V ──[SPIR-V Cross]──→ MSL  (Metal)
```

### DXC (dxc.h)
- `Dxc` — 编译器实例
- `DxcCompileParams` — 编译参数
- `DxcOutput` — 编译结果（字节码 + 反射数据）
- `HlslShaderDesc` — HLSL 反射信息（常量缓冲、绑定、签名）

### SPIR-V Cross (spvc.h)
- `ConvertSpirvToMsl()` — SPIR-V 转 MSL
- `SpirvShaderDesc` — SPIR-V 反射（类型、顶点输入、资源绑定、push constants）
- `SpirvToMslOption` — MSL 版本、平台（macOS/iOS）、argument buffers 开关

### MSL 工具 (msl.h)
- `MslShaderReflection` — MSL 反射信息
- `MetalMaxVertexInputBindings = 16` — **关键常量**，驱动整个 Metal 绑定布局

## Metal 后端绑定布局

**Graphics 阶段（Vertex/Fragment）：**
```
buffer(0-15)  — 顶点输入缓冲（保留，fragment 阶段同样保留）
buffer(16)    — push constants (setVertexBytes / setFragmentBytes)
buffer(17+)   — argument buffers，每个 descriptor set 一个
                (set N → buffer(N + 17))
```

**Compute 阶段：** 无偏移，资源直接使用原始绑定索引。

**SPIR-V → MSL 重映射（ConvertSpirvToMsl）：**
- 启用 argument buffers：每个 descriptor set 映射到 `msl_buffer = descSet + MetalMaxVertexInputBindings + 1`
- Push constants：`msl_buffer = MetalMaxVertexInputBindings`（即 16）
- 不启用 argument buffers：buffer 资源偏移 `+MetalMaxVertexInputBindings`，texture/sampler 保持原始索引

**HLSL register → Metal index 对照表（启用 argument buffers）：**

| HLSL | Metal (graphics) | Metal (compute) |
|------|-----------------|-----------------|
| `[[vk::push_constant]]` | `buffer(16)` | `buffer(0)` |
| `register(*, space0)` | argument buffer @ `buffer(17)` | 直接绑定 |
| `register(*, space1)` | argument buffer @ `buffer(18)` | 直接绑定 |
| 顶点属性 | `buffer(0-15)` / `attribute(N)` | N/A |

**运行时绑定（metal_impl.mm）：**
- `BindVertexBuffer` → `setVertexBuffer:atIndex:` index 0-15
- `SetPushConstant` → `set{Vertex,Fragment}Bytes:atIndex:` 使用 `slot`（无运行时偏移）
- `SetDescriptorSet` → `set{Vertex,Fragment}Buffer:atIndex:` 使用 `slot`（无运行时偏移）
- Metal 后端**不支持** Root Descriptors

## Bind Bridge 系统 (bind_bridge.h)

跨后端资源绑定抽象层，屏蔽各后端差异。

**核心类：**
- `BindBridgeLayout` — 分析 shader 反射，创建绑定布局
- `BindBridge` — 运行时绑定管理器
- `IBindBridgeLayoutPlanner` — 后端特定布局规划接口
  - `D3D12BindBridgeLayoutPlanner`
  - `VulkanBindBridgeLayoutPlanner`
  - `MetalBindBridgeLayoutPlanner`

**绑定条目类型：**
- `BindBridgePushConstEntry` — Push constants
- `BindBridgeRootDescriptorEntry` — Root descriptors（D3D12 概念）
- `BindBridgeDescriptorSetEntry` — Descriptor set 绑定

**工作流：**
1. 从 shader 反射（HLSL/SPIR-V/MSL）构建 IR
2. 使用后端特定 planner 规划布局
3. 从规划布局创建 `RootSignature`
4. 运行时使用 `BindBridge` 绑定资源

## GPU 资源辅助 (gpu_resource.h)

- `RenderMesh` — 顶点/索引缓冲管理，含绘制数据
- `CBufferArena` — 常量缓冲 arena 分配器，支持帧级 reset/clear
  - `Allocation` — 已分配的常量缓冲区域
  - `Block` — 内部内存块

## 关键枚举与标志

使用 `EnumFlags<T>` + `is_flags<T>` trait 实现位运算枚举：

| 枚举 | 用途 |
|------|------|
| `ShaderStage` | Vertex, Pixel, Compute, RayGen, Miss, ClosestHit, AnyHit, Intersection, Callable |
| `BufferUse` | Common, MapRead/Write, CopySource/Dest, Index, Vertex, CBuffer, Resource, UAV, Indirect, AS, Scratch, ShaderTable |
| `TextureUse` | CopySource/Dest, Resource, RenderTarget, DepthStencilRead/Write, UAV |
| `BufferState` / `TextureState` | 资源状态追踪，用于 barrier |
| `TextureFormat` | 40+ 格式（R8-RGBA32、深度模板、压缩格式） |
| `VertexFormat` | 30+ 顶点属性格式 |
| `ResourceBindType` | CBuffer, Buffer, Texture, Sampler, RWBuffer, RWTexture, AccelerationStructure |

## CMake 构建配置

**条件编译开关：**
- `RADRAY_ENABLE_D3D12` — D3D12 后端
- `RADRAY_ENABLE_VULKAN` — Vulkan 后端
- `RADRAY_ENABLE_METAL` — Metal 后端
- `RADRAY_ENABLE_DXC` — DXC shader 编译器
- `RADRAY_ENABLE_SPVCROSS` — SPIR-V Cross

**依赖：**
- Public：`radraycore`, Vulkan-Headers, volk, VMA, DirectX-Headers, D3D12MemoryAllocator
- Private：dxcompiler, spirv-cross (core/glsl/msl/util)
- Metal 框架：Metal, QuartzCore, Cocoa (macOS) / UIKit (iOS)

**特殊处理：**
- Metal `.mm` 文件使用 `-fobjc-arc` 编译（自动引用计数）
- DXC 运行时 DLL 在构建后复制到输出目录

## 关键模式与约定

1. **Nullable 模式**：`Nullable<T>` 包装指针类型，`.HasValue()` / `.Unwrap()` / `.Release()`
2. **RAII**：所有 GPU 资源通过 `RenderBase` 禁止拷贝/移动
3. **工厂模式**：`Device` 创建所有 GPU 对象，返回 `Nullable<unique_ptr<T>>`
4. **Variant 描述符**：`std::variant` 用于后端特定描述符
5. **显式状态管理**：`BufferState` / `TextureState` 枚举追踪资源状态，手动插入 barrier
6. **多语言注释**：代码注释中英混用，C++20 特性（concepts、ranges），Metal 使用 Objective-C++
