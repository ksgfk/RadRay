#include <radray/runtime/forward_pipeline/forward_graph.h>

#include <algorithm>
#include <radray/profiler.h>
#include <radray/runtime/render_framework/viewport.h>

namespace radray {
namespace {

struct ForwardGraphPassData {
    struct ViewData {
        ResolvedRenderView View;
        const RendererList* List{nullptr};
        // Everything the prepare stage reads is owned here: growing the outer vectors relocates the
        // inner buffers without copying them, so the spans below stay valid.
        vector<vector<byte>> Constants;
        vector<vector<RgParameterBinding>> Rows;
        vector<RendererListProgramParameters> Parameters;
        // Sets is declared before Prepared: the prepared draws hold spans into it.
        std::optional<RendererListPassSets> Sets;
        std::optional<PreparedRendererList> Prepared;
    };

    render::RenderBackend Backend{render::RenderBackend::MAX_COUNT};
    vector<ViewData> Views;
    DrawExecutionStats* Execution{nullptr};
};

/// Copies one program's non-graph rows into the payload, repointing every cbuffer row at payload
/// storage, and appends the rows of the resources this pass already declared.
void CopyProgramRows(const RendererListProgramParameters& program, std::span<const RgParameterBinding> shared,
                     ForwardGraphPassData::ViewData& view) {
    auto& rows = view.Rows.emplace_back();
    rows.reserve(program.Bindings.size() + shared.size());
    for (const RgParameterBinding& binding : program.Bindings) {
        RgParameterBinding& row = rows.emplace_back(binding);
        if (const auto* bytes = std::get_if<RgCBufferParameterBinding>(&row.Value)) {
            const auto& owned = view.Constants.emplace_back(bytes->Bytes.begin(), bytes->Bytes.end());
            row.Value = RgCBufferParameterBinding{owned};
        }
    }
    rows.insert(rows.end(), shared.begin(), shared.end());
    view.Parameters.push_back({program.Program, program.Group, rows});
}

bool PrepareForwardGraphPass(ForwardGraphPassData& data, RenderGraphPrepareContext& context) {
    for (ForwardGraphPassData::ViewData& view : data.Views) {
        if (!view.Parameters.empty()) {
            view.Sets = RendererListPassSets::Create(context, *view.List, view.Parameters);
            if (!view.Sets) return false;
        }
        view.Prepared = PrepareRendererList(*view.List, context, view.Sets ? &*view.Sets : nullptr);
        if (!view.Prepared) return false;
    }
    return true;
}

void ExecuteForwardGraphPass(
    const ForwardGraphPassData& data, RenderGraphRasterContext& context) {
    for (const ForwardGraphPassData::ViewData& view : data.Views) {
        if (!view.Prepared) continue;
        context.Encoder().SetViewport(MakeViewport(
            data.Backend, static_cast<float>(view.View.ViewRect.X),
            static_cast<float>(view.View.ViewRect.Y),
            static_cast<float>(view.View.ViewRect.Width),
            static_cast<float>(view.View.ViewRect.Height)));
        context.Encoder().SetScissor(view.View.ScissorRect);
        RecordRendererList(*view.Prepared, context, *data.Execution);
    }
}

std::optional<RgParameterBinding> DeclareBuffer(RenderGraphPassBuilder& builder, const ForwardPassResource& resource) {
    const RgBufferValue declared = resource.Write
                                       ? builder.WriteBuffer(resource.Buffer, RgBufferAccess::UnorderedAccess, resource.BufferRange)
                                       : builder.ReadBuffer(resource.Buffer, RgBufferAccess::ShaderRead, resource.BufferRange);
    if (!declared.IsValid()) return std::nullopt;
    return RgParameterBinding{resource.Declaration, 0, RgBufferParameterBinding{declared, resource.BufferRange, resource.StructureByteStride}};
}

std::optional<RgParameterBinding> TextureRow(const ForwardPassResource& resource, RgTextureViewHandle view) {
    if (!view.IsValid()) return std::nullopt;
    return RgParameterBinding{resource.Declaration, 0, RgTextureParameterBinding{view}};
}

}  // namespace

std::optional<RgParameterBinding> DeclareForwardPassResource(RenderGraphRasterBuilder& builder, const ForwardPassResource& resource) {
    if (resource.Buffer.IsValid()) return DeclareBuffer(builder, resource);
    return TextureRow(resource, resource.Write
                                    ? builder.WriteTexture(resource.Texture, render::ShaderStages{render::ShaderStage::Graphics}, resource.TextureView)
                                    : builder.ReadTexture(resource.Texture, resource.TextureView));
}

std::optional<RgParameterBinding> DeclareForwardPassResource(RenderGraphComputeBuilder& builder, const ForwardPassResource& resource) {
    if (resource.Buffer.IsValid()) return DeclareBuffer(builder, resource);
    return TextureRow(resource, resource.Write ? builder.WriteTexture(resource.Texture, resource.TextureView)
                                               : builder.ReadTexture(resource.Texture, resource.TextureView));
}

ForwardGraphStageOutput ForwardGraph::BuildGraph(
    RenderGraph& graph, ForwardGraphStage stage,
    const ForwardGraphStageInputs& inputs) {
    RADRAY_PROFILE_SCOPE_N("ForwardGraph::BuildGraph");
    ForwardGraphStageOutput result{
        .Color = inputs.Color,
        .Depth = inputs.Depth,
        .Pass = {},
        .Success = false};
    if (!EnumContains(stage) || !EnumContains(inputs.Backend) ||
        !inputs.Depth.IsValid() || inputs.Execution == nullptr ||
        (stage != ForwardGraphStage::Depth && !inputs.Color.IsValid())) {
        return result;
    }
    for (const ForwardGraphView& view : inputs.Views) {
        if (view.List == nullptr) return result;
    }

    // A stage without draws is not declared at all: an unconditional zero-draw stage would produce
    // the next color version and record a full-resolution Load/Store render pass every frame.
    const bool hasCommands = std::any_of(
        inputs.Views.begin(), inputs.Views.end(),
        [](const ForwardGraphView& view) { return !view.List->Commands.empty(); });
    if (!hasCommands && stage != ForwardGraphStage::Opaque && !inputs.PreserveEmptyPass) {
        result.Success = true;
        return result;
    }

    if (stage != ForwardGraphStage::Depth) result.Color = graph.NextVersion(inputs.Color);
    const bool readOnlyDepth = inputs.DepthAttachment.ReadOnly || stage == ForwardGraphStage::Transparent;
    if (!readOnlyDepth) result.Depth = graph.NextVersion(inputs.Depth);
    bool setupSuccess = true;
    result.Pass = graph.AddRasterPass<ForwardGraphPassData>(
        inputs.Name,
        [&](ForwardGraphPassData& data, RenderGraphRasterBuilder& builder) {
            data.Backend = inputs.Backend;
            data.Execution = inputs.Execution;
            data.Views.reserve(inputs.Views.size());
            for (const ForwardGraphView& view : inputs.Views) {
                ForwardGraphPassData::ViewData next;
                next.View = view.View;
                next.List = view.List;
                // Nothing binds these resources when the view has no program groups, so declaring
                // them would keep them alive and add barriers for a pass that never reads them.
                if (!view.Parameters.empty()) {
                    // Every program's pass group binds the same graph resources, so they are declared
                    // once for the pass and their rows are shared by all programs.
                    vector<RgParameterBinding> shared;
                    shared.reserve(view.Resources.size());
                    for (const ForwardPassResource& resource : view.Resources) {
                        const auto row = DeclareForwardPassResource(builder, resource);
                        if (!row) {
                            setupSuccess = false;
                            continue;
                        }
                        shared.push_back(*row);
                    }
                    next.Constants.reserve(view.Parameters.size());
                    next.Rows.reserve(view.Parameters.size());
                    next.Parameters.reserve(view.Parameters.size());
                    for (const RendererListProgramParameters& program : view.Parameters)
                        CopyProgramRows(program, shared, next);
                }
                data.Views.push_back(std::move(next));
            }
            if (stage != ForwardGraphStage::Depth &&
                !builder.SetColorAttachment(0, result.Color, inputs.ColorAttachment).IsValid()) {
                setupSuccess = false;
            }
            RgDepthAttachmentDesc depth = inputs.DepthAttachment;
            depth.ReadOnly = depth.ReadOnly || stage == ForwardGraphStage::Transparent;
            for (uint32_t i = 0; i < inputs.AuxiliaryColors.size(); ++i)
                if (!builder.SetColorAttachment(i + 1, inputs.AuxiliaryColors[i]).IsValid()) setupSuccess = false;
            if (!builder.SetDepthAttachment(result.Depth, depth).IsValid()) setupSuccess = false;
        },
        PrepareForwardGraphPass,
        ExecuteForwardGraphPass);
    result.Success = result.Pass.IsValid() && setupSuccess;
    return result;
}

}  // namespace radray
