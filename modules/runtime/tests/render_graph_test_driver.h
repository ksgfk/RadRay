#pragma once
#include <radray/runtime/render_framework/render_graph.h>

namespace radray {
struct RenderGraphTestDriver {
    static RenderGraphExecutionResult Execute(RenderGraph& graph, render::CommandBuffer& command) { return graph.Execute(command); }
};
}  // namespace radray
