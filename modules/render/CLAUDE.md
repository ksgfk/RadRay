# modules/render — Render RHI 层说明

## 目录结构

```
modules/render/
├── CMakeLists.txt
├── include/radray/render/
│   ├── common.h          # 核心接口定义
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

## Shader 编译流程

```
HLSL
 ├─[DXC]──→ DXIL        (D3D12)
 ├─[DXC]──→ SPIR-V      (Vulkan)
 └─[DXC]──→ SPIR-V ──[SPIR-V Cross]──→ MSL  (Metal)
```
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

## GPU 资源辅助 (gpu_resource.h)

- `RenderMesh` — 顶点/索引缓冲管理，含绘制数据
- `CBufferArena` — 常量缓冲 arena 分配器，支持帧级 reset/clear
  - `Allocation` — 已分配的常量缓冲区域
  - `Block` — 内部内存块

## CMake 构建配置

**条件编译开关：**
- `RADRAY_ENABLE_D3D12` — D3D12 后端
- `RADRAY_ENABLE_VULKAN` — Vulkan 后端
- `RADRAY_ENABLE_METAL` — Metal 后端
- `RADRAY_ENABLE_DXC` — DXC shader 编译器
- `RADRAY_ENABLE_SPVCROSS` — SPIR-V Cross

## 关键模式与约定

1. **Nullable 模式**：`Nullable<T>` 包装指针类型，`.HasValue()` / `.Unwrap()` / `.Release()`
2. **RAII**：所有 GPU 资源通过 `RenderBase` 禁止拷贝/移动
3. **工厂模式**：`Device` 创建所有 GPU 对象，返回 `Nullable<unique_ptr<T>>`
5. **显式状态管理**：`BufferState` / `TextureState` 枚举追踪资源状态，手动插入 barrier
