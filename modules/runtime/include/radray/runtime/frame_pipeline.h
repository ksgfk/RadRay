#pragma once

// 帧循环与所有权契约见 docs/architecture/runtime.md。

#include <radray/runtime/frame_slot.h>
#include <radray/runtime/scene.h>

namespace radray {

class FramePipeline {
public:
    virtual ~FramePipeline() noexcept;

    // 同步更新缓存期间持续借用登记项与处理器。
    virtual std::span<const MeshPassRegistration> GetMeshPassRegistrations() noexcept { return {}; }

    // 回调串行执行，不得重入调度器；Extract 不得提交异步工作。
    // Extract 与 Record 期间，源 Scene 始终保持只读。
    // Record 之后仍需使用的数据保存在槽内；Wait 不得解引用借用的 Scene。
    // 帧内对象使用 slot.GetArena() 分配，退役资源所有者交给 slot.RetireResource()。
    // 帧错误不可恢复；报告诊断并终止程序，不得返回未完成的工作。
    virtual void Extract(const Scene& source, FrameSlot& slot) noexcept = 0;

    // 准备数据，从槽内取得命令缓冲，完成 Begin/End 并提交到 slot.GetQueue()。
    // 持续借用源 Scene 及其缓存命令，直到 RunFrame 返回。
    // 可将帧资源所有者移入 slot.GetArena()，使其存活到 Wait 结束。
    // 返回前必须关闭所有编码器与命令录制；返回后必须调用 Wait，空帧也不例外。
    // 帧错误不可恢复，不提供回滚或重试协议。
    virtual void Record(const Scene& source, FrameSlot& slot) noexcept = 0;

    // 在 Record 之后调用；仅当槽内全部提交完成，且命令存储、CPU 数据与 arena 对象
    // 可以安全重置或销毁时才可返回。
    // arena 对象在 Wait 期间持续存活；仅由调度器触发回收。
    virtual void Wait(FrameSlot& slot) noexcept = 0;
};

}  // namespace radray
