#pragma once

#include <radray/coroutine.h>
#include <radray/runtime_type.h>
#include <radray/types.h>

namespace radray {

/// 帧边界等待器。等到"调用时刻已录制的 GPU work 全部完成"之后恢复调用者。
///
/// == 为何需要它 ==
///
/// GPU 资源不能在逻辑所有者死亡的那一刻销毁 —— 它可能仍被上一帧录进命令列表、GPU 还在
/// 读。传统做法是一个 fire-and-forget 的回收队列 (交出对象 + 记下目标 fence 值, 由某处
/// 轮询筛出已完成的)。那套做法要求每个持有者把对象【逐个】交出去, 于是销毁顺序退化为
/// 队列的入队顺序 —— 而 "view 必须先于 texture 死" 这类约束就此变成一条隐式纪律。
///
/// 本接口把它翻过来: 不交对象, 而是【交出一个挂起点】。持有者 co_await 本接口, 恢复后
/// 在自己的作用域里正常析构一整包数据, 销毁顺序由该包内的声明顺序显式表达。
///
/// == 恢复线程的约定 ==
///
/// 【必须在主线程恢复】。实现方不得在 GPU 完成的那一刻就地恢复 —— 多线程模式下
/// "flight 完成" 是渲染线程观察到的 (见 ThreadedRunner::RetireRenderedFrames →
/// GpuSystem::CompleteFlight), 在那里恢复就等于在渲染线程跑资产析构。故实现必须拆成
/// "标记完成" 与 "泵恢复" 两步, 后者由主线程的帧循环驱动 (见 GpuSystem::PumpWaitFrame)。
///
/// == 取消 ==
///
/// 关停时等待者会被 stop token 取消并就地恢复, 此时协程帧连同它捕获的数据一起销毁。
/// 所以调用方【不需要】为"等不到帧了"写兜底路径, 但必须保证自己的 TaskScope 在
/// 实现方 (通常是 GpuSystem) 之前析构 —— 否则取消时的析构会碰到已死的 device。
class IWaitFrameProcessor {
public:
    virtual ~IWaitFrameProcessor() noexcept = default;

    /// 挂起至"调用时刻已录制的 GPU work 全部完成", 之后在主线程恢复。
    ///
    /// 【口径是保守的】: 实现可以多等 (例如等满当前 flight 的 fence), 但不得少等。
    /// 一次多余的等待只是让内存晚一帧归还, 而少等一次就是 use-after-free。
    virtual task<void> Wait() = 0;
};

template <>
struct RuntimeTypeTrait<IWaitFrameProcessor> {
    static constexpr RuntimeTypeId value{0x1a7c4d92, 0x63be, 0x4f05, 0x9c, 0x2e, 0x80, 0x47, 0xb6, 0x15, 0xda, 0x38};
    using Bases = std::tuple<>;
};

}  // namespace radray
