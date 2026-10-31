#include "pipeline.h"

#include <algorithm>
#include <cstring>

#include <radray/logger.h>

namespace radray::examples::mesh_drawing {
namespace {

bool ValidateInput(const VertexInput& input) {
    vector<render::VertexAttribute> attributes;
    for (const auto& attribute : input.Attributes) {
        attributes.push_back({attribute.BufferBinding, attribute.Offset, attribute.Semantic,
                              attribute.SemanticIndex, attribute.Format, attribute.Location});
    }
    if (!render::ValidateVertexInputState({input.Layouts, attributes}) || input.Streams.size() != input.Layouts.size()) return false;
    for (const auto& layout : input.Layouts) {
        const auto stream = std::find_if(input.Streams.begin(), input.Streams.end(), [&](const VertexInputStream& value) {
            return value.Binding == layout.Binding;
        });
        if (stream == input.Streams.end() || stream->Size < layout.ArrayStride) return false;
    }
    return true;
}

void BuildMeshDrawCommands(const MeshBatchContext& context, render::GraphicsPipelineState& pipeline,
                           MeshPassDrawListContext& output, std::string_view passName) {
    // 步骤 5c：样例检查几何输入，准备共享 PSO/顶点绑定，再逐个展开非空 Element。
    // 为什么：具体命令如何构建由样例组织；runtime 的 Processor 只保留调用接口和输出 Context。
    // UE5 对照：Renderer/Public/MeshPassProcessor.inl 的 FMeshPassProcessor::BuildMeshDrawCommands。
    // 本例简化：构建 helper 留在样例，不作为 runtime API；仅支持直接索引单实例，不处理 shader bindings。
    const auto& batch = context.Batch;
    if (std::none_of(batch.Elements.begin(), batch.Elements.end(), [](const MeshBatchElement& element) { return element.IndexCount != 0; })) {
        return;
    }
    if (!batch.VertexInput || !ValidateInput(*batch.VertexInput)) {
        RADRAY_ERR_LOG("Mesh pass {}: batch {} has invalid vertex input layout or streams", passName, context.BatchHandle);
        return;
    }
    MeshDrawCommand initializer;
    initializer.Pipeline = &pipeline;
    for (const auto& stream : batch.VertexInput->Streams) {
        initializer.VertexBuffers.push_back({stream.Binding, {stream.Buffer.Get(), stream.Offset, stream.Size}});
    }
    for (size_t elementIndex = 0; elementIndex < batch.Elements.size(); ++elementIndex) {
        if (batch.Elements[elementIndex].IndexCount == 0) continue;
        auto& command = output.AddCommand(initializer);
        output.FinalizeCommand(context, elementIndex, command);
    }
}

bool ValidateGeometry(const MeshDrawCommand& command) noexcept {
    const auto& indices = command.IndexBuffer;
    if (!command.Pipeline || !command.Pipeline->IsValid() || !indices.Target || !indices.Target->IsValid() || command.IndexCount == 0 ||
        (indices.Stride != 2 && indices.Stride != 4) || indices.Offset % indices.Stride != 0) return false;
    const auto indexDesc = indices.Target->GetDesc();
    const uint64_t indexEnd = uint64_t{indices.Offset} +
                              (uint64_t{command.FirstIndex} + command.IndexCount) * indices.Stride;
    if (!indexDesc.Usage.HasFlag(render::BufferUse::Index) || indexEnd > indexDesc.Size) return false;
    for (size_t i = 0; i < command.VertexBuffers.size(); ++i) {
        const auto& binding = command.VertexBuffers[i];
        const auto& view = binding.View;
        if (!view.Target || !view.Target->IsValid() || view.Size == 0) return false;
        const auto desc = view.Target->GetDesc();
        if (!desc.Usage.HasFlag(render::BufferUse::Vertex) || view.Offset > desc.Size || view.Size > desc.Size - view.Offset) return false;
        for (size_t j = 0; j < i; ++j) {
            if (command.VertexBuffers[j].Binding == binding.Binding) return false;
        }
    }
    return true;
}

DrawConstants MakeConstants(const Eigen::Matrix4f& viewProjection,
                            const PrimitiveSceneInfo& primitive, const array<float, 4>& color) {
    // 步骤 6：样例为接受的绘制保存 MVP 和颜色，放入样例自己的 PreparedDraw。
    // 为什么：LocalToClip、Color 及 push constant 传递方式只属于 cube.hlsl 的参数约定。
    // UE5 对照：Renderer/Public/MeshPassProcessor.inl 准备共享 bindings，再展开 Element 构建 FMeshDrawCommand。
    // 本例简化：一个 80 字节 push block；runtime 的命令数据不持有或绑定 shader 参数。
    DrawConstants constants;
    const Eigen::Matrix4f localToClip = viewProjection * primitive.LocalToWorld.ToMatrix();
    std::memcpy(constants.LocalToClip.data(), localToClip.data(), sizeof(constants.LocalToClip));
    constants.Color = color;
    return constants;
}

Nullable<const BatchPassPolicy*> FindPolicy(std::span<const BatchPassPolicy> policies, SparseSetHandle handle) {
    const auto found = std::find_if(policies.begin(), policies.end(), [&](const BatchPassPolicy& policy) { return policy.Batch == handle; });
    return found == policies.end() ? nullptr : &*found;
}

void LogSkipped(std::string_view passName, const MeshBatchContext& context, const BatchPassPolicy& policy, std::string_view reason) {
    RADRAY_INFO_LOG("{} Pass 跳过：对象={} Primitive={} Batch={}（未生成 DrawCommand），原因={}",
                    passName, policy.ObjectName, context.PrimitiveHandle, context.BatchHandle, reason);
}

}  // namespace

SampleDrawListContext::SampleDrawListContext(const PassContext& pass, std::string_view passName,
                                             std::span<const BatchPassPolicy> policies, std::span<const MaterialColor> colors)
    : Pass{pass.ViewportRect, pass.Scissor, {}}, _viewProjection(pass.ViewProjection),
      _passName(passName), _policies(policies), _colors(colors) {}

MeshDrawCommand& SampleDrawListContext::AddCommand(const MeshDrawCommand& initializer) {
    Pass.Commands.push_back({initializer, {}});
    return Pass.Commands.back().Command;
}

void SampleDrawListContext::FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) {
    // 步骤 6：Context 完成 Element 绘制范围，并将样例参数附到自己的 PreparedDraw。
    // 为什么：AddMeshBatch 返回 void，输出由 Context 持有；runtime 无需知道颜色、MVP 或教学日志。
    // UE5 对照：Renderer/Public/MeshPassProcessor.h 的 FMeshPassDrawListContext::AddCommand / FinalizeCommand。
    // 本例简化：立即完成刚追加的命令，不保存命令指针；只有本次输出，没有静态缓存或状态桶。
    MeshPassDrawListContext::FinalizeCommand(context, elementIndex, command);
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ERR_LOG("Batch 没有样例 Pass 配置");
        HasErrors = true;
        return;
    }
    array<float, 4> color{1, 1, 1, 1};
    if (!_colors.empty()) {
        const auto found = std::find_if(_colors.begin(), _colors.end(), [&](const MaterialColor& value) {
            return value.Material == context.Batch.Material;
        });
        if (found == _colors.end()) {
            RADRAY_ERR_LOG("材质没有样例颜色映射");
            HasErrors = true;
            return;
        }
        color = found->Color;
    }
    Pass.Commands.back().Constants = MakeConstants(_viewProjection, context.Primitive, color);
    RADRAY_INFO_LOG("{} Pass 接受：对象={} Primitive={} Batch={} Element={} → Draw={}，索引范围=[{}, {})",
                    _passName, policy->ObjectName, context.PrimitiveHandle, context.BatchHandle, elementIndex,
                    Pass.Commands.size() - 1, command.FirstIndex, uint64_t{command.FirstIndex} + command.IndexCount);
}

DepthPassProcessor::DepthPassProcessor(MeshPassDrawListContext& drawListContext, render::GraphicsPipelineState& pipeline,
                                       std::span<const BatchPassPolicy> policies) noexcept
    : MeshPassProcessor(drawListContext), _pipeline(&pipeline), _policies(policies) {}

void DepthPassProcessor::AddMeshBatch(const MeshBatchContext& context) {
    // 步骤 5a：Depth Processor 排除 ColorOnly，其余 Batch 经 Element 循环写入 Context。
    // 为什么：跳过时直接返回，不创建状态结果或临时命令列表。
    // UE5 对照：Renderer/Private/DepthRendering.cpp 的 FDepthPassMeshProcessor::AddMeshBatch 返回 void。
    // 本例简化：只按样例 BatchPassPolicy 筛选；没有材质 fallback 或遮挡体尺寸判断。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ERR_LOG("Batch 没有样例 Pass 配置");
        return;
    }
    if (policy->Participation == PassParticipation::ColorOnly) {
        LogSkipped("Depth", context, *policy, "ColorOnly：不参加 Depth Pass");
        return;
    }
    BuildMeshDrawCommands(context, *_pipeline, _drawListContext, "Depth");
}

ColorPassProcessor::ColorPassProcessor(MeshPassDrawListContext& drawListContext, render::GraphicsPipelineState& pipeline,
                                       std::span<const BatchPassPolicy> policies) noexcept
    : MeshPassProcessor(drawListContext), _pipeline(&pipeline), _policies(policies) {}

void ColorPassProcessor::AddMeshBatch(const MeshBatchContext& context) {
    // 步骤 5b：Color Processor 排除 DepthOnly，其余 Batch 经 Element 循环写入 Context。
    // 为什么：Processor 决定是否生成及使用哪个 PSO；输出存储、样例颜色和日志由 Context 处理。
    // UE5 对照：Renderer/Private/BasePassRendering.cpp 的 FBasePassMeshProcessor::AddMeshBatch。
    // 本例简化：ColorOnly 仍使用允许补写深度的 Color PSO，不复刻 UE 的完整 Base Pass。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ERR_LOG("Batch 没有样例 Pass 配置");
        return;
    }
    if (policy->Participation == PassParticipation::DepthOnly) {
        LogSkipped("Color", context, *policy, "DepthOnly：仅作深度遮挡，不参加 Color Pass");
        return;
    }
    BuildMeshDrawCommands(context, *_pipeline, _drawListContext, "Color");
}

bool RecordDrawCommands(render::GraphicsCommandEncoder& encoder, const PreparedPass& pass) {
    // 步骤 7：样例按 PSO → VB/IB → 常量 → DrawIndexed 顺序录制。
    // 为什么：校验与 RHI 录制由样例负责；这里读取 PreparedDraw，不再读取 Scene。
    // UE5 对照：Renderer/Private/MeshPassProcessor.cpp 的 SubmitMeshDrawCommands / FMeshDrawCommand::SubmitDraw。
    // 本例简化：此处直接用 RHI 绑定固定 80 字节参数；runtime 提供命令数据、Processor 与输出 Context 接口。
    for (const auto& draw : pass.Commands) {
        if (!ValidateGeometry(draw.Command)) {
            RADRAY_ERR_LOG("样例的几何绘制命令无效");
            return false;
        }
    }
    encoder.SetViewport(pass.ViewportRect);
    encoder.SetScissor(pass.Scissor);
    for (const auto& draw : pass.Commands) {
        const auto& command = draw.Command;
        encoder.BindGraphicsPipelineState(command.Pipeline.Get());
        encoder.BindVertexBuffers(command.VertexBuffers);
        encoder.BindIndexBuffer(command.IndexBuffer);
        if (!encoder.SetPushConstants(0, 0, std::as_bytes(std::span{&draw.Constants, 1}))) {
            RADRAY_ERR_LOG("样例参数绑定失败，放弃提交当前 CommandBuffer");
            return false;
        }
        encoder.DrawIndexed(command.IndexCount, 1, command.FirstIndex, command.VertexOffset, 0);
    }
    return true;
}

}  // namespace radray::examples::mesh_drawing
