#pragma once

// 帧循环与所有权契约见 docs/architecture/runtime.md。

#include <radray/runtime/frame_slot.h>

namespace radray {

class FramePipeline {
public:
    virtual ~FramePipeline() noexcept;

    // 同步更新缓存期间持续借用登记项与处理器。
    virtual std::span<const MeshPassRegistration> GetMeshPassRegistrations() noexcept { return {}; }

    // 阶段串行执行，不得重入调度器；输入借用保持到 Submit 返回。
    // 帧错误不可恢复；正常返回表示该阶段全部完成。
    virtual void Extract(FrameSlot& slot) noexcept = 0;
    virtual void Prepare(FrameSlot& slot) noexcept = 0;
    // 仅录制已准备的数据；返回前关闭 encoder 并 End command buffer。
    virtual void Record(FrameSlot& slot) noexcept = 0;
    // 正常提交后才发布状态；保存本槽完成信息供 Wait 使用。
    virtual void Submit(FrameSlot& slot) noexcept = 0;
    // 等待实际提交完成；返回后仍须重置命令存储才能释放帧内对象。
    virtual void Wait(FrameSlot& slot) noexcept = 0;
};

}  // namespace radray
