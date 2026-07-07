#include <radray/runtime/render_framework/forward_pipeline.h>

#include <algorithm>
#include <cstring>

#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/render_queue.h>
#include <radray/runtime/render_framework/scene.h>

namespace radray {

namespace {

constexpr std::string_view kForwardPassTag = "UniversalForward";
constexpr std::string_view kPerObjectName = "gPerObject";
constexpr std::string_view kViewName = "gView";

}  // namespace

ForwardPipeline::ForwardPipeline(RenderSystem* renderSystem) noexcept
    : _device(renderSystem != nullptr && renderSystem->GetApplication() != nullptr
                  ? renderSystem->GetApplication()->GetDevice()
                  : nullptr) {
    if (_device != nullptr && renderSystem != nullptr) {
        const uint32_t flightCount =
            renderSystem->GetApplication() != nullptr && renderSystem->GetApplication()->GetGpuSystem() != nullptr
                ? renderSystem->GetApplication()->GetGpuSystem()->GetFlightDataCount()
                : 1;
        _executor = make_unique<MeshPassExecutor>(
            _device,
            renderSystem->GetShaderVariantCache(),
            renderSystem->GetGraphicsPipelineStateCache(),
            std::string{kPerObjectName},
            flightCount);
    }
}

ForwardPipeline::~ForwardPipeline() noexcept = default;

void ForwardPipeline::OnBuildCameraList(RenderPipelineContext& ctx, RenderCameraList& cameras) {
    cameras.Clear();
    if (ctx.App == nullptr) {
        return;
    }
    World* world = ctx.App->GetWorld();
    if (world == nullptr) {
        return;
    }
    Scene* scene = world->GetScene();
    if (scene == nullptr) {
        return;
    }

    // 最小实现: 取第一个相机, 渲染进第一个 target (主窗口)。
    CameraComponent* camera = nullptr;
    for (const unique_ptr<Actor>& actor : world->GetActors()) {
        if (actor == nullptr) {
            continue;
        }
        if (CameraComponent* found = actor->FindComponent<CameraComponent>(); found != nullptr) {
            camera = found;
            break;
        }
    }
    if (camera == nullptr) {
        return;
    }

    AppFrameTarget* target = ctx.Targets.empty() ? nullptr : &ctx.Targets.front().Target;
    cameras.Add(scene, camera, target);
}

ForwardPipeline::DepthTarget* ForwardPipeline::AcquireDepthTarget(uint32_t flight, uint32_t width, uint32_t height) {
    if (_device == nullptr) {
        return nullptr;
    }
    if (_depthTargets.size() <= flight) {
        _depthTargets.resize(static_cast<size_t>(flight) + 1);
    }
    DepthTarget& dt = _depthTargets[flight];
    if (dt.Texture != nullptr && dt.View != nullptr && dt.Width == width && dt.Height == height) {
        return &dt;
    }

    // 尺寸变化 (或首次): 重建。
    dt.View.reset();
    dt.Texture.reset();
    dt.State = render::TextureState::Undefined;

    render::TextureDescriptor texDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = kDepthFormat,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::DepthStencilWrite,
        .Hints = render::ResourceHint::None};
    auto texOpt = _device->CreateTexture(texDesc);
    if (!texOpt.HasValue()) {
        RADRAY_ERR_LOG("ForwardPipeline: failed to create depth texture");
        return nullptr;
    }
    dt.Texture = texOpt.Release();
    dt.Texture->SetDebugName("ForwardPipeline Depth");

    render::TextureViewDescriptor viewDesc{
        .Target = dt.Texture.get(),
        .Dim = render::TextureDimension::Dim2D,
        .Format = kDepthFormat,
        .Range = render::SubresourceRange{0, 1, 0, 1},
        .Usage = render::TextureViewUsage::DepthWrite};
    auto viewOpt = _device->CreateTextureView(viewDesc);
    if (!viewOpt.HasValue()) {
        RADRAY_ERR_LOG("ForwardPipeline: failed to create depth view");
        dt.Texture.reset();
        return nullptr;
    }
    dt.View = viewOpt.Release();
    dt.Width = width;
    dt.Height = height;
    dt.State = render::TextureState::Undefined;
    return &dt;
}

void ForwardPipeline::OnRenderCamera(RenderPipelineContext& ctx, const RenderCamera& camera) {
    if (_executor == nullptr || _device == nullptr || camera.RenderScene == nullptr || camera.ViewCamera == nullptr) {
        return;
    }
    if (!camera.Target.HasValue() || camera.Target.Get() == nullptr) {
        return;
    }
    const AppFrameTarget& target = *camera.Target.Get();
    if (target.BackBuffer == nullptr || target.BackBufferView == nullptr) {
        return;
    }

    const render::TextureDescriptor bbDesc = target.BackBuffer->GetDesc();
    const uint32_t width = bbDesc.Width;
    const uint32_t height = bbDesc.Height;
    if (width == 0 || height == 0) {
        return;
    }

    const uint32_t flight = ctx.Frame.FlightIndex();
    render::CommandBuffer* cmd = ctx.Frame.GetCommandBuffer();
    if (cmd == nullptr) {
        return;
    }

    Scene* scene = camera.RenderScene;
    CameraComponent* cameraComp = camera.ViewCamera;
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const Eigen::Matrix4f viewProj = cameraComp->ComputeViewProjMatrix(aspect);
    const Eigen::Vector3f eye = cameraComp->GetEyePosition();

    // ── per-view 常量 (ViewProj + 相机位置 + 点光源数组) ──
    ViewConstants view{};
    // Eigen 列主序, HLSL cbuffer float4x4 默认 column_major, 内存布局一致, 直接拷贝。
    std::memcpy(view.ViewProj, viewProj.data(), sizeof(float) * 16);
    view.CameraPosition[0] = eye.x();
    view.CameraPosition[1] = eye.y();
    view.CameraPosition[2] = eye.z();
    view.CameraPosition[3] = 1.0f;

    uint32_t lightCount = 0;
    for (const unique_ptr<LightSceneProxy>& light : scene->Lights()) {
        if (light == nullptr || light->GetLightType() != LightType::Point || !light->AffectsWorld()) {
            continue;
        }
        if (lightCount >= kMaxPointLights) {
            break;
        }
        const Eigen::Vector3f pos = light->GetOrigin();
        const Eigen::Vector3f color = light->GetColor();  // = lightColor * intensity
        const float range = light->GetRadius();
        PointLightGpu& gpu = view.PointLights[lightCount];
        gpu.Position[0] = pos.x();
        gpu.Position[1] = pos.y();
        gpu.Position[2] = pos.z();
        gpu.Position[3] = range;
        gpu.Intensity[0] = color.x();
        gpu.Intensity[1] = color.y();
        gpu.Intensity[2] = color.z();
        gpu.Intensity[3] = -1.0f;  // 无阴影
        ++lightCount;
    }
    view.LightCounts[0] = lightCount;

    _executor->BeginFrame(flight);  // 回收该 flight 上一轮的 table / arena
    _executor->SetViewConstants(
        kViewName,
        std::span<const byte>{reinterpret_cast<const byte*>(&view), sizeof(ViewConstants)});

    // ── 构建 DrawList (全收, 无视锥裁剪) ──
    // RTTI 禁用 (/GR-), 通过 PrimitiveSceneProxy 的虚 section 接口收集, 不用 dynamic_cast。
    // 按 material->IsTransparent() 分成两个 list: 不透明先画 (写深度), 透明后画 (读深度做遮挡)。
    DrawList opaqueList;
    DrawList transparentList;
    for (const unique_ptr<PrimitiveSceneProxy>& proxy : scene->Primitives()) {
        if (proxy == nullptr) {
            continue;
        }
        const uint32_t sectionCount = proxy->GetSectionCount();
        if (sectionCount == 0) {
            continue;
        }
        const Eigen::Vector3f center = proxy->GetLocalToWorld().block<3, 1>(0, 3);
        const float viewDistance = (center - eye).norm();
        for (uint32_t s = 0; s < sectionCount; ++s) {
            shared_ptr<const MaterialRenderSnapshot> snapshot = proxy->GetSectionSnapshot(s);
            if (snapshot == nullptr) {
                continue;
            }
            DrawList& target = snapshot->IsTransparent() ? transparentList : opaqueList;
            target.AddPrimitive(std::move(snapshot), proxy.get(), kForwardPassTag, s, viewDistance);
        }
    }
    opaqueList.SortOpaque();            // RenderQueue 升序 -> material 批处理 -> 近到远
    transparentList.SortTransparent();  // RenderQueue 升序 -> 远到近 (back-to-front)

    // ── 深度目标 + 单 render pass ──
    DepthTarget* depth = AcquireDepthTarget(flight, width, height);
    if (depth == nullptr || depth->View == nullptr) {
        return;
    }

    // 深度纹理转换到 DepthWrite 布局 (首次为 Undefined, 后续帧保持 DepthWrite)。
    if (depth->State != render::TextureState::DepthWrite) {
        render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
            .Target = depth->Texture.get(),
            .Before = depth->State,
            .After = render::TextureState::DepthWrite};
        cmd->ResourceBarrier(std::span{&barrier, 1});
        depth->State = render::TextureState::DepthWrite;
    }

    render::ColorAttachment colorAttachment{
        .Target = target.BackBufferView,
        .Load = render::LoadAction::Clear,
        .Store = render::StoreAction::Store,
        .ClearValue = render::ColorClearValue{{0.02f, 0.02f, 0.03f, 1.0f}}};
    render::DepthStencilAttachment depthAttachment{
        .Target = depth->View.get(),
        .DepthLoad = render::LoadAction::Clear,
        .DepthStore = render::StoreAction::Store,
        .StencilLoad = render::LoadAction::DontCare,
        .StencilStore = render::StoreAction::Discard,
        .ClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}}};
    render::RenderPassDescriptor passDesc{
        .ColorAttachments = std::span{&colorAttachment, 1},
        .DepthStencilAttachment = depthAttachment,
        .Name = "Forward Opaque"};
    auto encoderOpt = cmd->BeginRenderPass(passDesc);
    if (!encoderOpt.HasValue()) {
        RADRAY_ERR_LOG("ForwardPipeline: failed to begin forward render pass");
        return;
    }
    auto encoder = encoderOpt.Release();

    Viewport vp{
        .X = 0.0f,
        .Y = 0.0f,
        .Width = static_cast<float>(width),
        .Height = static_cast<float>(height),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f};
    if (_device->GetBackend() == render::RenderBackend::Vulkan) {
        vp.Y = static_cast<float>(height);
        vp.Height = -static_cast<float>(height);
    }
    encoder->SetViewport(vp);
    encoder->SetScissor(Rect{0, 0, width, height});

    // 同一 render pass、同一深度缓冲: 先不透明 (写深度), 再透明 (depth-write-off + alpha blend,
    // 复用不透明已写入的深度做遮挡测试; back-to-front 保证半透明叠加顺序正确)。
    _executor->Execute(encoder.get(), opaqueList);
    _executor->Execute(encoder.get(), transparentList);

    cmd->EndRenderPass(std::move(encoder));
}

}  // namespace radray
