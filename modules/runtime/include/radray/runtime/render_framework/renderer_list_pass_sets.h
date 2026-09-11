#pragma once

#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/renderer_list.h>

namespace radray {

struct RendererListProgramParameters {
    ShaderProgram* Program;
    uint32_t Group;
    std::span<const RgParameterBinding> Bindings;
};

/// Graph parameter sets shared by every draw of one program in a pass. Created during the prepare
/// stage of that pass; programs and the list must stay alive and immutable until execution finishes.
class RendererListPassSets {
public:
    static std::optional<RendererListPassSets> Create(RenderGraphPrepareContext& ctx, const RendererList& list,
                                                      std::span<const RendererListProgramParameters> parameters);
    /// Sets for this program, ascending by group; empty when the program has none.
    std::span<const PreparedShaderGroup> Find(const ShaderProgram& program) const noexcept;
    /// The pass whose prepare stage built these sets; their views are only declared there.
    RgPassHandle GetPass() const noexcept { return _pass; }

private:
    struct Range {
        const ShaderProgram* Program;
        uint32_t First, Count;
    };
    RgPassHandle _pass{};
    vector<Range> _programs;
    vector<PreparedShaderGroup> _sets;
};

}  // namespace radray
