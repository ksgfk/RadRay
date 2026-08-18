# ADR-0011 后端选择走 descriptor variant，不做运行期能力探测

状态: 部分被 ADR-0043 取代
日期: 2026-07
影响: `Device::Create`（`rhi.cpp`）、`DeviceDescriptor`、`RADRAY_ENABLE_*` CMake 选项

## 背景

一个多后端渲染器要回答"这次跑哪个后端"。常见做法是运行期探测：依次尝试初始化
D3D12 → Vulkan → ...，第一个成功的就用它。

问题是"初始化失败"与"这台机器不支持"并不是同一件事。驱动 bug、缺 SDK、缺特定扩展、
调试层未安装，都会让初始化失败，而自动回退会把这些都变成静默的"换了个后端"。
于是同一份代码在两台机器上跑不同后端，而没有任何地方记录这件事发生了。

## 决策

**后端由调用方传入的 descriptor 类型决定，`Device::Create` 只做一次 `std::visit`。**

```cpp
using DeviceDescriptor = std::variant<D3D12DeviceDescriptor, MetalDeviceDescriptor, VulkanDeviceDescriptor>;
```

这是全仓库唯一的后端分派点。对应后端未编译进来时打日志返回 `nullptr`，**不回退到别的后端**。

后端能否编译进来由 CMake 决定：`RADRAY_ENABLE_D3D12`（需 WIN32）、`RADRAY_ENABLE_VULKAN`、
`RADRAY_ENABLE_METAL`（需 APPLE）。宏传播到源码，`#ifdef` 守卫后端头与 `rhi.cpp` 的 include，
源文件按开关追加。

**运行期探测只用于选 GPU，不用于选后端**：D3D12 走 DXGI 1.6 的
`EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` 并回退枚举评分，Vulkan 按
`VkPhysicalDeviceType` 打分。

**不支持的关键能力直接 abort，不静默降级。** Vulkan 的 `CreateFence` 在没有
`timelineSemaphore` 时 `RADRAY_ABORT`；跨队列 barrier 缺 timeline 同样 abort。理由同上：
一个降级路径若没人测过，它就是一条随时会出问题的暗路。

## 放弃的方案及代价

- **依次尝试初始化、自动回退**。省下调用方的一次选择，代价是失败原因被吞掉，
  且同一份代码在不同机器上跑不同后端而不留痕迹。排查"为什么这台机器上效果不一样"要从
  猜后端开始。
- **枚举 + if/switch 而非 variant**。`RenderBackend` 枚举本来就有（日志和产物命名要用），
  但用它做分派意味着后端专属参数只能塞进一个大结构体的并集，或者靠一个 `void*`。
  variant 让每个后端的参数各自成型，且新增后端时漏掉一个分支是编译错误。
- **运行期特性开关 + 降级路径**（例如无 timeline semaphore 时退回 binary semaphore + fence
  池）。要多写一整套同步实现，而它只在旧驱动上被执行——也就是最少被测到的路径上。
  当前目标平台都有 VK 1.2。
- **把 Metal 分支删掉**。它现在是"声明存在、实现缺失"的悬空状态。留着是因为
  `RenderBackend::Metal` 已被序列化数据与产物命名使用（枚举成员不能改名），
  且 macOS 走 Vulkan-on-Metal 时仍需要那条 surface 路径。代价是
  `RADRAY_ENABLE_METAL` 默认 ON 而在 Apple 平台会编译失败，这条记在
  `architecture/render-rhi.md` 的现状陷阱里。

## 必须保持为真

- `Device::Create` 是唯一的后端分派点。不要在别处 `#ifdef` 出第二条创建路径。
- 后端未编译时返回 `nullptr` 并打日志，绝不回退到另一个后端。
- 新增后端 = 往 `DeviceDescriptor` variant 加一个成员 + 一个 `RADRAY_ENABLE_*` 开关。
- 缺关键能力时 abort，不加降级实现。
- 不要改 `RenderBackend` 已有成员的名字（`magic_enum` 与序列化数据在用）。
