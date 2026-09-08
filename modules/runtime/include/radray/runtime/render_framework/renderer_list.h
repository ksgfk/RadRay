#pragma once

#include <radray/runtime/render_framework/culling.h>
#include <radray/runtime/render_framework/mesh_draw_command.h>

namespace radray {

class MeshPassProcessor;

struct RenderQueueRange {
    int32_t Min{std::numeric_limits<int32_t>::min()}, Max{std::numeric_limits<int32_t>::max()};
    bool Contains(RenderQueue queue) const noexcept { return Min <= static_cast<int32_t>(queue) && static_cast<int32_t>(queue) <= Max; }
    static RenderQueueRange Opaque() noexcept { return {std::numeric_limits<int32_t>::min(), static_cast<int32_t>(RenderQueue::GeometryLast) - 1}; }
    static RenderQueueRange Transparent() noexcept { return {static_cast<int32_t>(RenderQueue::GeometryLast), std::numeric_limits<int32_t>::max()}; }
};
enum class RendererListSorting : uint8_t { StateThenFrontToBack,
                                           FrontToBack,
                                           BackToFront };
struct RendererListDesc {
    string Name;
    string MaterialPassName;
    Nullable<const CullingResults*> Culling{nullptr};
    Nullable<const ResolvedRenderView*> View{nullptr};
    RenderQueueRange QueueRange;
    uint32_t LayerMask{0xffffffffu};
    RendererListSorting Sorting{RendererListSorting::StateThenFrontToBack};
    bool RequireMaterialPass{false};
};
struct RendererListStats {
    uint64_t VisiblePrimitives{0}, ConsideredBatches{0}, LayerRejected{0}, QueueRejected{0}, MissingPass{0};
    uint64_t InvalidBindings{0}, InvalidGeometry{0}, PrepareResourceFailed{0}, ProcessorRejected{0}, Commands{0}, NonFiniteDepth{0};
    bool Valid{false};
    uint64_t MissingRequiredPass{0};
    bool ContentSucceeded() const noexcept {
        return Valid && MissingRequiredPass == 0 && InvalidBindings == 0 && InvalidGeometry == 0 && PrepareResourceFailed == 0 && ProcessorRejected == 0;
    }
};
struct RendererListItem {
    DrawSortData SortData;
    uint32_t CommandIndex{0};
};
struct RendererList {
    // Payloads remain in publication order; Items alone defines the sorted execution order.
    // Empty Items uses publication order for explicitly assembled lists.
    // Nonempty Items must be a permutation of Commands; PrepareRendererList validates it before use.
    vector<MeshDrawCommand> Commands;
    vector<RendererListItem> Items;
    RendererListStats Stats;
    std::span<const RendererListItem> GetItems() const noexcept { return Items; }
    // The caller supplies an in-range index and a valid item permutation.
    const MeshDrawCommand& GetCommand(size_t executionIndex) const noexcept {
        return Commands[Items.empty() ? executionIndex : Items[executionIndex].CommandIndex];
    }
    void ResetForReuse() noexcept {
        Items.clear();
        Commands.clear();
        Stats = {};
    }
};

bool BuildRendererList(const RendererListDesc& desc, MeshPassProcessor& processor, RendererList& out);

}  // namespace radray
