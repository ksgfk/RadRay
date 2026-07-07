#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include <radray/types.h>
#include <radray/runtime/material_asset.h>

namespace radray {

class PrimitiveSceneProxy;

/// 一条待绘制项 (对应 Unity 的一个 draw / UE 的 mesh batch element)。
/// Material 为渲染侧只读值快照 (shared_ptr 保活): 即便源 MaterialAsset 在渲染期间被 Unload,
/// 快照仍存活, 无悬垂; render 线程只读, 无竞争。proxy 生命周期由 scene 管理。
struct DrawItem {
    shared_ptr<const MaterialRenderSnapshot> Material{};
    PrimitiveSceneProxy* Proxy{nullptr};
    uint32_t PassIndex{0};       // material 快照的 shader 中匹配到的 pass 序号
    uint32_t SectionIndex{0};    // proxy 内的 section (sub-mesh)
    float ViewDistance{0.0f};    // 到相机的距离 (排序用)
    int32_t RenderQueue{0};      // 取自快照, 冗余存储避免排序时反复解引用
};

/// 一批待绘制项 + 过滤/排序。对应 Unity 的 DrawingSettings + SortingSettings 的最小化。
///
/// 设计要点:
/// - 纯 CPU 策略, 不接触 device, 可 headless 测试。
/// - AddPrimitive 按 PassTag (=LightMode) 过滤: material 的 shader 若无该 tag 的 pass, 整条丢弃。
/// - 排序:
///   - SortOpaque:  先按 RenderQueue, 再按 material 身份 (状态批处理), 再 front-to-back。
///   - SortTransparent: 先按 RenderQueue, 再 back-to-front。
class DrawList {
public:
    DrawList() noexcept = default;

    void Clear() noexcept { _items.clear(); }
    bool Empty() const noexcept { return _items.empty(); }
    size_t Size() const noexcept { return _items.size(); }

    std::span<const DrawItem> Items() const noexcept { return _items; }
    std::span<DrawItem> Items() noexcept { return _items; }

    /// 尝试为一个 (material 快照, proxy, section) 生成 draw item, 按 passTag 过滤。
    /// 快照 / shader 为空, 或 shader 无 passTag 对应的 pass, 返回 false (不加入)。
    bool AddPrimitive(
        shared_ptr<const MaterialRenderSnapshot> material,
        PrimitiveSceneProxy* proxy,
        std::string_view passTag,
        uint32_t sectionIndex = 0,
        float viewDistance = 0.0f) noexcept;

    /// 不透明排序: RenderQueue 升序 -> material 指针 (批处理) -> ViewDistance 升序 (近到远)。
    void SortOpaque() noexcept;

    /// 半透明排序: RenderQueue 升序 -> ViewDistance 降序 (远到近)。
    void SortTransparent() noexcept;

private:
    vector<DrawItem> _items;
};

}  // namespace radray
