#include "pipeline.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <radray/logger.h>

namespace radray::examples::mesh_drawing {
namespace {

bool ExtractMaterialColor(const Scene& scene, MaterialHandle materialHandle,
                          MaterialParameterIndices parameters, MaterialConstants& outConstants) {
    if (!scene.GetMaterial(materialHandle)) return false;
    const auto& proxy = (*scene.GetMaterial(materialHandle));
    const auto baseValue = proxy.GetParameter(parameters.BaseColor);
    const auto brightnessValue = proxy.GetParameter(parameters.Brightness);
    const Nullable<const array<float, 4>*> baseColor = baseValue ? std::get_if<array<float, 4>>(baseValue.Get()) : nullptr;
    const Nullable<const float*> brightness = brightnessValue ? std::get_if<float>(brightnessValue.Get()) : nullptr;
    if (!baseColor || !brightness) return false;
    outConstants.Color = *baseColor;
    for (size_t index = 0; index < 3; ++index) outConstants.Color[index] *= *brightness;
    return true;
}

Nullable<const BatchPassPolicy*> FindPolicy(std::span<const BatchPassPolicy> policies, MeshBatchHandle handle) {
    const auto found = std::find_if(policies.begin(), policies.end(), [&](const BatchPassPolicy& policy) { return policy.Batch == handle; });
    return found == policies.end() ? nullptr : &*found;
}

void LogSkipped(std::string_view passName, const MeshBatchContext& context, const BatchPassPolicy& policy, std::string_view reason) {
    RADRAY_INFO_LOG("{} Pass 跳过：对象={} Primitive={} Batch={}（未生成 DrawCommand），原因={}",
                    passName, policy.ObjectName, context.PrimitiveHandle, context.BatchHandle, reason);
}

bool AcceptsMaterial(const Scene& scene, const MeshBatchContext& context, const BatchPassPolicy& policy,
                     MaterialTagFilter filter, std::string_view passName) {
    if (!scene.GetMaterial(context.Batch.Material)) {
        RADRAY_ABORT("材质句柄失效：{}", context.Batch.Material);
    }
    const auto resource = (*scene.GetMaterial(context.Batch.Material)).GetResource();
    if (!resource) {
        RADRAY_ABORT("材质尚未同步到 Proxy");
    }
    const auto& tags = resource->Properties.Tags;
    if (!tags.GetTable()) {
        LogSkipped(passName, context, policy, filter.Description);
        return false;
    }
    if (tags.GetTable().Get() != filter.Table) {
        RADRAY_ABORT("材质与 Processor 必须使用同一张标签表");
    }
    if (!tags.Matches(filter.Required.Key, filter.Required.Value)) {
        LogSkipped(passName, context, policy, filter.Description);
        return false;
    }
    return true;
}

const MaterialPipelines* FindMaterialPipelines(
    const Scene& scene, MaterialHandle material, std::span<const MaterialPipelines> pipelines) {
    if (!scene.GetMaterial(material)) {
        RADRAY_ABORT("材质句柄失效：{}", material);
    }
    const auto resource = (*scene.GetMaterial(material)).GetResource();
    const auto found = std::find_if(pipelines.begin(), pipelines.end(), [&](const MaterialPipelines& entry) { return entry.Resource == resource.Get(); });
    if (!resource || found == pipelines.end()) RADRAY_ABORT("材质未同步到 Proxy 或其 PSO 尚未准备：{}", material);
    return &*found;
}

}  // namespace

DepthPassProcessor::DepthPassProcessor(const Scene& scene, std::span<const MaterialPipelines> pipelines,
                                       std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter) noexcept
    : _scene(&scene), _pipelines(pipelines), _policies(policies), _materialFilter(materialFilter) {}

void DepthPassProcessor::AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) {
    // 步骤 5a：Depth Processor 排除 ColorOnly，再检查材质 Surface=Opaque，接受的 Batch 才展开 Element。
    // 为什么：物体参与规则与材质语义同时满足才可绘制；登记后的 ID 比较无需字符串查找。
    // UE5 对照：Renderer/Private/DepthRendering.cpp 的 FDepthPassMeshProcessor::AddMeshBatch 返回 void。
    // 本例简化：按 BatchPassPolicy 与 Surface=Opaque 的 ID 对筛选；没有材质 fallback 或遮挡体尺寸判断。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ABORT("Batch 没有样例 Pass 配置");
    }
    if (policy->Participation == PassParticipation::ColorOnly) {
        LogSkipped("Depth", context, *policy, "ColorOnly：不参加 Depth Pass");
        return;
    }
    if (!AcceptsMaterial(*_scene, context, *policy, _materialFilter, "Depth")) return;
    const auto pipelines = FindMaterialPipelines(*_scene, context.Batch.Material, _pipelines);
    if (!pipelines->Depth) RADRAY_ABORT("缺失 Depth PSO");
    BuildMeshDrawCommands(context, {.Pipeline = pipelines->Depth.Get(), .AllowDeferredInstanceStream = true}, output);
}

ColorPassProcessor::ColorPassProcessor(const Scene& scene, std::span<const MaterialPipelines> pipelines,
                                       std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter, MaterialParameterIndices parameters) noexcept
    : _scene(&scene), _pipelines(pipelines), _policies(policies), _materialFilter(materialFilter), _parameters(parameters) {}

void ColorPassProcessor::AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) {
    // 步骤 5b：Color Processor 排除 DepthOnly，再检查材质标签，并将模板参考值复制到本次输出。
    // 为什么：Processor 选择 PSO 与最终动态状态；录制阶段不再读取材质或 Scene。
    // UE5 对照：Renderer/Private/BasePassRendering.cpp 的 FBasePassMeshProcessor::AddMeshBatch。
    // 本例简化：ColorOnly 仍使用允许补写深度的 Color PSO，不复刻 UE 的完整 Base Pass。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ABORT("Batch 没有样例 Pass 配置");
    }
    if (policy->Participation == PassParticipation::DepthOnly) {
        LogSkipped("Color", context, *policy, "DepthOnly：仅作深度遮挡，不参加 Color Pass");
        return;
    }
    if (!AcceptsMaterial(*_scene, context, *policy, _materialFilter, "Color")) return;
    const auto pipelines = FindMaterialPipelines(*_scene, context.Batch.Material, _pipelines);
    if (!pipelines->Color) RADRAY_ABORT("缺失 Color PSO");
    MaterialConstants constants;
    if (!ExtractMaterialColor(*_scene, context.Batch.Material, _parameters, constants)) {
        RADRAY_ABORT("Color Pass 的材质参数缺失或类型不匹配");
    }
    BuildMeshDrawCommands(context, {.Pipeline = pipelines->Color.Get(), .StencilReference = pipelines->Resource->Properties.RenderState.StencilReference, .PushConstants = MeshPushConstantBinding{0, 0, std::as_bytes(std::span{&constants, 1})}, .AllowDeferredInstanceStream = true}, output);
}

OutlinePassProcessor::OutlinePassProcessor(const Scene& scene, std::span<const MaterialPipelines> pipelines,
                                           std::span<const BatchPassPolicy> policies, MaterialTagFilter opaqueFilter, MaterialTagFilter outlineFilter) noexcept
    : _scene(&scene), _pipelines(pipelines), _policies(policies),
      _opaqueFilter(opaqueFilter), _outlineFilter(outlineFilter) {}

void OutlinePassProcessor::AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) {
    // 步骤 5d：只接受参与颜色绘制、Surface=Opaque 且 Outline=Enabled 的 Batch。
    // 为什么：先确认原物体会在 Color Pass 写模板，再用相同 Section 的放大外壳描边；没有标签就不生成命令。
    // UE5 对照：Renderer/Private/CustomDepthRendering.cpp 的 FCustomDepthPassMeshProcessor 筛选 Batch 并设置模板状态。
    // 本例简化：描边标签是样例约定；UE 的 CustomDepth 参与标记/模板值通常来自 PrimitiveSceneProxy，不是这套标签。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ABORT("Batch 没有样例 Pass 配置");
    }
    if (policy->Participation == PassParticipation::DepthOnly) {
        LogSkipped("Outline", context, *policy, "DepthOnly：不参加颜色绘制，也不描边");
        return;
    }
    if (!AcceptsMaterial(*_scene, context, *policy, _opaqueFilter, "Outline") ||
        !AcceptsMaterial(*_scene, context, *policy, _outlineFilter, "Outline")) return;
    const auto pipelines = FindMaterialPipelines(*_scene, context.Batch.Material, _pipelines);
    if (!pipelines->Outline) RADRAY_ABORT("缺失 Outline PSO");
    const MaterialConstants gold{{1.0f, 0.72f, 0.08f, 1.0f}};
    BuildMeshDrawCommands(context, {.Pipeline = pipelines->Outline.Get(), .StencilReference = pipelines->Resource->Properties.RenderState.StencilReference, .PushConstants = MeshPushConstantBinding{0, 0, std::as_bytes(std::span{&gold, 1})}, .AllowDeferredInstanceStream = true}, output);
}

TransparentPassProcessor::TransparentPassProcessor(const Scene& scene, std::span<const MaterialPipelines> pipelines,
                                                   std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter, MaterialParameterIndices parameters) noexcept
    : _scene(&scene), _pipelines(pipelines), _policies(policies), _materialFilter(materialFilter), _parameters(parameters) {}

void TransparentPassProcessor::AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) {
    // 步骤 5e：颜色参与规则满足后，只接受 Surface=Transparent；不透明材质在这里被拒绝。
    // 为什么：标签决定本 Pass 接受谁，Blend/DepthWrite 决定如何绘制；只把 alpha 改小不会自动启用混合。
    // UE5 对照：Renderer/Private/BasePassRendering.cpp 的 FBasePassMeshProcessor::ShouldDraw 判断透明类别；
    // Renderer/Private/TranslucentRendering.cpp 根据 BlendMode 设置透明混合状态。
    // 本例简化：单个凸透明物体，仅绘制正面，普通 source-over；无折射、OIT 或透明三角形排序。
    const auto policy = FindPolicy(_policies, context.BatchHandle);
    if (!policy) {
        RADRAY_ABORT("Batch 没有样例 Pass 配置");
    }
    if (policy->Participation == PassParticipation::DepthOnly) {
        LogSkipped("Transparent", context, *policy, "DepthOnly：不参加透明颜色绘制");
        return;
    }
    if (!AcceptsMaterial(*_scene, context, *policy, _materialFilter, "Transparent")) return;
    const auto pipelines = FindMaterialPipelines(*_scene, context.Batch.Material, _pipelines);
    if (!pipelines->Color) RADRAY_ABORT("缺失 Color PSO");
    MaterialConstants constants;
    if (!ExtractMaterialColor(*_scene, context.Batch.Material, _parameters, constants)) {
        RADRAY_ABORT("Transparent Pass 的材质参数缺失或类型不匹配");
    }
    BuildMeshDrawCommands(context, {.Pipeline = pipelines->Color.Get(), .StencilReference = pipelines->Resource->Properties.RenderState.StencilReference, .PushConstants = MeshPushConstantBinding{0, 0, std::as_bytes(std::span{&constants, 1})}, .AllowDeferredInstanceStream = true}, output);
}

bool PreparePassDrawCommands(MeshPassId passId, const Scene& scene,
                             std::span<const PrimitiveHandle> primitives, std::span<const BatchPassPolicy> policies,
                             MeshPassFrameDrawList& output) {
    output.Clear();
    const array<std::string_view, 4> passNames{"Depth", "Color", "Outline", "Transparent"};
    if (passId.Value >= passNames.size() || primitives.size() > std::numeric_limits<uint32_t>::max()) return false;
    for (size_t primitiveIndex = 0; primitiveIndex < primitives.size(); ++primitiveIndex) {
        const auto primitiveHandle = primitives[primitiveIndex];
        if (!scene.GetPrimitive(primitiveHandle)) return false;
        const auto& primitive = (*scene.GetPrimitive(primitiveHandle));
        for (const auto batchHandle : primitive.Meshes) {
            if (!scene.GetMeshBatch(batchHandle)) return false;
            const auto cache = scene.GetPassCache(passId);
            if (!cache || !cache->GetCommand(batchHandle.GetIndex())) continue;
            const VisibleMeshBatch visible{
                .Batch = batchHandle,
                .FirstInstanceOverride = static_cast<uint32_t>(primitiveIndex)};
            const auto firstCommand = output.VisibleCommands.size();
            if (!scene.GatherVisibleMeshDrawCommands(passId, std::span{&visible, 1}, output)) return false;
            const auto policy = FindPolicy(policies, batchHandle);
            if (!policy) return false;
            RADRAY_INFO_LOG("{} Pass 接受：对象={} Primitive={} Batch={} ElementMask={} → BatchCommand={}",
                            passNames[passId.Value], policy->ObjectName, primitiveHandle, batchHandle, output.VisibleCommands[firstCommand].ElementMask, firstCommand);
        }
    }
    return true;
}

struct MeshDrawingFramePipeline::ExtractedFramePayload {
    vector<InstanceData> Instances;
    array<vector<byte>, 2> PassBytes;
};

MeshDrawingFramePipeline::MeshDrawingFramePipeline(GpuResources& gpu, const Scene& scene, const PassContext& view,
                                                   std::span<const PrimitiveHandle> candidates, std::span<const BatchPassPolicy> policies,
                                                   MaterialParameterIndices parameters, uint32_t slotCount) noexcept
    : RenderGraphFramePipeline(*gpu.Device, slotCount), _gpu(gpu), _scene(scene), _view(view), _candidates(candidates), _policies(policies),
      _depthProcessor(scene, gpu.Pipelines, policies, {&gpu.MaterialTags, gpu.OpaqueTag, "材质缺少或不匹配 Surface=Opaque"}),
      _colorProcessor(scene, gpu.Pipelines, policies, {&gpu.MaterialTags, gpu.OpaqueTag, "材质缺少或不匹配 Surface=Opaque"}, parameters),
      _outlineProcessor(scene, gpu.Pipelines, policies, {&gpu.MaterialTags, gpu.OpaqueTag, "材质缺少或不匹配 Surface=Opaque"},
                        {&gpu.MaterialTags, gpu.OutlineTag, "材质缺少或不匹配 Outline=Enabled"}),
      _transparentProcessor(scene, gpu.Pipelines, policies, {&gpu.MaterialTags, gpu.TransparentTag, "材质缺少或不匹配 Surface=Transparent"}, parameters),
      _passes{{{{0}, &_depthProcessor}, {{1}, &_colorProcessor}, {{2}, &_outlineProcessor}, {{3}, &_transparentProcessor, MeshPassSortMode::DepthFirst}}},
      _payloads(slotCount) {}

bool MeshDrawingFramePipeline::Initialize() {
    if (_payloads.empty() || !_frames.empty() || _candidates.empty() || _candidates.size() > std::numeric_limits<uint32_t>::max()) return false;
    for (uint32_t index = 0; index < _payloads.size(); ++index) {
        auto frame = make_unique<GpuFrameResources>();
        if (!frame->Initialize(_gpu, static_cast<uint32_t>(_candidates.size()))) return false;
        _frames.push_back(std::move(frame));
    }
    return true;
}

std::span<const MeshPassRegistration> MeshDrawingFramePipeline::GetMeshPassRegistrations() noexcept { return _passes; }

void MeshDrawingFramePipeline::ExtractFrameData(FrameSlot& slot) noexcept {
    if (!_input) RADRAY_ABORT("MeshDrawingFramePipeline: missing input");
    if (slot.GetIndex() >= _frames.size()) RADRAY_ABORT("MeshDrawingFramePipeline: slot index exceeds frame resources");
    if (_input->HasDirtyBatches()) RADRAY_ABORT("MeshDrawingFramePipeline: mesh command synchronization failed");
    const auto& resources = *_frames[slot.GetIndex()];
    auto* payload = slot.GetArena().Emplace<ExtractedFramePayload>();
    _payloads[slot.GetIndex()] = payload;
    payload->Instances.reserve(_candidates.size());
    for (const auto handle : _candidates) {
        if (!_input->GetPrimitive(handle)) RADRAY_ABORT("MeshDrawingFramePipeline: stale primitive handle");
        const Eigen::Matrix4f matrix = (*_input->GetPrimitive(handle)).LocalToWorld.ToMatrix();
        auto& instance = payload->Instances.emplace_back();
        std::memcpy(instance.LocalToWorld.data(), matrix.data(), sizeof(InstanceData));
    }
    for (size_t index = 0; index < payload->PassBytes.size(); ++index) {
        PassConstants constants;
        std::memcpy(constants.ViewProjection.data(), _view.ViewProjection.data(), sizeof(constants.ViewProjection));
        constants.LocalScale = index == 0 ? 1.0f : 1.12f;
        auto& bytes = payload->PassBytes[index];
        bytes.resize(resources.PassBuffers[index]->GetDesc().Size, byte{0});
        std::memcpy(bytes.data(), &constants, sizeof(constants));
    }
}

void MeshDrawingFramePipeline::SetupGraph(RDGBuilder& graph, FrameSlot& slot) noexcept {
    const auto slotIndex = slot.GetIndex();
    auto& resources = *_frames[slotIndex];
    const auto& payload = *_payloads[slotIndex];
    const auto vb = graph.ImportBuffer(_gpu.VertexBuffer.Get(), _geometrySubmitted ? render::BufferState::Vertex : render::BufferState::Undefined,
                                       render::BufferState::Vertex);
    const auto ib = graph.ImportBuffer(_gpu.IndexBuffer.Get(), _geometrySubmitted ? render::BufferState::Index : render::BufferState::Undefined,
                                       render::BufferState::Index);
    if (!_geometrySubmitted) {
        graph.UploadBuffer(vb, 0, _gpu.PendingVertexBytes);
        graph.UploadBuffer(ib, 0, _gpu.PendingIndexBytes);
        graph.MarkOutput(vb);
        graph.MarkOutput(ib);
    }
    const auto instances = graph.ImportBuffer(resources.InstanceBuffer.Get(), render::BufferState::HostWrite, render::BufferState::HostWrite);
    graph.UploadBuffer(instances, 0, std::as_bytes(std::span{payload.Instances}));
    array<RDGBufferHandle, 2> constants;
    for (size_t index = 0; index < constants.size(); ++index) {
        constants[index] = graph.ImportBuffer(resources.PassBuffers[index].Get(), render::BufferState::HostWrite, render::BufferState::HostWrite);
        graph.UploadBuffer(constants[index], 0, payload.PassBytes[index]);
    }
    const auto depth = graph.CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = resources.Width, .Height = resources.Height, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::D32_FLOAT_S8_UINT, .Memory = render::MemoryType::Device, .Usage = render::TextureUse::DepthStencilWrite});
    const auto color = graph.CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = resources.Width, .Height = resources.Height, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::RGBA8_UNORM, .Memory = render::MemoryType::Device, .Usage = render::TextureUse::RenderTarget | render::TextureUse::CopySource});
    const auto readback = graph.ImportBuffer(resources.Readback.Get(), resources.CompletionValue == 0 ? render::BufferState::Undefined : render::BufferState::HostRead,
                                             render::BufferState::HostRead);
    const array<std::string_view, 4> names{"Tutorial Depth", "Tutorial Color", "Tutorial Outline", "Tutorial Transparent"};
    for (uint32_t index = 0; index < names.size(); ++index) {
        graph.AddRasterPass(names[index], [&, index](RDGRasterPassBuilder& pass) {
            const auto load = index == 0 ? render::LoadAction::Clear : render::LoadAction::Load;
            pass.UseMeshPass({index})
                .ReadBuffer(vb, render::BufferState::Vertex)
                .ReadBuffer(ib, render::BufferState::Index)
                .ReadBuffer(instances, render::BufferState::Vertex)
                .ReadBuffer(constants[index == 2 ? 1 : 0], render::BufferState::CBuffer)
                .SetDepthStencil(depth, render::TextureState::DepthWrite, load, render::StoreAction::Store, load, render::StoreAction::Store, {1.0f, 0});
            if (index != 0) {
                pass.WriteColor(0, color, index == 1 ? render::LoadAction::Clear : render::LoadAction::Load,
                                render::StoreAction::Store, {{0.035f, 0.055f, 0.085f, 1.0f}});
            } }, [this, index, slotIndex](RDGRasterPassContext& ctx) {
            RADRAY_INFO_LOG("步骤 7：RDG 录制 MeshPass={}", index);
            return RecordPass(ctx.GetEncoder(), ctx.GetSlot(), {index}, *_frames[slotIndex]); });
    }
    graph.AddCopyPass("Tutorial Readback", [&](RDGCopyPassBuilder& pass) { pass.ReadTexture(color).WriteBuffer(readback); }, [color, readback](RDGCopyPassContext& ctx) {
        ctx.GetCommandBuffer().CopyTextureToBuffer(&ctx.Resolve(readback), 0, &ctx.Resolve(color), {0, 1, 0, 1});
        return true; });
    graph.MarkOutput(readback);
}

void MeshDrawingFramePipeline::ExtractMeshPass(MeshPassId pass, FrameSlot& slot, MeshPassFrameDrawList& drawList) noexcept {
    if (!PreparePassDrawCommands(pass, *_input, _candidates, _policies, drawList)) RADRAY_ABORT("MeshDrawingFramePipeline: cannot prepare pass {}", pass.Value);
    RADRAY_INFO_LOG("步骤 6：Frame={} Slot={} MeshPass={}，{} 条单实例 Draw", slot.GetFrameId(), slot.GetIndex(), pass.Value, drawList.VisibleCommands.size());
}

bool MeshDrawingFramePipeline::RecordPass(render::GraphicsCommandEncoder& encoder, FrameSlot& slot,
                                          MeshPassId passId, const GpuFrameResources& resources) {
    encoder.SetViewport(_view.ViewportRect);
    encoder.SetScissor(_view.Scissor);
    const MeshShaderParameterBinding passBinding{0, resources.PassParameterSets[passId == MeshPassId{2} ? 1 : 0].Get(), {}};
    const MeshPassSubmitArgs submitArgs{
        .ParameterSets = std::span{&passBinding, 1},
        .InstanceStream = render::VertexBufferBinding{1, {resources.InstanceBuffer.Get(), 0, resources.InstanceBuffer->GetDesc().Size}}};
    auto& drawList = slot.GetPassDrawList(passId);
    return drawList.Submit(encoder, submitArgs);
}

void MeshDrawingFramePipeline::SubmitFrame(FrameSlot& slot, render::CommandBuffer& command) noexcept {
    auto& resources = *_frames[slot.GetIndex()];
    if (resources.CompletionValue >= std::numeric_limits<uint64_t>::max() - 1) RADRAY_ABORT("MeshDrawingFramePipeline: fence values exhausted");
    array submitted{&command};
    array fences{resources.CompletionFence.Get()};
    array values{resources.CompletionValue + 1};
    slot.GetQueue().Submit({.CmdBuffers = submitted, .SignalFences = fences, .SignalValues = values});
    resources.CompletionValue = values[0];
    _geometrySubmitted = true;
    _gpu.PendingVertexBytes.clear();
    _gpu.PendingIndexBytes.clear();
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
