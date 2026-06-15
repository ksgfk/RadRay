#include <radray/runtime/renderer/render_pipeline.h>

#include <utility>

#include <radray/runtime/renderer/render_context.h>

namespace radray {

// 析构函数定义在此:充当 vtable 锚点,避免每个 TU 重复发射。
RenderPipeline::~RenderPipeline() noexcept = default;

void RenderPipeline::AddPass(unique_ptr<RenderPass> pass) {
    if (pass != nullptr) {
        _passes.push_back(std::move(pass));
    }
}

size_t RenderPipeline::GetPassCount() const noexcept {
    return _passes.size();
}

void RenderPipeline::Render(RenderContext& ctx) {
    for (const unique_ptr<RenderPass>& pass : _passes) {
        if (pass != nullptr) {
            pass->Execute(ctx);
        }
    }
}

}  // namespace radray
