#pragma once

#include <span>

#include <radray/runtime/render_framework/culling.h>
#include <radray/runtime/render_framework/mesh_draw_command.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/render_graph_runtime_options.h>

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
    RenderValidationMode Validation{RenderValidationMode::Full};
    PassPolicyId Policy{};
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
struct RendererListProgramUse {
    Nullable<ShaderProgram*> Program{nullptr};
    uint64_t ProgramGeneration{0};
    Nullable<const StaticBindingRecipe*> Bindings{nullptr};
};
class RendererDrawGroupsView {
public:
    RendererDrawGroupsView() = default;
    explicit RendererDrawGroupsView(std::span<const PreparedShaderGroup> groups) noexcept : _groups(groups) {}
    RendererDrawGroupsView(const FrameDrawResources* resources, FrameDrawBindingId binding) noexcept : _resources(resources), _binding(binding) {}
    size_t size() const noexcept { return _resources ? _resources->GetBinding(_binding).size() : _groups.size(); }
    bool empty() const noexcept { return size() == 0; }
    const PreparedShaderGroup& operator[](size_t index) const noexcept {
        return _resources ? _resources->GetGroup(_resources->GetBinding(_binding)[index]) : _groups[index];
    }

private:
    Nullable<const FrameDrawResources*> _resources{nullptr};
    FrameDrawBindingId _binding;
    std::span<const PreparedShaderGroup> _groups;
};
struct RendererList {
    RendererList() = default;
    RendererList(const RendererList& other);
    RendererList& operator=(const RendererList& other);
    RendererList(RendererList&& other) noexcept;
    RendererList& operator=(RendererList&& other) noexcept;
    // Commands owns only explicitly assembled/custom dynamic fallbacks. Static draws hold indices.
    vector<MeshDrawCommand> Commands;
    vector<RendererListItem> Items;
    RendererListStats Stats;
    std::span<const RendererListItem> GetItems() const noexcept { return Items; }
    size_t GetDrawCount() const noexcept { return _draws.empty() ? Commands.size() : _draws.size(); }
    const MeshDrawDescription& GetDescription(size_t executionIndex) const noexcept;
    const MaterialPipelineState& GetPipelineState(size_t executionIndex) const noexcept;
    RendererDrawGroupsView GetGroups(size_t executionIndex) const noexcept;
    FrameDrawBindingId GetBindingId(size_t executionIndex) const noexcept;
    std::span<const RendererListProgramUse> GetPrograms() const noexcept { return _programs; }
    uint64_t GetEffectiveStateId(size_t executionIndex) const noexcept;
    std::span<const CpuVertexBindingRun> GetVertexBindingRuns(size_t executionIndex) const noexcept;
    uint64_t GetBuildRevision() const noexcept { return _buildRevision; }
    uint64_t GetFrameEpoch() const noexcept { return _frameEpoch; }
    Nullable<const FrameDrawResources*> GetFrameResources() const noexcept { return _resources; }
    bool IsCurrent() const noexcept;
    /// Legacy callers may use this only for a dynamic draw. Static draws have no MeshDrawCommand.
    const MeshDrawCommand& GetCommand(size_t executionIndex) const noexcept;
    void ResetForReuse() noexcept;
    bool AppendStatic(const RenderSceneSnapshot& scene, uint32_t recordIndex, FrameDrawResources& resources, FrameDrawBindingId binding);
    bool AppendDynamic(MeshDrawCommand&& command, Nullable<FrameDrawResources*> resources = nullptr);
    void AddProgram(RendererListProgramUse use);

private:
    struct Draw {
        uint32_t Source{0};
        FrameDrawBindingId Binding;
        bool Dynamic{false};
    };
    Nullable<const Draw*> FindDraw(size_t executionIndex) const noexcept;
    void AdvanceRevision() noexcept;
    vector<Draw> _draws;
    vector<CpuGeometryBindingPlan> _dynamicGeometryPlans;
    vector<RendererListProgramUse> _programs;
    Nullable<const RenderSceneSnapshot*> _scene{nullptr};
    Nullable<const FrameDrawResources*> _resources{nullptr};
    uint64_t _frameEpoch{0}, _publicationId{0}, _sceneEpoch{0}, _publicationRevision{0}, _buildRevision{1};
};

bool BuildRendererList(const RendererListDesc& desc, MeshPassProcessor& processor, RendererList& out);
/// One traversal of a shared Culling/View into several lists. `descs` and `outs` must be the same size.
bool BuildRendererLists(std::span<const RendererListDesc> descs, MeshPassProcessor& processor, std::span<RendererList*> outs);

}  // namespace radray
