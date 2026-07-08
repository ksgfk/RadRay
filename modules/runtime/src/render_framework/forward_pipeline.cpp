#include <radray/runtime/render_framework/forward_pipeline.h>

#include <algorithm>
#include <array>
#include <cstring>

#include <radray/logger.h>
#include <radray/render/sampler_cache.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/render_framework/material_render_snapshot.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/render_queue.h>
#include <radray/runtime/render_framework/scene.h>

namespace radray {

namespace {

constexpr std::string_view kForwardPassTag = "UniversalForward";
constexpr std::string_view kShadowPassTag = "ShadowCaster";
constexpr std::string_view kPerObjectName = "gPerObject";
constexpr std::string_view kViewName = "gView";
constexpr std::string_view kShadowViewName = "gShadowView";
constexpr std::string_view kShadowCubeName = "gShadowCube";
constexpr std::string_view kShadowSamplerName = "gShadowSampler";

// cube 6 面的 forward / up 方向, 面序严格匹配 point_shadow.hlsl 的 point_shadow_cube_face:
//   0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z。
// up 向量只影响面内朝向, 不影响 compareDepth (只依赖 forward), 取标准 cube 约定。
// (Eigen 向量非 constexpr 可构造, 用普通 float 数组存储。)
constexpr float kCubeForward[6][3] = {
    {1.0f, 0.0f, 0.0f},   // +X
    {-1.0f, 0.0f, 0.0f},  // -X
    {0.0f, 1.0f, 0.0f},   // +Y
    {0.0f, -1.0f, 0.0f},  // -Y
    {0.0f, 0.0f, 1.0f},   // +Z
    {0.0f, 0.0f, -1.0f},  // -Z
};
constexpr float kCubeUp[6][3] = {
    {0.0f, 1.0f, 0.0f},   // +X
    {0.0f, 1.0f, 0.0f},   // -X
    {0.0f, 0.0f, -1.0f},  // +Y (forward 与常规 up 平行, 换一个)
    {0.0f, 0.0f, 1.0f},   // -Y
    {0.0f, 1.0f, 0.0f},   // +Z
    {0.0f, 1.0f, 0.0f},   // -Z
};

// 生成一盏点光源某个 cube 面的 世界->裁剪 矩阵。90 度 FOV, 1:1 宽高比, near..radius。
Eigen::Matrix4f MakeCubeFaceViewProj(const Eigen::Vector3f& lightPos, uint32_t face, float radius) {
    const float nearZ = 0.05f;
    const float farZ = std::max(radius, nearZ + 0.01f);
    Eigen::Matrix4f proj = PerspectiveLH<float>(Radian(90.0f), 1.0f, nearZ, farZ);
    Eigen::Vector3f forward{kCubeForward[face][0], kCubeForward[face][1], kCubeForward[face][2]};
    Eigen::Vector3f up{kCubeUp[face][0], kCubeUp[face][1], kCubeUp[face][2]};
    Eigen::Matrix4f view = LookAtFrontLH<float>(lightPos, forward, up);
    return proj * view;
}

}  // namespace

ForwardPipeline::ForwardPipeline(RenderSystem* renderSystem) noexcept
    : _device(renderSystem != nullptr && renderSystem->GetApplication() != nullptr
                  ? renderSystem->GetApplication()->GetDevice()
                  : nullptr) {
    if (_device != nullptr && renderSystem != nullptr) {
        _samplerCache = renderSystem->GetSamplerCache();
        const uint32_t flightCount =
            renderSystem->GetApplication() != nullptr && renderSystem->GetApplication()->GetGpuSystem() != nullptr
                ? renderSystem->GetApplication()->GetGpuSystem()->GetFlightDataCount()
                : 1;
        _executor = make_unique<MeshPassExecutor>(
            _device,
            renderSystem->GetShaderVariantCache(),
            renderSystem->GetGraphicsPipelineStateCache(),
            renderSystem->GetSamplerCache(),
            std::string{kPerObjectName},
            flightCount);
        _shadowExecutor = make_unique<MeshPassExecutor>(
            _device,
            renderSystem->GetShaderVariantCache(),
            renderSystem->GetGraphicsPipelineStateCache(),
            renderSystem->GetSamplerCache(),
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

ForwardPipeline::ShadowCube* ForwardPipeline::AcquireShadowCube(uint32_t flight, uint32_t size) {
    if (_device == nullptr) {
        return nullptr;
    }
    if (_shadowCubes.size() <= flight) {
        _shadowCubes.resize(static_cast<size_t>(flight) + 1);
    }
    ShadowCube& sc = _shadowCubes[flight];
    if (sc.Texture != nullptr && sc.Srv != nullptr && sc.Size == size) {
        return &sc;
    }

    // 首次 (或尺寸变化): 重建 cube 深度纹理 + cube SRV + 每面 DSV。
    for (auto& dsv : sc.FaceDsv) {
        dsv.reset();
    }
    sc.Srv.reset();
    sc.Texture.reset();
    sc.State = render::TextureState::Undefined;

    render::TextureDescriptor texDesc{
        .Dim = render::TextureDimension::Cube,
        .Width = size,
        .Height = size,
        .DepthOrArraySize = kCubeFaceCount,  // cube = 6 层 2D array
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = kShadowFormat,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::DepthStencilWrite | render::TextureUse::Resource,
        .Hints = render::ResourceHint::None};
    auto texOpt = _device->CreateTexture(texDesc);
    if (!texOpt.HasValue()) {
        RADRAY_ERR_LOG("ForwardPipeline: failed to create shadow cube texture");
        return nullptr;
    }
    sc.Texture = texOpt.Release();
    sc.Texture->SetDebugName("ForwardPipeline ShadowCube");

    // cube SRV (采样用, 覆盖全 6 面)。用 Resource usage 走 SRV 路径 (深度格式会被
    // 映射成 R32_FLOAT 采样); DepthRead 会误入 DSV 路径, 不支持 Cube 维度。
    // SubresourceRange 字段序 = {BaseArrayLayer, ArrayLayerCount, BaseMipLevel, MipLevelCount}:
    // 6 个 array 层 (cube 面), 1 个 mip。
    render::TextureViewDescriptor srvDesc{
        .Target = sc.Texture.get(),
        .Dim = render::TextureDimension::Cube,
        .Format = kShadowFormat,
        .Range = render::SubresourceRange{0, kCubeFaceCount, 0, 1},
        .Usage = render::TextureViewUsage::Resource};
    auto srvOpt = _device->CreateTextureView(srvDesc);
    if (!srvOpt.HasValue()) {
        RADRAY_ERR_LOG("ForwardPipeline: failed to create shadow cube SRV");
        sc.Texture.reset();
        return nullptr;
    }
    sc.Srv = srvOpt.Release();

    // 每面一个 DSV (渲染用, 走 2DArray slice)。
    for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
        render::TextureViewDescriptor dsvDesc{
            .Target = sc.Texture.get(),
            .Dim = render::TextureDimension::Dim2DArray,
            .Format = kShadowFormat,
            .Range = render::SubresourceRange{face, 1, 0, 1},
            .Usage = render::TextureViewUsage::DepthWrite};
        auto dsvOpt = _device->CreateTextureView(dsvDesc);
        if (!dsvOpt.HasValue()) {
            RADRAY_ERR_LOG("ForwardPipeline: failed to create shadow cube face DSV {}", face);
            sc.Srv.reset();
            sc.Texture.reset();
            return nullptr;
        }
        sc.FaceDsv[face] = dsvOpt.Release();
    }

    sc.Size = size;
    sc.State = render::TextureState::Undefined;
    return &sc;
}

bool ForwardPipeline::RenderPointShadow(
    RenderPipelineContext& ctx,
    Scene* scene,
    const LightSceneProxy& light,
    uint32_t flight,
    PointShadowGpu& outShadow,
    ShadowCube*& outCube) {
    render::CommandBuffer* cmd = ctx.Frame.GetCommandBuffer();
    if (cmd == nullptr || _shadowExecutor == nullptr) {
        return false;
    }
    ShadowCube* cube = AcquireShadowCube(flight, kShadowCubeSize);
    if (cube == nullptr) {
        return false;
    }

    const Eigen::Vector3f lightPos = light.GetOrigin();
    const float radius = light.GetRadius();

    // 6 面矩阵 (供 depth pass 逐面 + 采样 shader 重投影)。
    std::array<Eigen::Matrix4f, kCubeFaceCount> faceVp;
    for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
        faceVp[face] = MakeCubeFaceViewProj(lightPos, face, radius);
        // Eigen 列主序, HLSL float4x4 默认 column_major, 内存一致, 直接拷贝。
        std::memcpy(outShadow.ViewProj[face], faceVp[face].data(), sizeof(float) * 16);
    }
    outShadow.LightPosInvRadius[0] = lightPos.x();
    outShadow.LightPosInvRadius[1] = lightPos.y();
    outShadow.LightPosInvRadius[2] = lightPos.z();
    outShadow.LightPosInvRadius[3] = radius > 0.0f ? 1.0f / radius : 0.0f;
    outShadow.Params[0] = 0.002f;                             // depthBias (裁剪空间 z)
    outShadow.Params[1] = radius * 0.02f;                     // normalBias (世界空间)
    outShadow.Params[2] = 1.0f / static_cast<float>(cube->Size);  // invResolution
    outShadow.Params[3] = 1.0f;                               // enable

    // 构建 shadow caster DrawList (ShadowCaster tag; shader 无该 pass 的 primitive 自动跳过)。
    DrawList casterList;
    for (const unique_ptr<PrimitiveSceneProxy>& proxy : scene->Primitives()) {
        if (proxy == nullptr) {
            continue;
        }
        const uint32_t sectionCount = proxy->GetSectionCount();
        for (uint32_t s = 0; s < sectionCount; ++s) {
            shared_ptr<const MaterialRenderSnapshot> snapshot = proxy->GetSectionSnapshot(s);
            if (snapshot == nullptr) {
                continue;
            }
            casterList.AddPrimitive(std::move(snapshot), proxy.get(), kShadowPassTag, s, 0.0f);
        }
    }
    if (casterList.Empty()) {
        // 没有投影者: 视为无阴影 (但仍需把 cube 清成远深度并转采样布局, 避免采样到脏数据)。
        outShadow.Params[3] = 0.0f;
    }

    // cube 转 DepthWrite 布局 (整资源 barrier)。
    if (cube->State != render::TextureState::DepthWrite) {
        render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
            .Target = cube->Texture.get(),
            .Before = cube->State,
            .After = render::TextureState::DepthWrite};
        cmd->ResourceBarrier(std::span{&barrier, 1});
        cube->State = render::TextureState::DepthWrite;
    }

    _shadowExecutor->BeginFrame(flight);
    _shadowExecutor->ClearGlobals();

    // 逐面渲染: 每面一个 depth-only render pass, 写入对应 slice。
    for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
        ShadowViewConstants sv{};
        std::memcpy(sv.ViewProj, faceVp[face].data(), sizeof(float) * 16);
        _shadowExecutor->SetViewConstants(
            kShadowViewName,
            std::span<const byte>{reinterpret_cast<const byte*>(&sv), sizeof(ShadowViewConstants)});

        render::DepthStencilAttachment depthAttachment{
            .Target = cube->FaceDsv[face].get(),
            .DepthLoad = render::LoadAction::Clear,
            .DepthStore = render::StoreAction::Store,
            .StencilLoad = render::LoadAction::DontCare,
            .StencilStore = render::StoreAction::Discard,
            .ClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}}};
        render::RenderPassDescriptor passDesc{
            .ColorAttachments = {},
            .DepthStencilAttachment = depthAttachment,
            .Name = "Point Shadow Face"};
        auto encoderOpt = cmd->BeginRenderPass(passDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("ForwardPipeline: failed to begin shadow face pass {}", face);
            return false;
        }
        auto encoder = encoderOpt.Release();

        Viewport vp{
            .X = 0.0f,
            .Y = 0.0f,
            .Width = static_cast<float>(cube->Size),
            .Height = static_cast<float>(cube->Size),
            .MinDepth = 0.0f,
            .MaxDepth = 1.0f};
        if (_device->GetBackend() == render::RenderBackend::Vulkan) {
            vp.Y = static_cast<float>(cube->Size);
            vp.Height = -static_cast<float>(cube->Size);
        }
        encoder->SetViewport(vp);
        encoder->SetScissor(Rect{0, 0, cube->Size, cube->Size});

        _shadowExecutor->Execute(encoder.get(), casterList);
        cmd->EndRenderPass(std::move(encoder));
    }

    // cube 转采样布局 (ShaderRead)。作为常规 SRV (R32_FLOAT) 在 pixel shader 里比较采样,
    // 故用 ShaderRead (映射到 PIXEL_SHADER_RESOURCE), 而非 DepthRead (DEPTH_READ, 不可采样)。
    render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
        .Target = cube->Texture.get(),
        .Before = render::TextureState::DepthWrite,
        .After = render::TextureState::ShaderRead};
    cmd->ResourceBarrier(std::span{&barrier, 1});
    cube->State = render::TextureState::ShaderRead;

    outCube = cube;
    return true;
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

    // 选取第一盏投影阴影的点光源 (记下它在灯光数组里的序号)。
    int32_t shadowLightIndex = -1;
    uint32_t lightCount = 0;
    const LightSceneProxy* shadowLight = nullptr;
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
        gpu.Intensity[3] = 0.0f;
        if (shadowLight == nullptr && light->CastShadow()) {
            shadowLight = light.get();
            shadowLightIndex = static_cast<int32_t>(lightCount);
        }
        ++lightCount;
    }
    view.LightCounts[0] = lightCount;
    view.LightCounts[1] = 0;  // 默认无阴影, 成功渲染阴影后置为 index+1 (0 表示无)

    // ── shadow caster pass (在 forward pass 之前, 同一 command buffer) ──
    ShadowCube* shadowCube = nullptr;
    if (shadowLight != nullptr) {
        if (RenderPointShadow(ctx, scene, *shadowLight, flight, view.PointShadow, shadowCube)) {
            view.LightCounts[1] = static_cast<uint32_t>(shadowLightIndex) + 1u;
        }
    }

    _executor->BeginFrame(flight);  // 回收该 flight 上一轮的 table / arena
    _executor->ClearGlobals();
    _executor->SetViewConstants(
        kViewName,
        std::span<const byte>{reinterpret_cast<const byte*>(&view), sizeof(ViewConstants)});
    if (shadowCube != nullptr && shadowCube->Srv != nullptr) {
        _executor->SetGlobalTexture(kShadowCubeName, shadowCube->Srv.get());
    }

    // shadow comparison sampler (LessEqual): 标准深度, 近值小, 通过测试 (可见) 时返回 1。
    if (_samplerCache != nullptr) {
        render::SamplerDescriptor sd{};
        sd.AddressS = render::AddressMode::ClampToEdge;
        sd.AddressT = render::AddressMode::ClampToEdge;
        sd.AddressR = render::AddressMode::ClampToEdge;
        sd.MinFilter = render::FilterMode::Linear;
        sd.MagFilter = render::FilterMode::Linear;
        sd.MipmapFilter = render::FilterMode::Nearest;
        sd.LodMin = 0.0f;
        sd.LodMax = 0.0f;
        sd.Compare = render::CompareFunction::LessEqual;
        sd.AnisotropyClamp = 0;
        auto samplerOpt = _samplerCache->GetOrCreate(sd);
        if (samplerOpt.HasValue()) {
            _executor->SetGlobalSampler(kShadowSamplerName, samplerOpt.Get());
        }
    }

    // ── 构建 DrawList (全收, 无视锥裁剪) ──
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
            DrawList& drawList = snapshot->IsTransparent() ? transparentList : opaqueList;
            drawList.AddPrimitive(std::move(snapshot), proxy.get(), kForwardPassTag, s, viewDistance);
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
