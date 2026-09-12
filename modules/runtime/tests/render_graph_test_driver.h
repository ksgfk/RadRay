#pragma once
#include <algorithm>
#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/mesh_draw_command.h>

namespace radray {
struct RenderGraphTestDriver {
    static size_t DrawCount(const PreparedRendererList& list) { return list.Draws.size(); }
    static array<int64_t, 3> DrawArguments(const PreparedRendererList& list, size_t index) {
        const auto& draw = list.Draws[index];
        return {draw.IndexCount, draw.FirstIndex, draw.VertexOffset};
    }
    static render::GraphicsPipelineState* Pipeline(const PreparedRendererList& list, size_t index) {
        return list.Draws[index].Pipeline;
    }
    static render::GraphicsCommandEncoder& NativeEncoder(RenderGraphRasterContext& context) {
        return context.Encoder()._encoder;
    }
    template <class Callback>
    static void WithEncoder(RenderGraphRasterContext& context, render::GraphicsCommandEncoder& encoder, Callback&& callback) {
        RenderGraphRasterContext testContext{context._graph, context._pass, encoder};
        callback(testContext);
    }
    inline static vector<std::pair<render::CommandBuffer*, shared_ptr<FrameSubmission>>> Pending;
    static RenderGraphExecutionResult Execute(RenderGraph& graph, render::CommandBuffer& command, Nullable<render::CommandBuffer*> native = nullptr) {
        auto result = graph.Execute(command);
        if (result.Submission) Pending.emplace_back(native ? native.Get() : &command, result.Submission);
        return result;
    }
    static RenderGraphExecutionResult ExecuteWithPresent(RenderGraph& graph, render::CommandBuffer& command,
                                                         std::span<const std::pair<render::Texture*, render::CommandBuffer*>> targets) {
        vector<RenderGraph::PresentCommandTarget> present;
        for (const auto& [texture, commands] : targets) present.push_back({texture, commands});
        return graph.Execute(command, present);
    }
    static void Submitted(render::CommandBuffer* command) {
        for (auto& entry : Pending)
            if (entry.first == command) entry.second->Submit(entry.second->Serial());
    }
    static void Completed(render::CommandBuffer* command) {
        std::erase_if(Pending, [=](auto& entry) {
            if (entry.first != command) return false;
            entry.second->Complete(entry.second->Serial(), true);
            return true;
        });
    }
};
}  // namespace radray
