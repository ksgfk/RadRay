#include <radray/runtime/render_framework/render_queue.h>

#include <algorithm>

#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/shader_asset.h>

namespace radray {

bool DrawList::AddPrimitive(
    MaterialAsset* material,
    PrimitiveSceneProxy* proxy,
    std::string_view passTag,
    uint32_t sectionIndex,
    float viewDistance) noexcept {
    if (material == nullptr || proxy == nullptr) {
        return false;
    }
    ShaderAsset* shader = material->GetShader().Get();
    if (shader == nullptr) {
        return false;
    }
    auto passIdx = shader->FindPassByTag(passTag);
    if (!passIdx.has_value()) {
        return false;  // 该 material 的 shader 没有此 LightMode 的 pass, 整条丢弃
    }
    DrawItem item{};
    item.Material = material;
    item.Proxy = proxy;
    item.PassIndex = passIdx.value();
    item.SectionIndex = sectionIndex;
    item.ViewDistance = viewDistance;
    item.RenderQueue = material->GetRenderQueue();
    _items.emplace_back(item);
    return true;
}

void DrawList::SortOpaque() noexcept {
    std::stable_sort(_items.begin(), _items.end(), [](const DrawItem& a, const DrawItem& b) noexcept {
        if (a.RenderQueue != b.RenderQueue) {
            return a.RenderQueue < b.RenderQueue;
        }
        // 同队列内按 material 身份聚合 (减少状态切换)。
        if (a.Material != b.Material) {
            return a.Material < b.Material;
        }
        // 同 material 内近到远。
        return a.ViewDistance < b.ViewDistance;
    });
}

void DrawList::SortTransparent() noexcept {
    std::stable_sort(_items.begin(), _items.end(), [](const DrawItem& a, const DrawItem& b) noexcept {
        if (a.RenderQueue != b.RenderQueue) {
            return a.RenderQueue < b.RenderQueue;
        }
        // 半透明远到近。
        return a.ViewDistance > b.ViewDistance;
    });
}

}  // namespace radray
