#pragma once

#include <optional>
#include <span>

#include <radray/render/rhi.h>
#include <radray/types.h>

// RenderPass / Framebuffer 的内容去重缓存。
//
// == 为何属 render 层 ==
//
// 它只依赖 rhi.h 里的 Device / RenderPass / Framebuffer / TextureView, 与资产、场景、
// 调度器全都无关。同层已有 SamplerCache (见 rhi.h) 做同一件事 —— 按 descriptor 去重
// GPU 对象 —— 且后端自己就在用它。
//
// == 与 PipelineLayoutCache 的关键差别: 这里不做归一化 ==
//
// PipelineLayoutCache 会把组与 binding 排序, 因为后端建 layout 时自己也排, 顺序不携带
// 语义。RenderPass 相反: ColorAttachments 的下标【就是】渲染目标槽位 (D3D12 的 RTV 序号
// / Vulkan 的 pAttachments 下标), 交换两个附件会得到一个不同的 render pass。故 key 保持
// 原序, 不排序、不合并。
//
// == 生命周期: 缓存拥有全部条目 ==
//
// 与 PipelineLayoutCache 不同, 这里【不做引用计数】: RenderPass 与 Framebuffer 没有跨
// 资产共享的持有者, 只有正在录制的命令缓冲在用它们, 而那是帧内的事。故缓存独占所有权,
// 析构即销毁。
//
// 【Framebuffer 存 TextureView 裸指针】, 所以 view 被销毁前必须调
// RemoveFramebuffersUsing 把引用它的 framebuffer 摘掉。这是本类唯一需要调用方配合的
// 约定, 交换尺寸变化时的后备缓冲 view 就走这条路。

namespace radray::render {

/// GetOrCreateRenderPass 的查表 key。RenderPassDescriptor 内是 span, 故这里持一份副本。
///
/// 【刻意保持原序】: ColorAttachments 的下标就是渲染目标槽位, 排序会改变语义。
struct RenderPassCacheKey {
    vector<RenderPassColorAttachmentDescriptor> ColorAttachments;
    std::optional<RenderPassDepthStencilAttachmentDescriptor> DepthStencilAttachment;

    static RenderPassCacheKey Build(const RenderPassDescriptor& desc);

    /// 返回的 descriptor 内 span 指向本对象, 本对象存活且未被改动期间有效。
    RenderPassDescriptor Get() const noexcept;

    friend bool operator==(const RenderPassCacheKey&, const RenderPassCacheKey&) noexcept = default;
};

/// GetOrCreateFramebuffer 的查表 key。同样是 FramebufferDescriptor 的持有版本。
struct FramebufferCacheKey {
    RenderPass* Pass{nullptr};
    vector<TextureView*> ColorAttachments;
    TextureView* DepthStencilAttachment{nullptr};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t Layers{1};

    static FramebufferCacheKey Build(const FramebufferDescriptor& desc);

    /// 返回的 descriptor 内 span 指向本对象。
    FramebufferDescriptor Get() const noexcept;

    /// 本 framebuffer 是否引用了给定 view。供 RemoveFramebuffersUsing 反查。
    bool References(const TextureView* view) const noexcept;

    friend bool operator==(const FramebufferCacheKey&, const FramebufferCacheKey&) noexcept = default;
};

}  // namespace radray::render

namespace std {

template <>
struct hash<radray::render::RenderPassCacheKey> {
    size_t operator()(const radray::render::RenderPassCacheKey& key) const noexcept;
};

template <>
struct hash<radray::render::FramebufferCacheKey> {
    size_t operator()(const radray::render::FramebufferCacheKey& key) const noexcept;
};

}  // namespace std

namespace radray::render {

/// 按创建参数去重 RenderPass 与 Framebuffer。缓存独占所有权。
class RenderPassRegistry final {
public:
    /// device 必须非空, 且必须活得比本对象久。
    explicit RenderPassRegistry(Device* device) noexcept;
    ~RenderPassRegistry() noexcept;
    RenderPassRegistry(const RenderPassRegistry&) = delete;
    RenderPassRegistry& operator=(const RenderPassRegistry&) = delete;
    RenderPassRegistry(RenderPassRegistry&&) = delete;
    RenderPassRegistry& operator=(RenderPassRegistry&&) = delete;

    /// 返回的指针由本缓存拥有, 在对应条目被清理前有效。
    Nullable<RenderPass*> GetOrCreateRenderPass(const RenderPassDescriptor& desc) noexcept;

    /// desc.Pass 必须来自本缓存的 GetOrCreateRenderPass —— framebuffer 与 pass 的兼容性
    /// 由后端校验, 传别处的 pass 会在 CreateFramebuffer 处失败。
    Nullable<Framebuffer*> GetOrCreateFramebuffer(const FramebufferDescriptor& desc) noexcept;

    /// 摘除并销毁全部引用了 attachment 的 framebuffer。
    ///
    /// 【必须在 view 销毁前调用】: framebuffer 存的是 TextureView 裸指针。交换链尺寸变化
    /// 时重建后备缓冲 view 就走这条路。attachment 为空时无操作。
    /// 返回摘除的条目数, 便于调用方确认确实清到了东西。
    uint32_t RemoveFramebuffersUsing(const TextureView* attachment) noexcept;

    /// 只清 framebuffer, 保留 render pass —— pass 不引用任何 view, 尺寸变化时无需重建。
    void ClearFramebuffers() noexcept;
    void Clear() noexcept;

    /// 【不置空 device】: Clear 只是清空缓存, 之后仍可继续使用。
    Device* GetDevice() const noexcept { return _device; }

    uint32_t GetRenderPassCount() const noexcept { return static_cast<uint32_t>(_passes.size()); }
    uint32_t GetFramebufferCount() const noexcept { return static_cast<uint32_t>(_framebuffers.size()); }
    /// 命中 / 未命中都指【查表结果】, 二者之和即对应 GetOrCreate 的调用次数。
    uint64_t GetRenderPassHitCount() const noexcept { return _renderPassHits; }
    uint64_t GetRenderPassMissCount() const noexcept { return _renderPassMisses; }
    uint64_t GetFramebufferHitCount() const noexcept { return _framebufferHits; }
    uint64_t GetFramebufferMissCount() const noexcept { return _framebufferMisses; }

private:
    Device* _device{nullptr};
    unordered_map<RenderPassCacheKey, unique_ptr<RenderPass>> _passes;
    unordered_map<FramebufferCacheKey, unique_ptr<Framebuffer>> _framebuffers;
    uint64_t _renderPassHits{0};
    uint64_t _renderPassMisses{0};
    uint64_t _framebufferHits{0};
    uint64_t _framebufferMisses{0};
};

}  // namespace radray::render
