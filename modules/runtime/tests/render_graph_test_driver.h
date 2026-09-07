#pragma once
#include <algorithm>
#include <radray/runtime/render_framework/render_graph.h>

namespace radray {
struct RenderGraphTestDriver {
    inline static vector<std::pair<render::CommandBuffer*, shared_ptr<FrameSubmission>>> Pending;
    static RenderGraphExecutionResult Execute(RenderGraph& graph, render::CommandBuffer& command, Nullable<render::CommandBuffer*> native = nullptr) {
        auto result = graph.Execute(command);
        if (result.Submission) Pending.emplace_back(native ? native.Get() : &command, result.Submission);
        return result;
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
