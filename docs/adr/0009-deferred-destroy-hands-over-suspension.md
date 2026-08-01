# ADR-0009 延迟销毁交出挂起点，不交对象

状态: 生效
日期: 2026-07
影响: `IWaitFrameProcessor`（`wait_frame.h`）、`AssetManager::DeferDestroy`、`GpuSystem` 的 per-flight 等待表

## 背景

GPU 资源不能在逻辑所有者死亡的那一刻销毁——它可能仍被上一帧录进命令列表、GPU 还在读。

传统做法是一个 fire-and-forget 的回收队列：交出对象 + 记下目标 fence 值，由某处轮询筛出
已完成的。曾经的接口就是这样：`IRenderResourceRecycler::RecycleRenderResource`。

那套做法要求每个持有者把对象**逐个**交出去，于是销毁顺序退化为队列的入队顺序。
而 "view 必须先于 texture 死" 这类约束就此变成一条隐式纪律——无法在类型上表达，
也无法在 review 中看见。

## 决策

**把它翻过来：不交对象，而是交出一个挂起点。**

```cpp
void MyAsset::OnUnload(AssetManager& manager) override {
    manager.DeferDestroy([tex = std::move(_texture), view = std::move(_view)]() mutable {});
}
```

持有者 `co_await` 帧边界，恢复后在自己的作用域里正常析构一整包数据。销毁顺序由该包内的
声明顺序**显式表达**。

`IWaitFrameProcessor::Wait()` 挂起至"调用时刻已录制的 GPU work 全部完成"。

**口径是保守的**：实现可以多等（例如等满当前 flight 的 fence），但不得少等。一次多余的
等待只是让内存晚一帧归还，而少等一次就是 use-after-free。

**必须在主线程恢复。** 实现方不得在 GPU 完成的那一刻就地恢复——多线程模式下"flight 完成"
是渲染线程观察到的（`ThreadedRunner::RetireRenderedFrames` → `GpuSystem::CompleteFlight`），
在那里恢复就等于在渲染线程跑资产析构。故实现拆成两步：
`CompleteFlight` 只标记 `FlightComplete`，真正的 resume 由主线程的
`GpuSystem::PumpWaitFrame` 做。

**等待表挂在 per-flight 槽位上，不是一张全局表。** "等到已录制的 work 完成"天然是
per-flight 的问题——flight 的 fence 就是那个完成条件，记在槽位上便无需另存 fence 值再
逐个比较。

**关停时取消等待者**，此时协程帧连同它捕获的数据一起销毁。所以调用方**不需要**为
"等不到帧了"写兜底路径。

**一帧一个协程帧**：同一次 `Pump` 内的全部 payload 攒成一批，共用一个等待帧边界的协程。

## 放弃的方案及代价

- **逐对象交出的回收队列**（曾经的 `IRenderResourceRecycler`）。销毁顺序寄托在队列语义上，
  是一条隐式契约。且每种资源类型都要在队列里有一个对应的擦除包装。
- **在 GPU 完成的那一刻就地 resume**。多线程模式下会在渲染线程跑资产析构。
  资产系统是单线程的（引用计数是普通整数、增减要触碰 manager 的表），这会直接踩坏它。
- **记下精确的 fence 值再比较**（而不是等满当前 flight）。省下最多一轮的等待，
  代价是每条记录多存一个 fence 值 + 一次比较，且要处理 fence 回绕。
  口径本就允许多等，不值得。
- **每个 payload 一个协程帧**。大量资产同时归零时会产生大量协程帧。
- **关停时不取消等待者**。挂在未提交 flight 上的记录永远等不到 fence，
  不取消就是协程帧连同它捕获的 GPU 对象一起泄漏。

## 必须保持为真

- `IWaitFrameProcessor::Wait()` 的实现分两步：标记完成（可能在渲染线程）+ 主线程泵恢复。
- 实现只允许多等，不允许少等。
- `Asset::OnUnload` 只交出**不能立刻析构**的东西。纯 CPU 数据交给析构函数。
- `DeferDestroy` 的 payload 内部声明顺序表达销毁顺序。不要依赖多次 `DeferDestroy` 的
  调用先后。
- 调用方的 `TaskScope` 必须在 `IWaitFrameProcessor` 实现方（通常是 `GpuSystem`）之前析构
  —— 否则取消时的析构会碰到已死的 device。
- 关停时 `CancelAllWaitFrames` 必须覆盖全部 flight。
