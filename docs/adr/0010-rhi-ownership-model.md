# ADR-0010 RHI 的所有权模型：Device 共享，其余独占

状态: 生效
日期: 2026-07
影响: `rhi.h` 全部 `CreateXxx` 工厂、两个后端的实现类、`RenderPassRegistry` / `SamplerCache`

## 背景

RHI 要对上层交出十几种 GPU 对象。每一种都可以选：`shared_ptr`、`unique_ptr`、
自定义引用计数、或裸指针 + 手动 `Destroy`。这个选择一旦铺开就极难改，因为它渗进每一个
调用点。

约束有两条。其一，后端实现对象普遍需要回指 device（拿分配器、描述符堆、Vulkan 函数表），
而 device 的生命周期由上层的 `GpuSystem` 决定。其二，Vulkan 与 D3D12 的原生对象生命周期
规则不同：D3D12 的 COM 对象自带引用计数，Vulkan 句柄要显式 `vkDestroyXxx` 且必须在
`VkDevice` 之前。

## 决策

**`Device` 用 `shared_ptr` 且 `enable_shared_from_this`；其余一切用
`Nullable<unique_ptr<T>>`。**

`Device::Create` 是唯一入口，其余全是 `Device` 上的纯虚工厂。`Nullable<unique_ptr<T>>`
表达"可能没有值"，调用方 `Get()` 取裸指针、`Release()` 转移所有权。

**需要共享的场合由上层解决，不下沉到 RHI。** runtime 层的 `SharedPipelineLayout` 做引用
计数去重（见 `architecture/asset-system.md`），render 层不知道这件事。

**两段式析构**：每个实现类有幂等的 `Destroy()` 和内部 `DestroyImpl()`，析构函数兜底调后者。
`Destroy()` 置空句柄与 `_valid`，所以可安全重复调用。缓存类要"先显式销毁再 erase"时就靠它。

唯一的例外是 Vulkan 的 `DescriptorSetLayoutVulkan`：它用 `IntrusivePtr` 做引用计数，
以便 `DescriptorSetLayoutCacheVulkan` 按 refcount 驱逐。这是后端内部优化，不出现在公共
RHI 面上。

## 放弃的方案及代价

- **全部用 `shared_ptr`**。每个 GPU 对象多一次原子操作和一个控制块，而 RHI 对象的共享需求
  实际上只出现在少数几处（layout、sampler）。更要紧的是它让"谁负责在 device 之前销毁"变得
  不可见——一个漏放的 `shared_ptr` 会让 Vulkan 句柄活过 `VkDevice`，症状是驱动层的崩溃。
- **裸指针 + 手动 `Destroy` 作为公共契约**。省下 `unique_ptr` 的形状，代价是每个调用点都要
  手写清理，且错误路径（创建失败后提前 return）极易漏。
- **`Device` 也用 `unique_ptr`**。后端对象需要回指 device；`unique_ptr` 下这只能是裸指针，
  于是"device 必须活得比它创建的一切都久"变成一条无法在类型上表达的口头约定。
  而 `enable_shared_from_this` 让后端能在需要时安全地持一份弱/强引用。
- **`std::optional<unique_ptr<T>>` 而非 `Nullable`**。语义等价但双层可空，
  且与仓库其余部分对"可能没有值"的表达不一致。

## 必须保持为真

- 全部 `Device::CreateXxx` 返回 `Nullable<unique_ptr<T>>`。新增工厂不要破例。
- 全部 RHI 对象在其 `Device` 之前销毁。
- `Destroy()` 幂等，且析构函数必须兜底调用销毁逻辑。
- 需要跨持有者共享的 GPU 对象在 runtime 层做引用计数，不在 render 层加 `shared_ptr`。
- `RenderBase` 保持禁拷贝禁移动。
