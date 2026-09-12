#pragma once

#include <optional>

#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/renderer_list_pass_sets.h>
#include <radray/runtime/forward_pipeline/gen_forward_cbuffers.h>

namespace radray {

enum class ForwardGraphStage : uint8_t {
    Depth,
    Opaque,
    Transparent,
};

/// A graph resource a parameter set binds, named by graph value instead of by handle: the pass
/// declares it while it is being set up and binds it during its prepare stage through the handle the
/// declaration returned. Rows with a valid `Buffer` are buffer rows, every other row is a texture row.
struct ForwardPassResource {
    std::string_view Declaration{};
    bool Write{false};
    RgTextureValue Texture{};
    RgTextureViewDesc TextureView{};
    RgBufferValue Buffer{};
    render::BufferRange BufferRange{render::BufferRange::AllRange()};
    uint32_t StructureByteStride{0};
};

/// Declares one resource on the pass currently being set up and returns the binding row that names
/// the handle the declaration returned; empty when the declaration was rejected.
std::optional<RgParameterBinding> DeclareForwardPassResource(RenderGraphRasterBuilder& builder, const ForwardPassResource& resource);
std::optional<RgParameterBinding> DeclareForwardPassResource(RenderGraphComputeBuilder& builder, const ForwardPassResource& resource);

/// Frame-owned inputs are filled by live work before any pass preparation runs.
struct ForwardGraphPassValues {
    const Forward_PassData* ShadowValues;
    const uint32_t* LocalLightCount;
    RenderExtent Extent;
    uint32_t MaxLightsPerTile;
    bool UseTiles, UseAo, Transparent;
};

struct ForwardGraphView {
    ResolvedRenderView View;
    const RendererList* List{nullptr};
    /// Rows of each program's pass group that name no graph resource: constants and samplers. Copied
    /// into the pass payload during setup, so this storage only has to survive BuildGraph.
    std::span<const RendererListProgramParameters> Parameters{};
    /// Graph resources every pass group of this view binds. Declared once while the stage is set up
    /// and appended to every program's rows as the handles those declarations returned.
    std::span<const ForwardPassResource> Resources{};
    /// Internal stable stage: declarations use Resources, and the live list supplies programs in Prepare.
    std::optional<ForwardGraphPassValues> PassValues{};
    RgWorkHandle Work{};
    uint64_t WorkMask{1};
};

struct ForwardGraphStageInputs {
    std::string_view Name;
    render::RenderBackend Backend{render::RenderBackend::MAX_COUNT};
    std::span<const ForwardGraphView> Views;
    RgTextureValue Color{};
    RgTextureValue Depth{};
    RgColorAttachmentDesc ColorAttachment{};
    RgDepthAttachmentDesc DepthAttachment{};
    DrawExecutionStats* Execution{nullptr};
    bool PreserveEmptyPass{false};
    std::span<const RgTextureValue> AuxiliaryColors{};
};

struct ForwardGraphStageOutput {
    RgTextureValue Color{};
    RgTextureValue Depth{};
    RgPassHandle Pass{};
    /// The pass was added and its attachments and accesses were declared. Parameter sets and draw
    /// data are built in the pass prepare stage, so a failure there fails graph execution instead of
    /// showing up here.
    bool Success{false};
};

/// Per-instance view/list inputs and prepared recording data. Static graph rows stay in the
/// immutable stage recipe; this object owns only the values of the current frame.
class ForwardGraphFrameData {
public:
    struct Impl;
    ForwardGraphFrameData();
    ~ForwardGraphFrameData();

private:
    friend class ForwardGraph;
    unique_ptr<Impl> _impl;
};

/// Adds one reusable forward-rendering stage to an existing graph. RendererList pointers are
/// borrowed until that graph executes; view values are copied into the callback payload.
class ForwardGraph {
public:
    static ForwardGraphStageOutput BuildGraph(
        RenderGraph& graph, ForwardGraphStage stage,
        const ForwardGraphStageInputs& inputs);
    static shared_ptr<ForwardGraphFrameData> MakeFrame(const ForwardGraphStageInputs& inputs);
    static ForwardGraphStageOutput DeclareTemplate(
        RenderGraph& graph, ForwardGraphStage stage, RgTemplateSlot<ForwardGraphFrameData> frame,
        const ForwardGraphStageInputs& inputs);
};

}  // namespace radray
