#include "pipeline.h"

#include <algorithm>
#include <limits>

#include <radray/logger.h>

namespace radray::examples::mesh_drawing {
namespace {

bool ExtractMaterialColor(const Scene& scene, SparseSetHandle materialHandle,
                          MaterialParameterIndices parameters, MaterialConstants& outConstants) {
    if (!scene.Materials.IsAlive(materialHandle)) return false;
    const auto& proxy = scene.Materials.Get(materialHandle);
    const auto baseValue = proxy.GetParameter(parameters.BaseColor);
    const auto brightnessValue = proxy.GetParameter(parameters.Brightness);
    const Nullable<const array<float, 4>*> baseColor = baseValue ? std::get_if<array<float, 4>>(baseValue.Get()) : nullptr;
    const Nullable<const float*> brightness = brightnessValue ? std::get_if<float>(brightnessValue.Get()) : nullptr;
    if (!baseColor || !brightness) return false;
    outConstants.Color = *baseColor;
    for (size_t index = 0; index < 3; ++index) outConstants.Color[index] *= *brightness;
    return true;
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
                     MaterialTagFilter filter, std::string_view passName, bool& hasErrors) {
    if (!scene.Materials.IsAlive(context.Batch.Material)) {
        RADRAY_ERR_LOG("材质句柄失效：{}", context.Batch.Material);
        hasErrors = true;
        return false;
    }
    const auto resource = scene.Materials.Get(context.Batch.Material).GetResource();
    if (!resource) {
        RADRAY_ERR_LOG("材质尚未同步到 Proxy");
        hasErrors = true;
        return false;
    }
    const auto& tags = resource->Properties.Tags;
    if (!tags.GetTable()) {
        LogSkipped(passName, context, policy, filter.Description);
        return false;
    }
    if (tags.GetTable().Get() != filter.Table) {
        RADRAY_ERR_LOG("材质与 Processor 必须使用同一张标签表");
        hasErrors = true;
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

DepthPassProcessor::DepthPassProcessor(MeshPassDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                                       std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter) noexcept
    : MeshPassProcessor(drawListContext), _scene(&scene), _pipelines(pipelines), _policies(policies), _materialFilter(materialFilter) {}

void DepthPassProcessor::AddMeshBatch(const MeshBatchContext& context) {
    // 步骤 5a：Depth Processor 排除 ColorOnly，再检查材质 Surface=Opaque，接受的 Batch 才展开 Element。
    // 为什么：物体参与规则与材质语义同时满足才可绘制；登记后的 ID 比较无需字符串查找。
    // UE5 对照：Renderer/Private/DepthRendering.cpp 的 FDepthPassMeshProcessor::AddMeshBatch 返回 void。
    // 本例简化：按 BatchPassPolicy 与 Surface=Opaque 的 ID 对筛选；没有材质 fallback 或遮挡体尺寸判断。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ERR_LOG("Batch 没有样例 Pass 配置");
        HasErrors = true;
        return;
    }
    if (policy->Participation == PassParticipation::ColorOnly) {
        LogSkipped("Depth", context, *policy, "ColorOnly：不参加 Depth Pass");
        return;
    }
    if (!AcceptsMaterial(*_scene, context, *policy, _materialFilter, "Depth", HasErrors)) return;
    const auto pipelines = FindMaterialPipelines(*_scene, context.Batch.Material, _pipelines);
    if (!pipelines || !pipelines->Depth) {
        HasErrors = true;
        return;
    }
    if (!BuildMeshDrawCommands(context, {.Pipeline = pipelines->Depth.Get(), .AllowDeferredInstanceStream = true})) HasErrors = true;
}

ColorPassProcessor::ColorPassProcessor(MeshPassDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                                       std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter, MaterialParameterIndices parameters) noexcept
    : MeshPassProcessor(drawListContext), _scene(&scene), _pipelines(pipelines), _policies(policies), _materialFilter(materialFilter), _parameters(parameters) {}

void ColorPassProcessor::AddMeshBatch(const MeshBatchContext& context) {
    // 步骤 5b：Color Processor 排除 DepthOnly，再检查材质标签，并将模板参考值复制到本次输出。
    // 为什么：Processor 选择 PSO 与最终动态状态；录制阶段不再读取材质或 Scene。
    // UE5 对照：Renderer/Private/BasePassRendering.cpp 的 FBasePassMeshProcessor::AddMeshBatch。
    // 本例简化：ColorOnly 仍使用允许补写深度的 Color PSO，不复刻 UE 的完整 Base Pass。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ERR_LOG("Batch 没有样例 Pass 配置");
        HasErrors = true;
        return;
    }
    if (policy->Participation == PassParticipation::DepthOnly) {
        LogSkipped("Color", context, *policy, "DepthOnly：仅作深度遮挡，不参加 Color Pass");
        return;
    }
    if (!AcceptsMaterial(*_scene, context, *policy, _materialFilter, "Color", HasErrors)) return;
    const auto pipelines = FindMaterialPipelines(*_scene, context.Batch.Material, _pipelines);
    if (!pipelines || !pipelines->Color) {
        HasErrors = true;
        return;
    }
    MaterialConstants constants;
    if (!ExtractMaterialColor(*_scene, context.Batch.Material, _parameters, constants)) {
        RADRAY_ERR_LOG("Color Pass 的材质参数缺失或类型不匹配");
        HasErrors = true;
        return;
    }
    if (!BuildMeshDrawCommands(context, {.Pipeline = pipelines->Color.Get(),
                                         .StencilReference = pipelines->Resource->Properties.RenderState.StencilReference,
                                         .PushConstants = MeshPushConstantBinding{0, 0, std::as_bytes(std::span{&constants, 1})},
                                         .AllowDeferredInstanceStream = true})) HasErrors = true;
}

OutlinePassProcessor::OutlinePassProcessor(MeshPassDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                                           std::span<const BatchPassPolicy> policies, MaterialTagFilter opaqueFilter, MaterialTagFilter outlineFilter) noexcept
    : MeshPassProcessor(drawListContext), _scene(&scene), _pipelines(pipelines), _policies(policies),
      _opaqueFilter(opaqueFilter), _outlineFilter(outlineFilter) {}

void OutlinePassProcessor::AddMeshBatch(const MeshBatchContext& context) {
    // 步骤 5d：只接受参与颜色绘制、Surface=Opaque 且 Outline=Enabled 的 Batch。
    // 为什么：先确认原物体会在 Color Pass 写模板，再用相同 Section 的放大外壳描边；没有标签就不生成命令。
    // UE5 对照：Renderer/Private/CustomDepthRendering.cpp 的 FCustomDepthPassMeshProcessor 筛选 Batch 并设置模板状态。
    // 本例简化：描边标签是样例约定；UE 的 CustomDepth 参与标记/模板值通常来自 PrimitiveSceneProxy，不是这套标签。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ERR_LOG("Batch 没有样例 Pass 配置");
        HasErrors = true;
        return;
    }
    if (policy->Participation == PassParticipation::DepthOnly) {
        LogSkipped("Outline", context, *policy, "DepthOnly：不参加颜色绘制，也不描边");
        return;
    }
    if (!AcceptsMaterial(*_scene, context, *policy, _opaqueFilter, "Outline", HasErrors) ||
        !AcceptsMaterial(*_scene, context, *policy, _outlineFilter, "Outline", HasErrors)) return;
    const auto pipelines = FindMaterialPipelines(*_scene, context.Batch.Material, _pipelines);
    if (!pipelines || !pipelines->Outline) {
        HasErrors = true;
        return;
    }
    const MaterialConstants gold{{1.0f, 0.72f, 0.08f, 1.0f}};
    if (!BuildMeshDrawCommands(context, {.Pipeline = pipelines->Outline.Get(),
                                         .StencilReference = pipelines->Resource->Properties.RenderState.StencilReference,
                                         .PushConstants = MeshPushConstantBinding{0, 0, std::as_bytes(std::span{&gold, 1})},
                                         .AllowDeferredInstanceStream = true})) HasErrors = true;
}

TransparentPassProcessor::TransparentPassProcessor(MeshPassDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                                                   std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter, MaterialParameterIndices parameters) noexcept
    : MeshPassProcessor(drawListContext), _scene(&scene), _pipelines(pipelines), _policies(policies), _materialFilter(materialFilter), _parameters(parameters) {}

void TransparentPassProcessor::AddMeshBatch(const MeshBatchContext& context) {
    // 步骤 5e：颜色参与规则满足后，只接受 Surface=Transparent；不透明材质在这里被拒绝。
    // 为什么：标签决定本 Pass 接受谁，Blend/DepthWrite 决定如何绘制；只把 alpha 改小不会自动启用混合。
    // UE5 对照：Renderer/Private/BasePassRendering.cpp 的 FBasePassMeshProcessor::ShouldDraw 判断透明类别；
    // Renderer/Private/TranslucentRendering.cpp 根据 BlendMode 设置透明混合状态。
    // 本例简化：单个凸透明物体，仅绘制正面，普通 source-over；无折射、OIT 或透明三角形排序。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ERR_LOG("Batch 没有样例 Pass 配置");
        HasErrors = true;
        return;
    }
    if (policy->Participation == PassParticipation::DepthOnly) {
        LogSkipped("Transparent", context, *policy, "DepthOnly：不参加透明颜色绘制");
        return;
    }
    if (!AcceptsMaterial(*_scene, context, *policy, _materialFilter, "Transparent", HasErrors)) return;
    const auto pipelines = FindMaterialPipelines(*_scene, context.Batch.Material, _pipelines);
    if (!pipelines || !pipelines->Color) {
        HasErrors = true;
        return;
    }
    MaterialConstants constants;
    if (!ExtractMaterialColor(*_scene, context.Batch.Material, _parameters, constants)) {
        RADRAY_ERR_LOG("Transparent Pass 的材质参数缺失或类型不匹配");
        HasErrors = true;
        return;
    }
    if (!BuildMeshDrawCommands(context, {.Pipeline = pipelines->Color.Get(),
                                         .StencilReference = pipelines->Resource->Properties.RenderState.StencilReference,
                                         .PushConstants = MeshPushConstantBinding{0, 0, std::as_bytes(std::span{&constants, 1})},
                                         .AllowDeferredInstanceStream = true})) HasErrors = true;
}

bool PreparePassDrawCommands(MeshPassId passId, const Scene& scene,
                             std::span<const SparseSetHandle> primitives, std::span<const BatchPassPolicy> policies,
                             MeshPassFrameDrawList& output) {
    output.Clear();
    const array<std::string_view, 4> passNames{"Depth", "Color", "Outline", "Transparent"};
    if (passId.Value >= passNames.size() || primitives.size() > std::numeric_limits<uint32_t>::max()) return false;
    for (size_t primitiveIndex = 0; primitiveIndex < primitives.size(); ++primitiveIndex) {
        const auto primitiveHandle = primitives[primitiveIndex];
        if (!scene.Primitives.IsAlive(primitiveHandle)) return false;
        const auto& primitive = scene.Primitives.Get(primitiveHandle);
        for (const auto batchHandle : primitive.Meshes) {
            if (!scene.Meshes.IsAlive(batchHandle)) return false;
            const auto& batch = scene.Meshes.Get(batchHandle);
            if (std::none_of(batch.CachedCommands.begin(), batch.CachedCommands.end(), [&](const CachedMeshDrawCommandInfo& info) { return info.Pass == passId; })) continue;
            const VisibleMeshBatch visible{
                .Batch = batchHandle,
                .FirstInstanceOverride = static_cast<uint32_t>(primitiveIndex)};
            const auto firstCommand = output.VisibleCommands.size();
            if (!scene.GatherVisibleMeshDrawCommands(passId, std::span{&visible, 1}, output.VisibleCommands)) return false;
            const auto policy = FindPolicy(policies, batchHandle);
            if (!policy) return false;
            size_t drawIndex = firstCommand;
            for (const auto& info : batch.CachedCommands) {
                if (info.Pass != passId) continue;
                RADRAY_INFO_LOG("{} Pass 接受：对象={} Primitive={} Batch={} Element={} → Draw={}，CommandHandle={}",
                                passNames[passId.Value], policy->ObjectName, primitiveHandle, batchHandle, info.ElementIndex, drawIndex++, info.CommandHandle);
            }
        }
    }
    output.SortVisibleCommands();
    return true;
}

MeshDrawingFramePipeline::MeshDrawingFramePipeline(GpuResources& gpu, const Scene& scene, const PassContext& view,
                                                   std::span<const SparseSetHandle> candidates, std::span<const BatchPassPolicy> policies,
                                                   MaterialParameterIndices parameters) noexcept
    : _gpu(gpu), _scene(scene), _view(view), _candidates(candidates), _policies(policies),
      _depthProcessor(_defaultOutput, scene, gpu.Pipelines, policies, {&gpu.MaterialTags, gpu.OpaqueTag, "材质缺少或不匹配 Surface=Opaque"}),
      _colorProcessor(_defaultOutput, scene, gpu.Pipelines, policies, {&gpu.MaterialTags, gpu.OpaqueTag, "材质缺少或不匹配 Surface=Opaque"}, parameters),
      _outlineProcessor(_defaultOutput, scene, gpu.Pipelines, policies, {&gpu.MaterialTags, gpu.OpaqueTag, "材质缺少或不匹配 Surface=Opaque"},
                        {&gpu.MaterialTags, gpu.OutlineTag, "材质缺少或不匹配 Outline=Enabled"}),
      _transparentProcessor(_defaultOutput, scene, gpu.Pipelines, policies, {&gpu.MaterialTags, gpu.TransparentTag, "材质缺少或不匹配 Surface=Transparent"}, parameters),
      _passes{{{{0}, &_depthProcessor}, {{1}, &_colorProcessor}, {{2}, &_outlineProcessor}, {{3}, &_transparentProcessor}}} {}

bool MeshDrawingFramePipeline::Initialize(uint32_t slotCount) {
    if (slotCount == 0 || !_frames.empty() || _candidates.empty() || _candidates.size() > std::numeric_limits<uint32_t>::max()) return false;
    for (uint32_t index = 0; index < slotCount; ++index) {
        auto frame = make_unique<GpuFrameResources>();
        if (!frame->Initialize(_gpu, static_cast<uint32_t>(_candidates.size()))) return false;
        _frames.push_back(std::move(frame));
    }
    return true;
}

std::span<const MeshPassRegistration> MeshDrawingFramePipeline::GetMeshPassRegistrations() noexcept { return _passes; }

bool MeshDrawingFramePipeline::HasErrors() const noexcept {
    return _depthProcessor.HasErrors || _colorProcessor.HasErrors || _outlineProcessor.HasErrors || _transparentProcessor.HasErrors;
}

void MeshDrawingFramePipeline::Extract(const Scene& source, FrameSlot& slot) noexcept {
    if (&source != &_scene || slot.GetScene().Get() != &source) RADRAY_ABORT("MeshDrawingFramePipeline: unexpected source scene");
    if (slot.GetIndex() >= _frames.size()) RADRAY_ABORT("MeshDrawingFramePipeline: slot index exceeds frame resources");
    if (HasErrors() || source.HasDirtyBatches()) RADRAY_ABORT("MeshDrawingFramePipeline: mesh command synchronization failed");
    auto& resources = *_frames[slot.GetIndex()];
    vector<Eigen::Matrix4f> matrices;
    matrices.reserve(_candidates.size());
    for (const auto handle : _candidates) {
        if (!source.Primitives.IsAlive(handle)) RADRAY_ABORT("MeshDrawingFramePipeline: stale primitive handle");
        matrices.push_back(source.Primitives.Get(handle).LocalToWorld.ToMatrix());
    }
    if (!resources.UploadSceneParameters(_view.ViewProjection, matrices)) RADRAY_ABORT("MeshDrawingFramePipeline: cannot upload scene parameters");
    for (uint32_t pass = 0; pass < _passes.size(); ++pass) {
        if (!PreparePassDrawCommands({pass}, source, _candidates, _policies, slot.GetPassDrawList({pass}))) RADRAY_ABORT("MeshDrawingFramePipeline: cannot prepare pass {}", pass);
    }
    const auto depth = slot.GetPassDrawList({0}).VisibleCommands.size();
    const auto color = slot.GetPassDrawList({1}).VisibleCommands.size();
    const auto outline = slot.GetPassDrawList({2}).VisibleCommands.size();
    const auto transparent = slot.GetPassDrawList({3}).VisibleCommands.size();
    RADRAY_INFO_LOG("步骤 6：Frame={} Slot={}，Depth={}，Color={}，Outline={}，Transparent={}，总计 {} 条单实例 Draw",
                    slot.GetFrameId(), slot.GetIndex(), depth, color, outline, transparent, depth + color + outline + transparent);
}

bool MeshDrawingFramePipeline::RecordPass(render::GraphicsCommandEncoder& encoder, FrameSlot& slot,
                                          MeshPassId passId, const GpuFrameResources& resources) {
    const auto source = slot.GetScene();
    if (!source) return false;
    encoder.SetViewport(_view.ViewportRect);
    encoder.SetScissor(_view.Scissor);
    const MeshShaderParameterBinding passBinding{0, resources.PassParameterSets[passId == MeshPassId{2} ? 1 : 0].Get(), {}};
    const MeshPassSubmitArgs submitArgs{
        .ParameterSets = std::span{&passBinding, 1},
        .InstanceStream = render::VertexBufferBinding{1, {resources.InstanceBuffer.Get(), 0, resources.InstanceBuffer->GetDesc().Size}}};
    return slot.GetPassDrawList(passId).Submit(encoder, source->GetCachedDrawList(passId), submitArgs);
}

void MeshDrawingFramePipeline::Record(FrameSlot& slot) noexcept {
    if (!slot.GetScene() || slot.GetIndex() >= _frames.size()) RADRAY_ABORT("MeshDrawingFramePipeline: invalid recording slot");
    auto& resources = *_frames[slot.GetIndex()];
    if (resources.CompletionValue >= std::numeric_limits<uint64_t>::max() - 1) RADRAY_ABORT("MeshDrawingFramePipeline: fence values exhausted");
    auto acquired = slot.AcquireCommandBuffer();
    if (!acquired) RADRAY_ABORT("MeshDrawingFramePipeline: cannot acquire command buffer");
    auto& command = *acquired;
    command.Begin();
    if (!_geometrySubmitted) {
        const render::ResourceBarrierDescriptor geometry[]{
            render::BarrierBufferDescriptor{_gpu.VertexBuffer.Get(), render::BufferState::HostWrite, render::BufferState::Vertex},
            render::BarrierBufferDescriptor{_gpu.IndexBuffer.Get(), render::BufferState::HostWrite, render::BufferState::Index}};
        command.ResourceBarrier(geometry);
    }
    const render::ResourceBarrierDescriptor initialBuffers[]{
        render::BarrierBufferDescriptor{resources.InstanceBuffer.Get(), render::BufferState::HostWrite, render::BufferState::Vertex},
        render::BarrierBufferDescriptor{resources.PassBuffers[0].Get(), render::BufferState::HostWrite, render::BufferState::CBuffer},
        render::BarrierBufferDescriptor{resources.PassBuffers[1].Get(), render::BufferState::HostWrite, render::BufferState::CBuffer},
        render::BarrierBufferDescriptor{resources.Readback.Get(), resources.CompletionValue == 0 ? render::BufferState::Undefined : render::BufferState::HostRead, render::BufferState::CopyDestination}};
    command.ResourceBarrier(initialBuffers);
    TransitionTexture(command, *resources.DepthTexture, resources.CompletionValue == 0 ? render::TextureState::Undefined : render::TextureState::DepthWrite, render::TextureState::DepthWrite);
    RADRAY_INFO_LOG("步骤 7：录制 Depth Pass，清除并写入深度");
    auto depthEncoder = command.BeginRenderPass({.DepthStencil = render::RenderPassDepthStencilAttachment{
                                                     .View = resources.DepthWriteView.Get(), .DepthLoad = render::LoadAction::Clear, .DepthStore = render::StoreAction::Store, .StencilLoad = render::LoadAction::Clear, .StencilStore = render::StoreAction::Store, .Clear = {1, 0}},
                                                 .Width = resources.Width,
                                                 .Height = resources.Height,
                                                 .Name = "Tutorial Depth"});
    if (!depthEncoder) RADRAY_ABORT("MeshDrawingFramePipeline: cannot begin depth pass");
    const bool depthRecorded = RecordPass(*depthEncoder, slot, {0}, resources);
    command.EndRenderPass(depthEncoder.Release());
    if (!depthRecorded) RADRAY_ABORT("MeshDrawingFramePipeline: cannot record depth pass");

    TransitionTexture(command, *resources.DepthTexture, render::TextureState::DepthWrite, render::TextureState::DepthWrite);
    TransitionTexture(command, *resources.ColorTexture, resources.CompletionValue == 0 ? render::TextureState::Undefined : render::TextureState::CopySource, render::TextureState::RenderTarget);
    RADRAY_INFO_LOG("步骤 7：录制 Color Pass，Load 同一深度，LessEqual 测试并补写深度");
    const render::RenderPassColorAttachment colorAttachment{
        resources.ColorView.Get(), render::LoadAction::Clear, render::StoreAction::Store, {{0.035f, 0.055f, 0.085f, 1}}};
    auto colorEncoder = command.BeginRenderPass({.Colors = std::span{&colorAttachment, 1},
                                                 .DepthStencil = render::RenderPassDepthStencilAttachment{
                                                     .View = resources.DepthWriteView.Get(), .DepthLoad = render::LoadAction::Load, .DepthStore = render::StoreAction::Store, .StencilLoad = render::LoadAction::Load, .StencilStore = render::StoreAction::Store},
                                                 .Width = resources.Width,
                                                 .Height = resources.Height,
                                                 .Name = "Tutorial Color"});
    if (!colorEncoder) RADRAY_ABORT("MeshDrawingFramePipeline: cannot begin color pass");
    const bool colorRecorded = RecordPass(*colorEncoder, slot, {1}, resources);
    command.EndRenderPass(colorEncoder.Release());
    if (!colorRecorded) RADRAY_ABORT("MeshDrawingFramePipeline: cannot record color pass");

    TransitionTexture(command, *resources.DepthTexture, render::TextureState::DepthWrite, render::TextureState::DepthWrite);
    TransitionTexture(command, *resources.ColorTexture, render::TextureState::RenderTarget, render::TextureState::RenderTarget);
    const render::RenderPassColorAttachment loadedColor{
        resources.ColorView.Get(), render::LoadAction::Load, render::StoreAction::Store, {}};
    RADRAY_INFO_LOG("步骤 7a：Outline Pass，Load 模板；NotEqual 7、禁止模板/深度写入，金色外壳放大 1.12 倍");
    auto outlineEncoder = command.BeginRenderPass({.Colors = std::span{&loadedColor, 1},
                                                   .DepthStencil = render::RenderPassDepthStencilAttachment{
                                                       .View = resources.DepthWriteView.Get(), .DepthLoad = render::LoadAction::Load, .DepthStore = render::StoreAction::Store, .StencilLoad = render::LoadAction::Load, .StencilStore = render::StoreAction::Store},
                                                   .Width = resources.Width,
                                                   .Height = resources.Height,
                                                   .Name = "Tutorial Outline"});
    if (!outlineEncoder) RADRAY_ABORT("MeshDrawingFramePipeline: cannot begin outline pass");
    const bool outlineRecorded = RecordPass(*outlineEncoder, slot, {2}, resources);
    command.EndRenderPass(outlineEncoder.Release());
    if (!outlineRecorded) RADRAY_ABORT("MeshDrawingFramePipeline: cannot record outline pass");

    TransitionTexture(command, *resources.DepthTexture, render::TextureState::DepthWrite, render::TextureState::DepthWrite);
    TransitionTexture(command, *resources.ColorTexture, render::TextureState::RenderTarget, render::TextureState::RenderTarget);
    RADRAY_INFO_LOG("步骤 7b：Transparent Pass，Load 颜色并进行 alpha 混合；LessEqual 测试，不写深度");
    auto transparentEncoder = command.BeginRenderPass({.Colors = std::span{&loadedColor, 1},
                                                       .DepthStencil = render::RenderPassDepthStencilAttachment{
                                                           .View = resources.DepthWriteView.Get(), .DepthLoad = render::LoadAction::Load, .DepthStore = render::StoreAction::Store, .StencilLoad = render::LoadAction::Load, .StencilStore = render::StoreAction::Store},
                                                       .Width = resources.Width,
                                                       .Height = resources.Height,
                                                       .Name = "Tutorial Transparent"});
    if (!transparentEncoder) RADRAY_ABORT("MeshDrawingFramePipeline: cannot begin transparent pass");
    const bool transparentRecorded = RecordPass(*transparentEncoder, slot, {3}, resources);
    command.EndRenderPass(transparentEncoder.Release());
    if (!transparentRecorded) RADRAY_ABORT("MeshDrawingFramePipeline: cannot record transparent pass");

    TransitionTexture(command, *resources.ColorTexture, render::TextureState::RenderTarget, render::TextureState::CopySource);
    command.CopyTextureToBuffer(resources.Readback.Get(), 0, resources.ColorTexture.Get(), {0, 1, 0, 1});
    const render::ResourceBarrierDescriptor toHost = render::BarrierBufferDescriptor{
        resources.Readback.Get(), render::BufferState::CopyDestination, render::BufferState::HostRead};
    command.ResourceBarrier(std::span{&toHost, 1});
    const render::ResourceBarrierDescriptor toUpload[]{
        render::BarrierBufferDescriptor{resources.InstanceBuffer.Get(), render::BufferState::Vertex, render::BufferState::HostWrite},
        render::BarrierBufferDescriptor{resources.PassBuffers[0].Get(), render::BufferState::CBuffer, render::BufferState::HostWrite},
        render::BarrierBufferDescriptor{resources.PassBuffers[1].Get(), render::BufferState::CBuffer, render::BufferState::HostWrite}};
    command.ResourceBarrier(toUpload);
    command.End();
    array submitted{&command};
    array fences{resources.CompletionFence.Get()};
    array values{resources.CompletionValue + 1};
    slot.GetQueue().Submit({.CmdBuffers = submitted, .SignalFences = fences, .SignalValues = values});
    resources.CompletionValue = values[0];
    _geometrySubmitted = true;
    RADRAY_INFO_LOG("步骤 8：提交 Frame={} Slot={} Fence={}，复用该槽时才等待", slot.GetFrameId(), slot.GetIndex(), resources.CompletionValue);
}

void MeshDrawingFramePipeline::Wait(FrameSlot& slot) noexcept {
    auto& resources = *_frames[slot.GetIndex()];
    resources.CompletionFence->Wait(resources.CompletionValue);
    RADRAY_INFO_LOG("等待完成：Frame={} Slot={} Fence={}", slot.GetFrameId(), slot.GetIndex(), resources.CompletionValue);
}

bool MeshDrawingFramePipeline::SaveImage(uint32_t slotIndex, const std::filesystem::path& output) const {
    if (slotIndex >= _frames.size()) return false;
    const auto& resources = *_frames[slotIndex];
    if (resources.CompletionValue == 0 || resources.CompletionFence->GetCompletedValue() < resources.CompletionValue) return false;
    return resources.SaveImage(output);
}

}  // namespace radray::examples::mesh_drawing
