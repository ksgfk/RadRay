#pragma once

#include <radray/runtime/render_framework/renderer_list.h>

namespace radray {

// A deferred diagnostic must reject a changed source before traversing its borrowed draw data.
struct RendererListValidationSource {
    explicit RendererListValidationSource(const RendererList& source) noexcept
        : List(&source), Revision(source.GetBuildRevision()), Resources(source.GetFrameResources()),
          Epoch(source.GetFrameEpoch()), Commands(source.Commands.data()), Items(source.Items.data()),
          CommandCount(source.Commands.size()), ItemCount(source.Items.size()), DrawCount(source.GetDrawCount()) {}

    bool IsCurrent() const noexcept {
        return List->IsCurrent() && List->GetBuildRevision() == Revision &&
               List->GetFrameResources() == Resources && List->GetFrameEpoch() == Epoch &&
               List->Commands.data() == Commands.Get() && List->Items.data() == Items.Get() &&
               List->Commands.size() == CommandCount && List->Items.size() == ItemCount && List->GetDrawCount() == DrawCount;
    }

    const RendererList* List;
    uint64_t Revision;
    Nullable<const FrameDrawResources*> Resources;
    uint64_t Epoch;
    Nullable<const MeshDrawCommand*> Commands;
    Nullable<const RendererListItem*> Items;
    size_t CommandCount, ItemCount, DrawCount;
};

// Publicly assembled draw orders need bounds checks before any indexed lookup, in every mode.
inline bool HasSafeRendererListOrder(const RendererList& list) noexcept {
    if (list.Items.empty()) return true;
    const auto count = list.GetDrawCount();
    if (list.Items.size() != count) return false;
    for (const auto& item : list.Items)
        if (item.CommandIndex >= count) return false;
    return true;
}

}  // namespace radray
