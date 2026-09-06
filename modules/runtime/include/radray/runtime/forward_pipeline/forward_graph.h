#pragma once

#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/renderer_list_pass_bindings.h>

namespace radray {

enum class ForwardGraphStage : uint8_t {
    Depth,
    Opaque,
    Transparent,
};

struct ForwardGraphView {
    ResolvedRenderView View;
    const RendererList* List{nullptr};
    std::span<const RendererListProgramParameters> Parameters{};
};

struct ForwardGraphStageInputs {
    std::string_view Name;
    render::RenderBackend Backend{render::RenderBackend::MAX_COUNT};
    std::span<const ForwardGraphView> Views;
    RgTextureHandle Color{};
    RgTextureHandle Depth{};
    RgColorAttachmentDesc ColorAttachment{};
    RgDepthAttachmentDesc DepthAttachment{};
    DrawExecutionStats* Execution{nullptr};
    bool PreserveEmptyPass{false};
    std::span<const RgTextureHandle> AuxiliaryColors{};
};

struct ForwardGraphStageOutput {
    RgTextureHandle Color{};
    RgTextureHandle Depth{};
    RgPassHandle Pass{};
    bool Success{false};
};

/// Adds one reusable forward-rendering stage to an existing graph. RendererList pointers are
/// borrowed until that graph executes; view values are copied into the callback payload.
class ForwardGraph {
public:
    static ForwardGraphStageOutput BuildGraph(
        RenderGraph& graph, ForwardGraphStage stage,
        const ForwardGraphStageInputs& inputs);
};

}  // namespace radray
