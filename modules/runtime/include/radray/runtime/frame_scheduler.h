#pragma once

// 帧循环与所有权契约见 docs/architecture/runtime.md。

#include <radray/runtime/frame_pipeline.h>

namespace radray {

class FrameScheduler final {
public:
    ~FrameScheduler() noexcept;

    FrameScheduler(const FrameScheduler&) = delete;
    FrameScheduler& operator=(const FrameScheduler&) = delete;
    FrameScheduler(FrameScheduler&&) = delete;
    FrameScheduler& operator=(FrameScheduler&&) = delete;

    // 进入时即接管 pipeline；槽数必须为正数，创建后保持不变。
    // DeviceContext 必须晚于本调度器销毁。
    static Nullable<unique_ptr<FrameScheduler>> Create(DeviceContext& device, unique_ptr<FramePipeline> pipeline, uint32_t slotCount = 2);

    uint32_t GetSlotCount() const noexcept;

    // 在场景更新和上传准备之前，等待并回收下一槽。
    // 重复调用保留全部准备结果，直到 RunFrame 或 WaitIdle。
    FrameSlot& BeginFrame() noexcept;
    // 准备、Extract 和 Record 阶段之外返回空值。
    Nullable<FrameSlot*> GetCurrentSlot() noexcept;

    // 仅允许串行调用；返回表示 Record 完成，不保证 GPU 完成；帧错误不可恢复。
    // RunFrame 期间源 Scene 必须存活，且外部不得修改；返回时结束借用。
    // 被引用的 GPU 资源须存活到命令分配器重置，其生命周期独立于 CPU 命令数据。
    // 可写重载在 Extract 之前同步脏网格命令。
    void RunFrame(Scene& source) noexcept;
    void RunFrame(const Scene& source) noexcept;

    // 等待所有已接收帧完成，再清空命令录制、CPU 数据与 arena 对象。
    // 不改变下一槽索引；重复调用不会再次等待已完成的帧。
    void WaitIdle() noexcept;

private:
    FrameScheduler(unique_ptr<FramePipeline> pipeline, vector<unique_ptr<FrameSlot>> slots) noexcept;

    void WaitSlot(FrameSlot& slot) noexcept;
    void RunFrameImpl(const Scene& source, Nullable<Scene*> mutableSource) noexcept;

    // 析构时等待并清理全部槽；须先释放各槽的命令录制，再销毁流水线资源。
    unique_ptr<FramePipeline> _pipeline;
    vector<unique_ptr<FrameSlot>> _slots;
    uint64_t _nextFrameId{0};
    uint32_t _currentSlotIndex{0};
    bool _slotBegun{false};
};

}  // namespace radray
