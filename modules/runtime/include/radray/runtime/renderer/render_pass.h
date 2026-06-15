#pragma once

#include <string_view>

#include <radray/runtime/renderer/render_context.h>

namespace radray {

/// 渲染管线的一个阶段。对应 Unity ScriptableRenderPass / UE5 RDG 的一个 pass(最小化)。
///
/// 设计(最小化):
/// - 只有两个虚函数:身份(GetName)与执行(Execute)。无 event 排序、无图元过滤。
/// - 派生类在 Execute 内自行完成 BeginRenderPass → 录制 → EndRenderPass,
///   并自管自己申请的中间资源(如 depth)及其 barrier。
/// - runtime 不提供任何具体 pass:具体 pass(画场景、后处理等)由 game 层定义。
///   runtime 只提供"怎么组织 pass"的词汇表,不提供"画什么"的内容。
class RenderPass {
public:
    RenderPass() noexcept = default;
    RenderPass(const RenderPass&) = delete;
    RenderPass(RenderPass&&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;
    RenderPass& operator=(RenderPass&&) = delete;
    virtual ~RenderPass() noexcept = default;

    /// 调试名。出现在日志 / RenderPassDescriptor::Name 等处。
    virtual std::string_view GetName() const noexcept = 0;

    /// 执行本阶段。自行 BeginRenderPass → 录制 → EndRenderPass。
    virtual void Execute(RenderContext& ctx) = 0;
};

}  // namespace radray
