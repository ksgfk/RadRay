#pragma once

// 帧边界等待与延迟销毁: docs/architecture/frame-and-gpu.md

#include <radray/coroutine.h>
#include <radray/runtime_type.h>
#include <radray/types.h>

namespace radray {

/// 帧边界等待器。等到"调用时刻已录制的 GPU work 全部完成"之后恢复调用者。
///
/// 这是 GPU 资源延迟销毁的机制: 不交对象, 而是交出一个挂起点 —— 持有者 co_await 本接口,
/// 恢复后在自己的作用域里正常析构一整包数据, 销毁顺序由该包内的声明顺序显式表达。
///
/// 【必须在主线程恢复】实现方不得在 GPU 完成的那一刻就地恢复 —— 多线程模式下那是渲染
/// 线程, 在那里恢复等于在渲染线程跑资产析构。故实现拆成"标记完成"与"泵恢复"两步。
///
/// 关停时等待者被取消并就地恢复, 协程帧连同捕获的数据一起销毁。调用方不需要为"等不到帧"
/// 写兜底路径, 但必须保证自己的 TaskScope 在实现方 (通常是 GpuSystem) 之前析构。
class IWaitFrameProcessor {
public:
    virtual ~IWaitFrameProcessor() noexcept = default;

    /// 挂起至"调用时刻已录制的 GPU work 全部完成", 之后在主线程恢复。
    /// 【口径是保守的】实现可以多等, 但不得少等 —— 少等一次就是 use-after-free。
    virtual task<void> Wait() = 0;
};

template <>
struct RuntimeTypeTrait<IWaitFrameProcessor> {
    static constexpr RuntimeTypeId value{0x1a7c4d92, 0x63be, 0x4f05, 0x9c, 0x2e, 0x80, 0x47, 0xb6, 0x15, 0xda, 0x38};
};

}  // namespace radray
