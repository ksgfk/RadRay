#include <radray/runtime/forward_pipeline/forward_graph.h>

#include <algorithm>
#include <radray/profiler.h>
#include <radray/runtime/render_framework/viewport.h>

namespace radray {
struct ForwardGraphFrameData::Impl {
    struct ViewData {
        ResolvedRenderView View;
        const RendererList* List{nullptr};
        // Everything the prepare stage reads is owned here: growing the outer vectors relocates the
        // inner buffers without copying them, so the spans below stay valid.
        vector<vector<byte>> Constants;
        vector<vector<char>> Names;
        vector<vector<RgParameterBinding>> Rows;
        vector<RendererListProgramParameters> Parameters;
        vector<RgParameterBinding> SharedRows;
        std::span<const RgParameterBinding> TemplateRows;
        std::optional<ForwardGraphPassValues> PassValues;
        // Sets is declared before Prepared: the prepared draws hold spans into it.
        std::optional<RendererListPassSets> Sets;
        std::optional<PreparedRendererList> Prepared;
    };

    render::RenderBackend Backend{render::RenderBackend::MAX_COUNT};
    vector<ViewData> Views;
    DrawExecutionStats* Execution{nullptr};
};
ForwardGraphFrameData::ForwardGraphFrameData() : _impl(make_unique<Impl>()) {}
ForwardGraphFrameData::~ForwardGraphFrameData() = default;

namespace {
using ForwardGraphPassData = ForwardGraphFrameData::Impl;

struct ForwardGraphRecipe {
    struct View {
        vector<vector<char>> Names;
        vector<RgParameterBinding> Rows;
        bool PassValues{false};
    };
    vector<View> Views;
};

/// Copies one program's non-graph rows into the payload, repointing every cbuffer row at payload
/// storage, and appends the rows of the resources this pass already declared.
void CopyProgramRows(const RendererListProgramParameters& program, std::span<const RgParameterBinding> shared,
                     ForwardGraphPassData::ViewData& view) {
    RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.CopyProgramRows");
    auto& rows = view.Rows.emplace_back();
    rows.reserve(program.Bindings.size() + shared.size());
    const auto copy = [&](const RgParameterBinding& binding) {
        RgParameterBinding& row = rows.emplace_back(binding);
        const auto& name = view.Names.emplace_back(binding.Declaration.begin(), binding.Declaration.end());
        row.Declaration = {name.data(), name.size()};
        if (const auto* bytes = std::get_if<RgCBufferParameterBinding>(&row.Value)) {
            const auto& owned = view.Constants.emplace_back(bytes->Bytes.begin(), bytes->Bytes.end());
            row.Value = RgCBufferParameterBinding{owned};
        }
    };
    for (const RgParameterBinding& binding : program.Bindings) copy(binding);
    for (const RgParameterBinding& binding : shared) copy(binding);
    view.Parameters.push_back({program.Program, program.Group, rows});
}

bool PrepareForwardGraphPass(ForwardGraphPassData& data, RenderGraphPrepareContext& context) {
    RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.Ready");
    for (ForwardGraphPassData::ViewData& view : data.Views) {
        if (view.PassValues) {
            const auto& inputs = *view.PassValues;
            Forward_PassData values = *inputs.ShadowValues;
            values.Extent = Eigen::Vector4f{float(inputs.Extent.Width), float(inputs.Extent.Height), float((inputs.Extent.Width + 15) / 16), float(inputs.MaxLightsPerTile)};
            values.LocalLightCount = *inputs.LocalLightCount;
            values.UseTiles = inputs.UseTiles ? 1u : 0u;
            values.UseAo = inputs.UseAo ? 1u : 0u;
            values.Transparent = inputs.Transparent ? 1u : 0u;
            unordered_set<ShaderProgram*> programs;
            const auto addProgram = [&](ShaderProgram* program, Nullable<const StaticBindingRecipe*> bindings) {
                if (!programs.insert(program).second) return true;
                uint32_t group = UINT32_MAX;
                if (bindings)
                    group = bindings->Groups[static_cast<size_t>(StaticBindingRole::Pass)];
                else {
                    const auto binding = program->GetArtifact().FindBindingInfo("ForwardPass");
                    if (binding) group = binding->Group;
                }
                if (group == UINT32_MAX) return false;
                render::SamplerDescriptor sampler;
                sampler.MinFilter = sampler.MagFilter = sampler.MipmapFilter = render::FilterMode::Linear;
                sampler.AddressS = sampler.AddressT = sampler.AddressR = render::AddressMode::ClampToEdge;
                auto compare = sampler;
                compare.Compare = render::CompareFunction::LessEqual;
                const RgParameterBinding rows[]{
                    {"ForwardPass", 0, RgCBufferParameterBinding{std::as_bytes(std::span{&values, 1})}},
                    {"ShadowSampler", 0, RgSamplerParameterBinding{compare}},
                    {"ScreenSampler", 0, RgSamplerParameterBinding{sampler}}};
                CopyProgramRows({program, group, rows}, view.TemplateRows.empty() ? std::span<const RgParameterBinding>{view.SharedRows} : view.TemplateRows, view);
                return true;
            };
            const auto uses = view.List->GetPrograms();
            if (!uses.empty()) {
                for (const auto& use : uses)
                    if (!use.Program || !addProgram(use.Program.Get(), use.Bindings)) return false;
            } else {
                for (size_t index = 0; index < view.List->GetDrawCount(); ++index) {
                    const auto program = view.List->GetDescription(index).Program;
                    if (!program || !addProgram(program.Get(), nullptr)) return false;
                }
            }
        }
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

shared_ptr<ForwardGraphFrameData> ForwardGraph::MakeFrame(const ForwardGraphStageInputs& inputs) {
    RADRAY_PROFILE_SCOPE_N("Forward::DrawWorkBuild.PassFrameInputs");
    if (!EnumContains(inputs.Backend) || inputs.Execution == nullptr) return {};
    auto frame = make_shared<ForwardGraphFrameData>();
    auto& data = *frame->_impl;
    data.Backend = inputs.Backend;
    data.Execution = inputs.Execution;
    data.Views.reserve(inputs.Views.size());
    for (const auto& input : inputs.Views) {
        if (!input.List) return {};
        auto& view = data.Views.emplace_back();
        view.View = input.View;
        view.List = input.List;
        view.PassValues = input.PassValues;
        for (const auto& program : input.Parameters) CopyProgramRows(program, {}, view);
    }
    return frame;
}

ForwardGraphStageOutput ForwardGraph::DeclareTemplate(
    RenderGraph& graph, ForwardGraphStage stage, RgTemplateSlot<ForwardGraphFrameData> frame,
    const ForwardGraphStageInputs& inputs) {
    ForwardGraphStageOutput result{inputs.Color, inputs.Depth, {}, false};
    if (!EnumContains(stage) || !inputs.Depth.IsValid() || (stage != ForwardGraphStage::Depth && !inputs.Color.IsValid())) return result;
    if (stage != ForwardGraphStage::Depth) result.Color = graph.NextVersion(inputs.Color);
    const bool readOnlyDepth = inputs.DepthAttachment.ReadOnly || stage == ForwardGraphStage::Transparent;
    if (!readOnlyDepth) result.Depth = graph.NextVersion(inputs.Depth);
    bool valid = true;
    result.Pass = graph.AddTemplateRasterPass<ForwardGraphRecipe>(inputs.Name, frame, [&](ForwardGraphRecipe& recipe, RenderGraphRasterBuilder& builder) {
            recipe.Views.resize(inputs.Views.size());
            for (size_t index = 0; index < inputs.Views.size(); ++index) {
                const auto& input = inputs.Views[index];
                auto& view = recipe.Views[index];
                view.PassValues = input.PassValues.has_value();
                if (!input.Parameters.empty()) {
                    builder.Reject("TemplateCapture", "Forward template stages take pass constants from their frame slot");
                    valid = false;
                }
                if (input.Work.IsValid()) builder.RequireWork(input.Work, input.WorkMask);
                if (!view.PassValues) continue;
                view.Rows.reserve(input.Resources.size());
                view.Names.reserve(input.Resources.size());
                for (const auto& resource : input.Resources) {
                    const auto row = DeclareForwardPassResource(builder, resource);
                    if (!row) { valid = false; continue; }
                    const auto& name = view.Names.emplace_back(row->Declaration.begin(), row->Declaration.end());
                    view.Rows.push_back(*row);
                    view.Rows.back().Declaration = {name.data(), name.size()};
                }
            }
            if (stage != ForwardGraphStage::Depth && !builder.SetColorAttachment(0, result.Color, inputs.ColorAttachment).IsValid()) valid = false;
            auto depth = inputs.DepthAttachment;
            depth.ReadOnly = readOnlyDepth;
            for (uint32_t index = 0; index < inputs.AuxiliaryColors.size(); ++index)
                if (!builder.SetColorAttachment(index + 1, inputs.AuxiliaryColors[index]).IsValid()) valid = false;
            if (!builder.SetDepthAttachment(result.Depth, depth).IsValid()) valid = false; }, +[](const ForwardGraphRecipe& recipe, ForwardGraphFrameData& frame, RenderGraphPrepareContext& context) {
            auto& data = *frame._impl;
            if (data.Views.size() != recipe.Views.size()) return false;
            for (size_t index = 0; index < data.Views.size(); ++index) {
                if (data.Views[index].PassValues.has_value() != recipe.Views[index].PassValues) return false;
                data.Views[index].TemplateRows = recipe.Views[index].Rows;
            }
            return PrepareForwardGraphPass(data, context); }, +[](const ForwardGraphRecipe&, const ForwardGraphFrameData& frame, RenderGraphRasterContext& context) { ExecuteForwardGraphPass(*frame._impl, context); });
    result.Success = valid && result.Pass.IsValid();
    return result;
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

    // Deferred stages preserve their clear/load/store contract independently of current visibility.
    // The legacy adapter retains its explicit omission of empty optional stages.
    const bool deferred = std::any_of(inputs.Views.begin(), inputs.Views.end(), [](const ForwardGraphView& view) { return view.Work.IsValid(); });
    const bool hasCommands = deferred || std::any_of(
                                             inputs.Views.begin(), inputs.Views.end(),
                                             [](const ForwardGraphView& view) { return view.List->GetDrawCount() != 0; });
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
                next.PassValues = view.PassValues;
                if (view.Work.IsValid()) builder.RequireWork(view.Work, view.WorkMask);
                // Deferred stages declare the complete resource contract before their programs exist.
                if (!view.Parameters.empty() || view.PassValues) {
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
                    if (view.PassValues) {
                        next.SharedRows = std::move(shared);
                        for (auto& row : next.SharedRows) {
                            const auto& name = next.Names.emplace_back(row.Declaration.begin(), row.Declaration.end());
                            row.Declaration = {name.data(), name.size()};
                        }
                    }
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
