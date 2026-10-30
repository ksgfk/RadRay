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
                           SampleDrawListContext& output, std::string_view passName, uint8_t stencilReference) {
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
        output.HasErrors = true;
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
        output.Pass.Commands.back().StencilReference = stencilReference;
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
                            const PrimitiveSceneInfo& primitive, const array<float, 4>& color, float localScale) {
    // 步骤 6：样例为接受的绘制保存 MVP 和颜色，放入样例自己的 PreparedDraw。
    // 为什么：LocalToClip、Color 及 push constant 传递方式只属于 cube.hlsl 的参数约定。
    // UE5 对照：Renderer/Public/MeshPassProcessor.inl 准备共享 bindings，再展开 Element 构建 FMeshDrawCommand。
    // 本例简化：一个 80 字节 push block；runtime 的命令数据不持有或绑定 shader 参数。
    DrawConstants constants;
    Eigen::Matrix4f expansion = Eigen::Matrix4f::Identity();
    expansion(0, 0) = expansion(1, 1) = expansion(2, 2) = localScale;
    const Eigen::Matrix4f localToClip = viewProjection * primitive.LocalToWorld.ToMatrix() * expansion;
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

bool AcceptsMaterial(const Scene& scene, const MeshBatchContext& context, const BatchPassPolicy& policy,
                     MaterialTagFilter filter, std::string_view passName, SampleDrawListContext& output) {
    if (!scene.Materials.IsAlive(context.Batch.Material)) {
        RADRAY_ERR_LOG("材质句柄失效：{}", context.Batch.Material);
        output.HasErrors = true;
        return false;
    }
    const auto resource = scene.Materials.Get(context.Batch.Material).GetResource();
    if (!resource) {
        RADRAY_ERR_LOG("材质尚未同步到 Proxy");
        output.HasErrors = true;
        return false;
    }
    const auto& tags = resource->Properties.Tags;
    if (!tags.GetTable()) {
        LogSkipped(passName, context, policy, filter.Description);
        return false;
    }
    if (tags.GetTable().Get() != filter.Table) {
        RADRAY_ERR_LOG("材质与 Processor 必须使用同一张标签表");
        output.HasErrors = true;
        return false;
    }
    if (!tags.Matches(filter.Required.Key, filter.Required.Value)) {
        LogSkipped(passName, context, policy, filter.Description);
        return false;
    }
    return true;
}

Nullable<const MaterialPipelines*> FindMaterialPipelines(
    const Scene& scene, SparseSetHandle material, std::span<const MaterialPipelines> pipelines) {
    if (!scene.Materials.IsAlive(material)) {
        RADRAY_ERR_LOG("材质句柄失效：{}", material);
        return nullptr;
    }
    const auto resource = scene.Materials.Get(material).GetResource();
    for (const auto& entry : pipelines) {
        if (entry.Resource == resource.Get()) return &entry;
    }
    RADRAY_ERR_LOG("材质未同步到 Proxy 或其 PSO 尚未准备：{}", material);
    return nullptr;
}

}  // namespace

SampleDrawListContext::SampleDrawListContext(const PassContext& pass, std::string_view passName,
                                             const Scene& scene, MaterialParameterIndices parameters, std::span<const BatchPassPolicy> policies,
                                             float localScale, std::optional<array<float, 4>> colorOverride)
    : Pass{pass.ViewportRect, pass.Scissor, {}}, _viewProjection(pass.ViewProjection),
      _passName(passName), _policies(policies), _scene(&scene), _parameters(parameters),
      _localScale(localScale), _colorOverride(colorOverride) {}

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
    // 步骤 M5：从 Proxy 快照读取材质参数，再适配成本例 shader 的 80 字节参数块。
    // 为什么：Material 声明参数含义，样例负责其 GPU 布局；runtime MeshDrawCommand 不持有 push constant。
    // UE5 对照：Renderer/Private/ShaderBaseClasses.cpp 的 FMaterialShader::GetShaderBindings 绑定材质 uniform buffer。
    // 本例简化：CPU 读取两个类型明确的值；不实现 UE 的 uniform expression 求值、GPU 缓冲缓存或线程同步。
    if (!_scene->Materials.IsAlive(context.Batch.Material)) {
        RADRAY_ERR_LOG("材质句柄失效");
        HasErrors = true;
        return;
    }
    const auto& proxy = _scene->Materials.Get(context.Batch.Material);
    const auto baseValue = proxy.GetParameter(_parameters.BaseColor);
    const auto brightnessValue = proxy.GetParameter(_parameters.Brightness);
    const Nullable<const array<float, 4>*> baseColor = baseValue ? std::get_if<array<float, 4>>(baseValue.Get()) : nullptr;
    const Nullable<const float*> brightness = brightnessValue ? std::get_if<float>(brightnessValue.Get()) : nullptr;
    if (!baseColor || !brightness || !proxy.GetResource()) {
        RADRAY_ERR_LOG("材质参数不符合本例 BaseColor(float4) / Brightness(float) 契约");
        HasErrors = true;
        return;
    }
    auto color = *baseColor;
    for (size_t index = 0; index < 3; ++index) color[index] *= *brightness;
    // Outline 的固定颜色与局部放大倍率由 Pass 覆盖；不改变原 Primitive 或 MaterialInstance。
    if (_colorOverride) color = *_colorOverride;
    Pass.Commands.back().Constants = MakeConstants(_viewProjection, context.Primitive, color, _localScale);
    RADRAY_INFO_LOG("{} Pass 接受：对象={} Primitive={} Batch={} Element={} → Draw={}，索引范围=[{}, {})，Material={} Cull={}",
                    _passName, policy->ObjectName, context.PrimitiveHandle, context.BatchHandle, elementIndex,
                    Pass.Commands.size() - 1, command.FirstIndex, uint64_t{command.FirstIndex} + command.IndexCount,
                    context.Batch.Material, static_cast<int32_t>(proxy.GetResource()->Properties.RenderState.Cull));
}

DepthPassProcessor::DepthPassProcessor(SampleDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                                       std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter) noexcept
    : MeshPassProcessor(drawListContext), _output(&drawListContext), _scene(&scene), _pipelines(pipelines), _policies(policies), _materialFilter(materialFilter) {}

void DepthPassProcessor::AddMeshBatch(const MeshBatchContext& context) {
    // 步骤 5a：Depth Processor 排除 ColorOnly，再检查材质 Surface=Opaque，接受的 Batch 才展开 Element。
    // 为什么：物体参与规则与材质语义同时满足才可绘制；登记后的 ID 比较无需字符串查找。
    // UE5 对照：Renderer/Private/DepthRendering.cpp 的 FDepthPassMeshProcessor::AddMeshBatch 返回 void。
    // 本例简化：按 BatchPassPolicy 与 Surface=Opaque 的 ID 对筛选；没有材质 fallback 或遮挡体尺寸判断。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ERR_LOG("Batch 没有样例 Pass 配置");
        _output->HasErrors = true;
        return;
    }
    if (policy->Participation == PassParticipation::ColorOnly) {
        LogSkipped("Depth", context, *policy, "ColorOnly：不参加 Depth Pass");
        return;
    }
    if (!AcceptsMaterial(*_scene, context, *policy, _materialFilter, "Depth", *_output)) return;
    const auto pipelines = FindMaterialPipelines(*_scene, context.Batch.Material, _pipelines);
    if (!pipelines || !pipelines->Depth) {
        _output->HasErrors = true;
        return;
    }
    BuildMeshDrawCommands(context, *pipelines->Depth, *_output, "Depth", 0);
}

ColorPassProcessor::ColorPassProcessor(SampleDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                                       std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter) noexcept
    : MeshPassProcessor(drawListContext), _output(&drawListContext), _scene(&scene), _pipelines(pipelines), _policies(policies), _materialFilter(materialFilter) {}

void ColorPassProcessor::AddMeshBatch(const MeshBatchContext& context) {
    // 步骤 5b：Color Processor 排除 DepthOnly，再检查材质标签，并将模板参考值复制到本次输出。
    // 为什么：Processor 选择 PSO 与最终动态状态；录制阶段不再读取材质或 Scene。
    // UE5 对照：Renderer/Private/BasePassRendering.cpp 的 FBasePassMeshProcessor::AddMeshBatch。
    // 本例简化：ColorOnly 仍使用允许补写深度的 Color PSO，不复刻 UE 的完整 Base Pass。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ERR_LOG("Batch 没有样例 Pass 配置");
        _output->HasErrors = true;
        return;
    }
    if (policy->Participation == PassParticipation::DepthOnly) {
        LogSkipped("Color", context, *policy, "DepthOnly：仅作深度遮挡，不参加 Color Pass");
        return;
    }
    if (!AcceptsMaterial(*_scene, context, *policy, _materialFilter, "Color", *_output)) return;
    const auto pipelines = FindMaterialPipelines(*_scene, context.Batch.Material, _pipelines);
    if (!pipelines || !pipelines->Color) {
        _output->HasErrors = true;
        return;
    }
    BuildMeshDrawCommands(context, *pipelines->Color, *_output, "Color", pipelines->Resource->Properties.RenderState.StencilReference);
}

OutlinePassProcessor::OutlinePassProcessor(SampleDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                                           std::span<const BatchPassPolicy> policies, MaterialTagFilter opaqueFilter, MaterialTagFilter outlineFilter) noexcept
    : MeshPassProcessor(drawListContext), _output(&drawListContext), _scene(&scene), _pipelines(pipelines), _policies(policies),
      _opaqueFilter(opaqueFilter), _outlineFilter(outlineFilter) {}

void OutlinePassProcessor::AddMeshBatch(const MeshBatchContext& context) {
    // 步骤 5d：只接受参与颜色绘制、Surface=Opaque 且 Outline=Enabled 的 Batch。
    // 为什么：先确认原物体会在 Color Pass 写模板，再用相同 Section 的放大外壳描边；没有标签就不生成命令。
    // UE5 对照：Renderer/Private/CustomDepthRendering.cpp 的 FCustomDepthPassMeshProcessor 筛选 Batch 并设置模板状态。
    // 本例简化：描边标签是样例约定；UE 的 CustomDepth 参与标记/模板值通常来自 PrimitiveSceneProxy，不是这套标签。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ERR_LOG("Batch 没有样例 Pass 配置");
        _output->HasErrors = true;
        return;
    }
    if (policy->Participation == PassParticipation::DepthOnly) {
        LogSkipped("Outline", context, *policy, "DepthOnly：不参加颜色绘制，也不描边");
        return;
    }
    if (!AcceptsMaterial(*_scene, context, *policy, _opaqueFilter, "Outline", *_output) ||
        !AcceptsMaterial(*_scene, context, *policy, _outlineFilter, "Outline", *_output)) return;
    const auto pipelines = FindMaterialPipelines(*_scene, context.Batch.Material, _pipelines);
    if (!pipelines || !pipelines->Outline) {
        _output->HasErrors = true;
        return;
    }
    BuildMeshDrawCommands(context, *pipelines->Outline, *_output, "Outline", pipelines->Resource->Properties.RenderState.StencilReference);
}

TransparentPassProcessor::TransparentPassProcessor(SampleDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                                                   std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter) noexcept
    : MeshPassProcessor(drawListContext), _output(&drawListContext), _scene(&scene), _pipelines(pipelines), _policies(policies), _materialFilter(materialFilter) {}

void TransparentPassProcessor::AddMeshBatch(const MeshBatchContext& context) {
    // 步骤 5e：颜色参与规则满足后，只接受 Surface=Transparent；不透明材质在这里被拒绝。
    // 为什么：标签决定本 Pass 接受谁，Blend/DepthWrite 决定如何绘制；只把 alpha 改小不会自动启用混合。
    // UE5 对照：Renderer/Private/BasePassRendering.cpp 的 FBasePassMeshProcessor::ShouldDraw 判断透明类别；
    // Renderer/Private/TranslucentRendering.cpp 根据 BlendMode 设置透明混合状态。
    // 本例简化：单个凸透明物体，仅绘制正面，普通 source-over；无折射、OIT 或透明三角形排序。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ERR_LOG("Batch 没有样例 Pass 配置");
        _output->HasErrors = true;
        return;
    }
    if (policy->Participation == PassParticipation::DepthOnly) {
        LogSkipped("Transparent", context, *policy, "DepthOnly：不参加透明颜色绘制");
        return;
    }
    if (!AcceptsMaterial(*_scene, context, *policy, _materialFilter, "Transparent", *_output)) return;
    const auto pipelines = FindMaterialPipelines(*_scene, context.Batch.Material, _pipelines);
    if (!pipelines || !pipelines->Color) {
        _output->HasErrors = true;
        return;
    }
    BuildMeshDrawCommands(context, *pipelines->Color, *_output, "Transparent", pipelines->Resource->Properties.RenderState.StencilReference);
}

bool RecordDrawCommands(render::GraphicsCommandEncoder& encoder, const PreparedPass& pass) {
    // 步骤 7：样例按 PSO → VB/IB → 模板参考值 → 常量 → DrawIndexed 顺序录制。
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
        encoder.SetStencilReference(draw.StencilReference);
        if (!encoder.SetPushConstants(0, 0, std::as_bytes(std::span{&draw.Constants, 1}))) {
            RADRAY_ERR_LOG("样例参数绑定失败，放弃提交当前 CommandBuffer");
            return false;
        }
        encoder.DrawIndexed(command.IndexCount, 1, command.FirstIndex, command.VertexOffset, 0);
    }
    return true;
}

}  // namespace radray::examples::mesh_drawing
