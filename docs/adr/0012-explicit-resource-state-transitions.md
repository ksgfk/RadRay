# ADR-0012 资源状态转换全部显式，RHI 不跟踪状态

状态: 生效
日期: 2026-07
影响: `CommandBuffer::ResourceBarrier`、`BarrierBufferDescriptor` / `BarrierTextureDescriptor`、两个后端的 barrier 翻译

## 背景

D3D12（非 enhanced barrier 路径）与 Vulkan 都要求调用方给出资源的前后状态：
D3D12 是 `D3D12_RESOURCE_STATES` 对，Vulkan 是 access mask + image layout + pipeline stage。
两边的状态模型不同，但都是"显式转换"。

上层可以选择让 RHI 替它记账：在每个资源上存当前状态，`ResourceBarrier` 只给目标状态，
before 由 RHI 查表填。很多引擎这么做。

## 决策

**RHI 不跟踪任何资源状态。调用方给出 before 与 after。**

```cpp
struct BarrierBufferDescriptor {
    Buffer* Target;
    BufferStates Before;
    BufferStates After;
    Nullable<CommandQueue*> OtherQueue;
    bool IsFromOrToOtherQueue;
};
```

`ResourceBarrierDescriptor` 是 buffer 版与 texture 版的 variant，`CommandBuffer::ResourceBarrier`
接一个 span，一次提交一批。

初始状态在创建时固化，不记录：buffer 按 memory type 推导，texture 一律 `COMMON`。

跨队列所有权转移用 `OtherQueue` + `IsFromOrToOtherQueue` 表达（`true` = from，`false` = to）。

后端各自把状态对翻译成原生形式，并做**只影响性能不影响语义**的优化：D3D12 把 UAV→UAV 变成
UAV barrier，把无变化以及 PRESENT↔COMMON 这类等价转换直接跳过；Vulkan 生成
buffer/image memory barrier，access mask 与 layout 由 helper 表映射。

## 放弃的方案及代价

- **RHI 内部跟踪 per-resource 状态**。看起来省事，但它在两处必然出问题：
  一是多线程录制时同一资源被两个 command buffer 触碰，"当前状态"没有单一答案，
  要么加锁要么引入 per-command-buffer 的影子状态表，复杂度立刻超过它省下的；
  二是它把一个隐藏的可变状态放进了 RHI 对象，于是"这次 barrier 会翻译成什么"取决于
  之前录了什么，无法孤立地读一段录制代码。
- **自动 barrier 插入**（在 bind / draw 时推导需要的转换）。要 RHI 理解整帧的资源使用图，
  那是 render graph 的职责，属上层。放在 RHI 会让它必须知道 pass 之间的依赖。
- **只给 after，before 由 RHI 查表**。是上一条的轻量版，同样引入隐藏可变状态，
  且错误表现为"偶尔缺一个 barrier"——这类 bug 在验证层关闭时几乎不可见。
- **切到 D3D12 enhanced barriers 统一两边模型**。它确实更接近 Vulkan 的形状，
  但要求较新的 SDK 与运行时，且当前的显式模型已经能同时映射到两边。

## 必须保持为真

- RHI 对象不存"当前资源状态"。新增字段前先读本文。
- 调用方负责 before / after 的正确性；后端只做等价变换与跳过，绝不改变语义。
- 后端的 barrier 优化必须是纯性能优化：跳过的转换必须真的等价。
- 跨队列转移必须成对出现（release + acquire），且 Vulkan 侧缺 timeline semaphore 时 abort。
- 资源的初始状态由创建路径决定，不要引入"创建后再查一次当前状态"的接口。
