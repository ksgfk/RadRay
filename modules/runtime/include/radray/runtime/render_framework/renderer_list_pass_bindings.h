#pragma once

#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/renderer_list.h>

namespace radray {

struct RendererListPassBinding {
    ShaderProgram* Program;
    uint32_t Group;
    RgParameterSetHandle Parameters;
};

struct RendererListProgramParameters {
    ShaderProgram* Program;
    uint32_t Group;
    std::span<const RgParameterBinding> Bindings;
};

/// Pass-local values; programs and the list must remain alive and immutable until graph execution.
class RendererListPassBindings {
public:
    static std::optional<RendererListPassBindings> Build(RenderGraphRasterBuilder& builder, const RendererList& list,
                                                         std::span<const RendererListPassBinding> bindings);
    static std::optional<RendererListPassBindings> Create(RenderGraphRasterBuilder& builder, const RendererList& list,
                                                          std::span<const RendererListProgramParameters> parameters);
    bool IsValidFor(const RenderGraphRasterContext& context, const ShaderProgram& program) const noexcept;
    std::span<const RendererListPassBinding> Find(const ShaderProgram& program) const noexcept;

private:
    RgPassHandle _pass;
    vector<const ShaderProgram*> _programs;
    vector<RendererListPassBinding> _bindings;
};

}  // namespace radray
