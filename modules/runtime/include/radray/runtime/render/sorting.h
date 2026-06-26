#pragma once

#include <cstdint>

namespace radray::srp {

/// 排序标志。对应 Unity `SortingCriteria`(最小子集):
/// 不透明 pass 用 front-to-back(减少 overdraw),透明 pass 用 back-to-front(正确混合)。
/// 源码:DrawObjectsPass 按 m_IsOpaque 选 CommonOpaque / CommonTransparent。
enum class SortingCriteria : uint8_t {
    None = 0,
    FrontToBack,  ///< CommonOpaque:近→远
    BackToFront,  ///< CommonTransparent:远→近
};

/// 渲染队列区间。对应 Unity 的 renderQueue,决定 renderer 落不透明还是透明 pass。
/// material 的 BlendMode 映射到一个 RenderQueue 值;FilteringSettings 用区间过滤。
enum class RenderQueue : uint16_t {
    Background = 1000,
    Opaque = 2000,
    AlphaTest = 2450,   ///< Unity: masked 落在 opaque 末尾
    Transparent = 3000,
    Overlay = 4000,
};

/// 队列区间过滤(对应 Unity `RenderQueueRange`)。闭区间 [Min, Max]。
struct RenderQueueRange {
    uint16_t Min{0};
    uint16_t Max{0xFFFF};

    constexpr bool Contains(RenderQueue q) const noexcept {
        auto v = static_cast<uint16_t>(q);
        return v >= Min && v <= Max;
    }

    /// 不透明区间:[0, AlphaTest]。masked 也算不透明(与 Unity 一致)。
    static constexpr RenderQueueRange Opaque() noexcept {
        return RenderQueueRange{0, static_cast<uint16_t>(RenderQueue::AlphaTest)};
    }

    /// 透明区间:(AlphaTest, +inf]。
    static constexpr RenderQueueRange Transparent() noexcept {
        return RenderQueueRange{static_cast<uint16_t>(RenderQueue::AlphaTest) + 1, 0xFFFF};
    }

    /// 全部。
    static constexpr RenderQueueRange All() noexcept {
        return RenderQueueRange{0, 0xFFFF};
    }
};

}  // namespace radray::srp
