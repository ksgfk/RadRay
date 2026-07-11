#include <radray/runtime/render_framework/render_queue.h>

#include <algorithm>

#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/shader_asset.h>

namespace radray {

bool DrawList::AddPrimitive(
    shared_ptr<const MaterialRenderSnapshot> material,
    PrimitiveSceneProxy* proxy,
    std::string_view passTag,
    uint32_t sectionIndex,
    float viewDistance) noexcept {
    if (material == nullptr || proxy == nullptr) {
        return false;
    }
    ShaderAsset* shader = material->Shader.Get();
    if (shader == nullptr) {
        return false;
    }
    auto passIdx = shader->FindPassByTag(passTag);
    if (!passIdx.has_value()) {
        return false;  // 该 material 的 shader 没有此 LightMode 的 pass, 整条丢弃
    }
    DrawItem item{};
    item.PassIndex = passIdx.value();
    item.SectionIndex = sectionIndex;
    item.ViewDistance = viewDistance;
    item.RenderQueue = material->RenderQueue;
    item.Proxy = proxy;
    item.Material = std::move(material);
    _items.emplace_back(std::move(item));
    return true;
}

void DrawList::SortOpaque() noexcept {
    std::stable_sort(_items.begin(), _items.end(), [](const DrawItem& a, const DrawItem& b) noexcept {
        if (a.RenderQueue != b.RenderQueue) {
            return a.RenderQueue < b.RenderQueue;
        }
        // 同队列内按不可变材质内容聚合，快照分配地址不参与排序。
        if (a.Material->BindingKey != b.Material->BindingKey) {
            return std::tie(a.Material->BindingKey.Lo, a.Material->BindingKey.Hi) <
                   std::tie(b.Material->BindingKey.Lo, b.Material->BindingKey.Hi);
        }
        // 同 material 内近到远。
        return a.ViewDistance < b.ViewDistance;
    });
    _sortMode = SortMode::Opaque;
}

void DrawList::SortTransparent() noexcept {
    std::stable_sort(_items.begin(), _items.end(), [](const DrawItem& a, const DrawItem& b) noexcept {
        if (a.RenderQueue != b.RenderQueue) {
            return a.RenderQueue < b.RenderQueue;
        }
        // 半透明远到近。
        return a.ViewDistance > b.ViewDistance;
    });
    _sortMode = SortMode::Transparent;
}

}  // namespace radray
