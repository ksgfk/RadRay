#include <radray/runtime/application.h>
#include <radray/runtime/imgui_system.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gltf_asset.h>
#include <radray/runtime/shader_variant.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/camera_control_component.h>
#include <radray/runtime/components/light_component.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/render/scene.h>
#include <radray/runtime/render/scene_view.h>
#include <radray/runtime/render/shader.h>
#include <radray/runtime/render/shader_variant_cache.h>
#include <radray/runtime/render/render_context.h>
#include <radray/runtime/render/draw_objects_pass.h>
#include <radray/runtime/render/render_resource_pool.h>
#include <radray/runtime/render/cull.h>
#include <radray/runtime/render/material.h>
#include <radray/runtime/render/culling_results.h>
#include <radray/render/common.h>
#include <radray/basic_math.h>
#include <radray/logger.h>
#include <radray/types.h>

#include "viewer_lighting.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#ifndef RADRAY_GLTF_VIEWER_ASSET_DIR
#define RADRAY_GLTF_VIEWER_ASSET_DIR "."
#endif

using namespace radray;

namespace {

constexpr std::string_view GltfViewerDepthName = "GltfViewerDepth";
constexpr std::string_view ShadowMapDepthName = "ShadowMapDepth";
constexpr std::string_view AdditionalShadowMapDepthName = "AdditionalShadowMapDepth";
constexpr std::string_view ShadowPreviewAtlasName = "ShadowMapPreviewAtlas";
constexpr uint32_t MaxShadowCascades = gltfviewer::ShadowCascadeData::MaxCascades; // Must match SceneLightBuffer/Shader cascade constants.
constexpr uint32_t ShadowMapResolution = 2048;
constexpr uint32_t AdditionalShadowMapResolution = 1024;
constexpr uint32_t MaxAdditionalShadowSlices = gltfviewer::AdditionalShadowData::MaxSlices; // Must match SceneLightBuffer/Shader slice constants.
constexpr uint32_t ShadowPreviewCellSize = 256;
constexpr uint32_t ShadowPreviewAtlasSize = ShadowPreviewCellSize * 2;
constexpr float DefaultShadowMaxDistance = 50.0f;
constexpr float DefaultShadowSplitLambda = 0.85f;
constexpr int32_t ShadowRasterDepthBias = 0;
constexpr float ShadowRasterSlopeBias = 0.0f;
static_assert(MaxShadowCascades == gltfviewer::SceneLightBuffer::MaxShadowCascadesGpu);
static_assert(MaxShadowCascades == 4);
static_assert(MaxAdditionalShadowSlices == gltfviewer::SceneLightBuffer::MaxAdditionalShadowSlicesGpu);

/// per-object 常量,匹配 gltf_viewer.hlsl 的 SceneConstants(b0, space0, push constant)。
/// float4x4 MVP; float4x4 Model; float4 CameraPosition; uint4 Debug —— 列主序,与 Eigen 一致。
struct SceneConstants {
    float MVP[16];
    float Model[16];
    float CameraPosition[4];
    uint32_t Debug[4];
};
static_assert(sizeof(SceneConstants) == 160);

void WriteMatrix(float (&dst)[16], const Eigen::Matrix4f& m) noexcept {
    // Eigen 默认列主序,HLSL ConstantBuffer 也按列主序读 → 直接 memcpy。
    std::memcpy(dst, m.data(), sizeof(float) * 16);
}

float ShadowSoftKernelRadius(gltfviewer::ShadowSoftMode mode) noexcept {
    switch (mode) {
    case gltfviewer::ShadowSoftMode::Low:
        return 1.5f;
    case gltfviewer::ShadowSoftMode::Medium:
        return 2.5f;
    default:
        return 1.0f;
    }
}

constexpr std::string_view ShadowPreviewShaderSource = R"(
#if defined(VULKAN)
#define VK_BINDING(b, s) [[vk::binding(b, s)]]
#define VK_IMAGE_FORMAT(fmt) [[vk::image_format(#fmt)]]
#else
#define VK_BINDING(b, s)
#define VK_IMAGE_FORMAT(fmt)
#endif

static const uint ShadowMapSize = 2048;
static const uint PreviewCellSize = 256;
static const uint PreviewAtlasSize = PreviewCellSize * 2;

VK_BINDING(0, 0) Texture2DArray<float> gShadowMap : register(t0, space0);
VK_BINDING(1, 0) VK_IMAGE_FORMAT(rgba8) RWTexture2D<float4> gPreviewAtlas : register(u0, space0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (dispatchThreadId.x >= PreviewAtlasSize || dispatchThreadId.y >= PreviewAtlasSize) {
        return;
    }

    const uint cascade = (dispatchThreadId.x / PreviewCellSize) + (dispatchThreadId.y / PreviewCellSize) * 2;
    const uint2 local = uint2(dispatchThreadId.x % PreviewCellSize, dispatchThreadId.y % PreviewCellSize);
    const uint2 src = min((local * ShadowMapSize) / PreviewCellSize, uint2(ShadowMapSize - 1, ShadowMapSize - 1));
    const float depth = gShadowMap.Load(int4(src.x, src.y, cascade, 0));
    const float value = saturate(1.0 - depth);
    gPreviewAtlas[dispatchThreadId.xy] = float4(value, value, value, 1.0);
}
)";

// ── 瞬态纹理 acquire 助手(吃 pool/flight/device/cmd,不再吃 RenderContext)──

render::TextureView* AcquireDepthView(
    srp::RenderResourcePool& pool,
    uint32_t flight,
    render::Device& device,
    render::CommandBuffer* cmd,
    uint32_t width,
    uint32_t height,
    render::TextureFormat depthFormat) {
    render::TextureDescriptor depthDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = depthFormat,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::DepthStencilWrite,
        .Hints = render::ResourceHint::None};
    if (pool.Acquire(GltfViewerDepthName, flight, depthDesc, device) == nullptr) {
        return nullptr;
    }
    pool.Transition(GltfViewerDepthName, flight, render::TextureState::DepthWrite, *cmd);

    render::TextureViewDescriptor depthViewDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Format = depthFormat,
        .Range = render::SubresourceRange::AllSub(),
        .Usage = render::TextureViewUsage::DepthWrite};
    return pool.GetView(GltfViewerDepthName, flight, depthViewDesc, device);
}

render::TextureDescriptor MakeShadowDepthDescriptor() {
    return render::TextureDescriptor{
        .Dim = render::TextureDimension::Dim2DArray,
        .Width = ShadowMapResolution,
        .Height = ShadowMapResolution,
        .DepthOrArraySize = MaxShadowCascades,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = render::TextureFormat::D32_FLOAT,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::DepthStencilWrite | render::TextureUse::Resource,
        .Hints = render::ResourceHint::None};
}

render::TextureView* AcquireShadowSliceDepthView(
    srp::RenderResourcePool& pool,
    uint32_t flight,
    render::Device& device,
    render::CommandBuffer* cmd,
    uint32_t cascade) {
    const render::TextureDescriptor shadowDesc = MakeShadowDepthDescriptor();
    if (pool.Acquire(ShadowMapDepthName, flight, shadowDesc, device) == nullptr) {
        return nullptr;
    }
    pool.Transition(ShadowMapDepthName, flight, render::TextureState::DepthWrite, *cmd);

    render::TextureViewDescriptor depthViewDesc{
        .Dim = render::TextureDimension::Dim2DArray,
        .Format = render::TextureFormat::D32_FLOAT,
        .Range = render::SubresourceRange{cascade, 1, 0, 1},
        .Usage = render::TextureViewUsage::DepthWrite};
    return pool.GetView(ShadowMapDepthName, flight, depthViewDesc, device);
}

render::TextureView* AcquireShadowArrayResourceView(
    srp::RenderResourcePool& pool,
    uint32_t flight,
    render::Device& device) {
    const render::TextureDescriptor shadowDesc = MakeShadowDepthDescriptor();
    if (pool.Acquire(ShadowMapDepthName, flight, shadowDesc, device) == nullptr) {
        return nullptr;
    }

    render::TextureViewDescriptor viewDesc{
        .Dim = render::TextureDimension::Dim2DArray,
        .Format = render::TextureFormat::D32_FLOAT,
        .Range = render::SubresourceRange::AllSub(),
        .Usage = render::TextureViewUsage::Resource};
    return pool.GetView(ShadowMapDepthName, flight, viewDesc, device);
}

render::TextureDescriptor MakeAdditionalShadowDepthDescriptor() {
    return render::TextureDescriptor{
        .Dim = render::TextureDimension::Dim2DArray,
        .Width = AdditionalShadowMapResolution,
        .Height = AdditionalShadowMapResolution,
        .DepthOrArraySize = MaxAdditionalShadowSlices,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = render::TextureFormat::D32_FLOAT,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::DepthStencilWrite | render::TextureUse::Resource,
        .Hints = render::ResourceHint::None};
}

render::TextureView* AcquireAdditionalShadowSliceDepthView(
    srp::RenderResourcePool& pool,
    uint32_t flight,
    render::Device& device,
    render::CommandBuffer* cmd,
    uint32_t slice) {
    const render::TextureDescriptor shadowDesc = MakeAdditionalShadowDepthDescriptor();
    if (pool.Acquire(AdditionalShadowMapDepthName, flight, shadowDesc, device) == nullptr) {
        return nullptr;
    }
    pool.Transition(AdditionalShadowMapDepthName, flight, render::TextureState::DepthWrite, *cmd);

    render::TextureViewDescriptor depthViewDesc{
        .Dim = render::TextureDimension::Dim2DArray,
        .Format = render::TextureFormat::D32_FLOAT,
        .Range = render::SubresourceRange{slice, 1, 0, 1},
        .Usage = render::TextureViewUsage::DepthWrite};
    return pool.GetView(AdditionalShadowMapDepthName, flight, depthViewDesc, device);
}

render::TextureView* AcquireAdditionalShadowArrayResourceView(
    srp::RenderResourcePool& pool,
    uint32_t flight,
    render::Device& device) {
    const render::TextureDescriptor shadowDesc = MakeAdditionalShadowDepthDescriptor();
    if (pool.Acquire(AdditionalShadowMapDepthName, flight, shadowDesc, device) == nullptr) {
        return nullptr;
    }

    render::TextureViewDescriptor viewDesc{
        .Dim = render::TextureDimension::Dim2DArray,
        .Format = render::TextureFormat::D32_FLOAT,
        .Range = render::SubresourceRange::AllSub(),
        .Usage = render::TextureViewUsage::Resource};
    return pool.GetView(AdditionalShadowMapDepthName, flight, viewDesc, device);
}

render::TextureDescriptor MakeShadowPreviewAtlasDescriptor() {
    return render::TextureDescriptor{
        .Dim = render::TextureDimension::Dim2D,
        .Width = ShadowPreviewAtlasSize,
        .Height = ShadowPreviewAtlasSize,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = render::TextureFormat::RGBA8_UNORM,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::Resource | render::TextureUse::UnorderedAccess,
        .Hints = render::ResourceHint::None};
}

render::TextureView* AcquireShadowPreviewAtlasResourceView(
    srp::RenderResourcePool& pool,
    uint32_t flight,
    render::Device& device) {
    const render::TextureDescriptor desc = MakeShadowPreviewAtlasDescriptor();
    if (pool.Acquire(ShadowPreviewAtlasName, flight, desc, device) == nullptr) {
        return nullptr;
    }

    render::TextureViewDescriptor viewDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Format = desc.Format,
        .Range = render::SubresourceRange::AllSub(),
        .Usage = render::TextureViewUsage::Resource};
    return pool.GetView(ShadowPreviewAtlasName, flight, viewDesc, device);
}

render::TextureView* AcquireShadowPreviewAtlasUnorderedAccessView(
    srp::RenderResourcePool& pool,
    uint32_t flight,
    render::Device& device) {
    const render::TextureDescriptor desc = MakeShadowPreviewAtlasDescriptor();
    if (pool.Acquire(ShadowPreviewAtlasName, flight, desc, device) == nullptr) {
        return nullptr;
    }

    render::TextureViewDescriptor viewDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Format = desc.Format,
        .Range = render::SubresourceRange::AllSub(),
        .Usage = render::TextureViewUsage::UnorderedAccess};
    return pool.GetView(ShadowPreviewAtlasName, flight, viewDesc, device);
}

void SetViewportAndScissor(render::GraphicsCommandEncoder* encoder, render::Device& device, uint32_t width, uint32_t height) {
    Viewport vp{
        .X = 0.0f,
        .Y = 0.0f,
        .Width = static_cast<float>(width),
        .Height = static_cast<float>(height),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f};
    if (device.GetBackend() == render::RenderBackend::Vulkan) {
        vp.Y = static_cast<float>(height);
        vp.Height = -static_cast<float>(height);
    }
    encoder->SetViewport(vp);
    encoder->SetScissor(Rect{0, 0, width, height});
}

void SetShadowViewportAndScissor(render::GraphicsCommandEncoder* encoder, render::Device& device) {
    SetViewportAndScissor(encoder, device, ShadowMapResolution, ShadowMapResolution);
}

void SetResolutionViewportAndScissor(render::GraphicsCommandEncoder* encoder, render::Device& device, uint32_t resolution) {
    SetViewportAndScissor(encoder, device, resolution, resolution);
}

}  // namespace

// 这个 example 是一个"纯游戏应用":只重写窄接口 OnInit / OnUpdate / OnRenderView / OnShutdown。
//
// 渲染走新的 srp 框架。CSM 4 级联方向光阴影、附加(聚光/点光)阴影图集、debug 预览 compute、
// alpha test、tone mapping 等 viewer 特性,作为 gltf_viewer 私有代码运行在最小的 srp 框架之上
// (灯光/阴影 god-struct 迁出 runtime,落在 viewer_lighting + 本文件)。
//
// 各逻辑 pass 因目标 RT 不同(4 阴影 slice / N 附加 slice / pre-z / base / transparent),
// 无法共用 RenderPipeline.RenderSingleCamera 的单 encoder;OnRenderView 逐 pass 手动
// BeginRenderPass(其附件) → rc.SetEncoder → 配 DrawObjectsPass → pass.Execute → EndRenderPass。
class GltfViewerApp : public Application {
public:
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;
    static constexpr render::TextureFormat DepthFormat = render::TextureFormat::D32_FLOAT;

    void SetInitialLoadPath(std::filesystem::path path) {
        _initialLoadPath = std::move(path);
    }

    void OnInit() override {
        // ── srp::Shader:三个 LightMode pass,源文件均为 gltf_viewer.hlsl(保持不变)──
        std::filesystem::path shaderPath = std::filesystem::path{RADRAY_GLTF_VIEWER_ASSET_DIR} / "gltf_viewer.hlsl";
        const string shaderPathStr = shaderPath.string();
        _viewerShader = make_unique<srp::Shader>(srp::ShaderId{0x6171F}, "gltf_viewer");
        {
            srp::ShaderPassSource pass{};
            pass.ShaderPath = shaderPathStr;
            pass.ShaderName = "gltf_viewer";
            pass.VsEntry = "VSMain";
            pass.PsEntry = "PSMain";
            pass.Tags.Tags.emplace_back("LightMode", "UniversalForward");
            _viewerShader->AddPass(std::move(pass));
        }
        {
            srp::ShaderPassSource pass{};
            pass.ShaderPath = shaderPathStr;
            pass.ShaderName = "gltf_viewer";
            pass.VsEntry = "VSMain";
            pass.PsEntry = "PSDepthOnlyMain";
            pass.Tags.Tags.emplace_back("LightMode", "ShadowCaster");
            _viewerShader->AddPass(std::move(pass));
        }
        {
            srp::ShaderPassSource pass{};
            pass.ShaderPath = shaderPathStr;
            pass.ShaderName = "gltf_viewer";
            pass.VsEntry = "VSMain";
            pass.PsEntry = "PSDepthOnlyMain";
            pass.Tags.Tags.emplace_back("LightMode", "DepthOnly");
            _viewerShader->AddPass(std::move(pass));
        }

        _variantCache = make_unique<srp::ShaderVariantCache>(GetGpuSystem());

        SpawnCamera();
        SpawnDefaultLights();
        ConfigureCameraControl();
        SetCameraFrame(Eigen::Vector3f::Zero(), 4.0f);

        _loadPath.resize(1024);
        const std::filesystem::path defaultPath = _initialLoadPath.empty()
            ? (std::filesystem::path{RADRAY_GLTF_VIEWER_ASSET_DIR} / "model.gltf")
            : _initialLoadPath;
        const string initial = defaultPath.string();
        std::memcpy(_loadPath.data(), initial.c_str(), std::min(initial.size(), _loadPath.size() - 1));
        if (!_initialLoadPath.empty()) {
            _pendingLoadPath = _initialLoadPath;
            _pendingLoad = true;
        }
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        if (_pendingLoad) {
            LoadSceneFromPath(_pendingLoadPath);
            _pendingLoad = false;
        }
        TryExportReadyAsset();
        if (ImGuiSystem* imgui = GetSubsystem<ImGuiSystem>()) {
            if (imgui->Begin(ctx)) {
                DrawUi(ctx);
                imgui->End();
            }
        }
    }

    bool OnRenderView(AppFrameContext& ctx, const AppFrameTarget& target) override {
        if (!target.Window->IsMainWindow()) {
            return false;
        }
        render::TextureDescriptor bbDesc = target.BackBuffer->GetDesc();
        const uint32_t width = bbDesc.Width;
        const uint32_t height = bbDesc.Height;
        if (width == 0 || height == 0) {
            return false;
        }

        const uint32_t flight = ctx.FlightIndex();
        render::CommandBuffer* cmd = ctx.GetCommandBuffer();
        render::Device* device = GetDevice();
        srp::Scene* scene = GetWorld()->GetScene();
        if (scene == nullptr || _cameraComp == nullptr || cmd == nullptr || device == nullptr) {
            return false;
        }

        srp::SceneView view{};
        _cameraComp->FillSceneView(view, width, height);
        srp::CullingResults cull = srp::CullAll(*scene, view);

        // ── 准备每帧阴影数据(方向光 CSM + 附加光图集)──
        ComputeDirectionalCascades(view, _shadow);
        _lastShadow = _shadow;
        gltfviewer::BuildAdditionalShadows(*scene, AdditionalShadowMapResolution, _shadowSoftMode, _additionalShadow);
        _lastAdditionalShadowSlices = _additionalShadow.SliceCount;
        _lastAdditionalShadowLights = static_cast<uint32_t>(_additionalShadow.Lights.size());

        render::TextureView* shadowSrv = AcquireShadowArrayResourceView(_resourcePool, flight, *device);
        render::TextureView* additionalShadowSrv = AcquireAdditionalShadowArrayResourceView(_resourcePool, flight, *device);

        // ── space0(per-view):从 UniversalForward 变体取 rootSig,构建灯光/阴影 descriptor set ──
        const srp::ShaderVariant* fwd = _variantCache->Get(*_viewerShader, "UniversalForward", {});
        render::RootSignature* rootSig = fwd != nullptr ? fwd->RootSig : nullptr;
        render::DescriptorSet* lightSet = nullptr;
        if (rootSig != nullptr) {
            lightSet = _lightBuffer.Update(
                *device,
                rootSig,
                render::DescriptorSetIndex{0},
                flight,
                *scene,
                _shadow,
                shadowSrv,
                _additionalShadow,
                additionalShadowSrv);
        }

        srp::RenderContext rc{GetGpuSystem(), _variantCache.get()};

        // 逐 pass 通用流程:绑 encoder → 执行 → 解绑。BeginRenderPass / EndRenderPass 由各调用点管理。
        auto runPass = [&](render::GraphicsCommandEncoder* enc, srp::DrawObjectsPass& pass) {
            rc.SetEncoder(enc);
            pass.Execute(rc, view, cull);
            rc.SetEncoder(nullptr);
        };

        // ============ 1. 方向光级联阴影(per-cascade)============
        {
            const uint32_t cascadeCount = _shadow.Enabled
                ? std::min<uint32_t>(_shadow.CascadeCount, MaxShadowCascades)
                : 0u;
            const uint32_t passes = std::max<uint32_t>(cascadeCount, 1u);
            for (uint32_t i = 0; i < passes; ++i) {
                render::TextureView* depthView = AcquireShadowSliceDepthView(_resourcePool, flight, *device, cmd, i);
                if (depthView == nullptr) {
                    return false;
                }
                render::DepthStencilAttachment depthAttachment{
                    .Target = depthView,
                    .DepthLoad = render::LoadAction::Clear,
                    .DepthStore = render::StoreAction::Store,
                    .StencilLoad = render::LoadAction::DontCare,
                    .StencilStore = render::StoreAction::Discard,
                    .ClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}}};
                render::RenderPassDescriptor passDesc{
                    .ColorAttachments = {},
                    .DepthStencilAttachment = depthAttachment,
                    .Name = "glTF Viewer Shadow Caster"};
                auto encoderOpt = cmd->BeginRenderPass(passDesc);
                if (!encoderOpt.HasValue()) {
                    RADRAY_ERR_LOG("failed to begin glTF viewer shadow caster pass");
                    return false;
                }
                auto encoder = encoderOpt.Release();
                SetShadowViewportAndScissor(encoder.get(), *device);

                if (cascadeCount > 0) {
                    const gltfviewer::ShadowCascade& cascade = _shadow.Cascades[i];
                    const Eigen::Vector3f lightDir = _shadow.LightDirectionForBias;
                    srp::DrawObjectsPass::Desc desc{};
                    desc.Event = srp::RenderPassEvent::BeforeRenderingShadows;
                    desc.ShaderTags = {"ShadowCaster"};
                    desc.Filtering.QueueRange = srp::RenderQueueRange::Opaque();
                    desc.SortFlags = srp::SortingCriteria::FrontToBack;
                    desc.PassKeywords.Add(shader_define::ShadowCaster);
                    desc.RenderState = srp::MeshPassRenderState::Shadow(ShadowRasterDepthBias, ShadowRasterSlopeBias);
                    desc.RTFormats.DepthFormat = render::TextureFormat::D32_FLOAT;
                    desc.PerObjectParamName = "gScene";
                    desc.PerObjectByteSize = sizeof(SceneConstants);
                    desc.PerObjectFn = [cascade, lightDir](std::span<byte> dst, const srp::Renderer& renderer, const srp::SceneView&) {
                        if (dst.size() < sizeof(SceneConstants)) {
                            return;
                        }
                        SceneConstants sc{};
                        WriteMatrix(sc.MVP, cascade.View.ViewProjMatrix);
                        WriteMatrix(sc.Model, renderer.WorldMatrix());
                        sc.CameraPosition[0] = lightDir.x();
                        sc.CameraPosition[1] = lightDir.y();
                        sc.CameraPosition[2] = lightDir.z();
                        sc.CameraPosition[3] = cascade.DepthBias;
                        std::memcpy(&sc.Debug[1], &cascade.NormalBias, sizeof(float));
                        std::memcpy(dst.data(), &sc, sizeof(SceneConstants));
                    };
                    srp::DrawObjectsPass pass{std::move(desc)};
                    runPass(encoder.get(), pass);
                }

                cmd->EndRenderPass(std::move(encoder));
            }
            _resourcePool.Transition(ShadowMapDepthName, flight, render::TextureState::ShaderRead, *cmd);
        }

        // ============ 2. 附加光(聚光/点光)阴影图集(per-slice)============
        {
            const uint32_t sliceCount = _additionalShadow.Enabled
                ? std::min<uint32_t>(_additionalShadow.SliceCount, MaxAdditionalShadowSlices)
                : 0u;
            const uint32_t passes = std::max<uint32_t>(sliceCount, 1u);
            for (uint32_t i = 0; i < passes; ++i) {
                render::TextureView* depthView = AcquireAdditionalShadowSliceDepthView(_resourcePool, flight, *device, cmd, i);
                if (depthView == nullptr) {
                    return false;
                }
                render::DepthStencilAttachment depthAttachment{
                    .Target = depthView,
                    .DepthLoad = render::LoadAction::Clear,
                    .DepthStore = render::StoreAction::Store,
                    .StencilLoad = render::LoadAction::DontCare,
                    .StencilStore = render::StoreAction::Discard,
                    .ClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}}};
                render::RenderPassDescriptor passDesc{
                    .ColorAttachments = {},
                    .DepthStencilAttachment = depthAttachment,
                    .Name = "glTF Viewer Additional Shadow Caster"};
                auto encoderOpt = cmd->BeginRenderPass(passDesc);
                if (!encoderOpt.HasValue()) {
                    RADRAY_ERR_LOG("failed to begin glTF viewer additional shadow caster pass");
                    return false;
                }
                auto encoder = encoderOpt.Release();
                SetResolutionViewportAndScissor(encoder.get(), *device, _additionalShadow.Resolution);

                if (i < sliceCount) {
                    const gltfviewer::AdditionalShadowSlice& slice = _additionalShadow.Slices[i];
                    srp::DrawObjectsPass::Desc desc{};
                    desc.Event = srp::RenderPassEvent::BeforeRenderingShadows;
                    desc.ShaderTags = {"ShadowCaster"};
                    desc.Filtering.QueueRange = srp::RenderQueueRange::Opaque();
                    desc.SortFlags = srp::SortingCriteria::FrontToBack;
                    desc.PassKeywords.Add(shader_define::ShadowCaster);
                    desc.RenderState = srp::MeshPassRenderState::Shadow(ShadowRasterDepthBias, ShadowRasterSlopeBias);
                    desc.RTFormats.DepthFormat = render::TextureFormat::D32_FLOAT;
                    desc.PerObjectParamName = "gScene";
                    desc.PerObjectByteSize = sizeof(SceneConstants);
                    desc.PerObjectFn = [slice](std::span<byte> dst, const srp::Renderer& renderer, const srp::SceneView&) {
                        if (dst.size() < sizeof(SceneConstants)) {
                            return;
                        }
                        SceneConstants sc{};
                        WriteMatrix(sc.MVP, slice.View.ViewProjMatrix);
                        WriteMatrix(sc.Model, renderer.WorldMatrix());
                        sc.CameraPosition[0] = slice.LightDirectionForBias.x();
                        sc.CameraPosition[1] = slice.LightDirectionForBias.y();
                        sc.CameraPosition[2] = slice.LightDirectionForBias.z();
                        sc.CameraPosition[3] = slice.DepthBias;
                        std::memcpy(&sc.Debug[1], &slice.NormalBias, sizeof(float));
                        std::memcpy(dst.data(), &sc, sizeof(SceneConstants));
                    };
                    srp::DrawObjectsPass pass{std::move(desc)};
                    runPass(encoder.get(), pass);
                }
                // i >= sliceCount:padding slice,仅清除(上面的 Clear 已填远深度)。

                cmd->EndRenderPass(std::move(encoder));
            }
            _resourcePool.Transition(AdditionalShadowMapDepthName, flight, render::TextureState::ShaderRead, *cmd);
        }

        // ============ 3. Pre-Z(只写深度)============
        {
            render::TextureView* depthView = AcquireDepthView(_resourcePool, flight, *device, cmd, width, height, DepthFormat);
            if (depthView == nullptr) {
                return false;
            }
            render::DepthStencilAttachment depthAttachment{
                .Target = depthView,
                .DepthLoad = render::LoadAction::Clear,
                .DepthStore = render::StoreAction::Store,
                .StencilLoad = render::LoadAction::DontCare,
                .StencilStore = render::StoreAction::Discard,
                .ClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}}};
            render::RenderPassDescriptor passDesc{
                .ColorAttachments = {},
                .DepthStencilAttachment = depthAttachment,
                .Name = "glTF Viewer Pre-Z"};
            auto encoderOpt = cmd->BeginRenderPass(passDesc);
            if (!encoderOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to begin glTF viewer Pre-Z pass");
                return false;
            }
            auto encoder = encoderOpt.Release();
            SetViewportAndScissor(encoder.get(), *device, width, height);

            srp::DrawObjectsPass::Desc desc{};
            desc.Event = srp::RenderPassEvent::BeforeRenderingPrePasses;
            desc.ShaderTags = {"DepthOnly"};
            desc.Filtering.QueueRange = srp::RenderQueueRange::Opaque();
            desc.SortFlags = srp::SortingCriteria::FrontToBack;
            desc.RenderState = srp::MeshPassRenderState::PreZ();
            desc.RTFormats.DepthFormat = DepthFormat;
            desc.PerObjectParamName = "gScene";
            desc.PerObjectByteSize = sizeof(SceneConstants);
            desc.PerObjectFn = [](std::span<byte> dst, const srp::Renderer& renderer, const srp::SceneView& v) {
                if (dst.size() < sizeof(SceneConstants)) {
                    return;
                }
                SceneConstants sc{};
                const Eigen::Matrix4f& model = renderer.WorldMatrix();
                WriteMatrix(sc.MVP, v.ViewProjMatrix * model);
                WriteMatrix(sc.Model, model);
                std::memcpy(dst.data(), &sc, sizeof(SceneConstants));
            };
            srp::DrawObjectsPass pass{std::move(desc)};
            runPass(encoder.get(), pass);

            cmd->EndRenderPass(std::move(encoder));
        }

        // ============ 4. Base(不透明,OpaqueBase:depth Equal + 不写深度)============
        {
            render::TextureView* depthView = AcquireDepthView(_resourcePool, flight, *device, cmd, width, height, DepthFormat);
            if (depthView == nullptr) {
                return false;
            }
            render::ColorAttachment colorAttachment{
                .Target = target.BackBufferView,
                .Load = render::LoadAction::Clear,
                .Store = render::StoreAction::Store,
                .ClearValue = render::ColorClearValue{{0.06f, 0.07f, 0.08f, 1.0f}}};
            render::DepthStencilAttachment depthAttachment{
                .Target = depthView,
                .DepthLoad = render::LoadAction::Load,
                .DepthStore = render::StoreAction::Store,
                .StencilLoad = render::LoadAction::DontCare,
                .StencilStore = render::StoreAction::Discard,
                .ClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}}};
            render::RenderPassDescriptor passDesc{
                .ColorAttachments = std::span{&colorAttachment, 1},
                .DepthStencilAttachment = depthAttachment,
                .Name = "glTF Viewer Base"};
            auto encoderOpt = cmd->BeginRenderPass(passDesc);
            if (!encoderOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to begin glTF viewer base pass");
                return false;
            }
            auto encoder = encoderOpt.Release();
            SetViewportAndScissor(encoder.get(), *device, width, height);

            const uint32_t overlay = _showShadowCascadeOverlay ? 7u : 0u;
            srp::DrawObjectsPass::Desc desc{};
            desc.Event = srp::RenderPassEvent::BeforeRenderingOpaques;
            desc.ShaderTags = {"UniversalForward"};
            desc.Filtering.QueueRange = srp::RenderQueueRange::Opaque();
            desc.SortFlags = srp::SortingCriteria::FrontToBack;
            desc.RenderState = srp::MeshPassRenderState::OpaqueBase();
            desc.RTFormats.ColorFormats = {BackBufferFormat};
            desc.RTFormats.DepthFormat = DepthFormat;
            desc.ViewSetIndex = render::DescriptorSetIndex{0};
            desc.ViewSetFn = [lightSet](const srp::SceneView&) { return lightSet; };
            desc.PerObjectParamName = "gScene";
            desc.PerObjectByteSize = sizeof(SceneConstants);
            desc.PerObjectFn = [overlay](std::span<byte> dst, const srp::Renderer& renderer, const srp::SceneView& v) {
                if (dst.size() < sizeof(SceneConstants)) {
                    return;
                }
                SceneConstants sc{};
                const Eigen::Matrix4f& model = renderer.WorldMatrix();
                WriteMatrix(sc.MVP, v.ViewProjMatrix * model);
                WriteMatrix(sc.Model, model);
                sc.CameraPosition[0] = v.EyePosition.x();
                sc.CameraPosition[1] = v.EyePosition.y();
                sc.CameraPosition[2] = v.EyePosition.z();
                sc.CameraPosition[3] = 1.0f;
                sc.Debug[0] = overlay;
                std::memcpy(dst.data(), &sc, sizeof(SceneConstants));
            };
            srp::DrawObjectsPass pass{std::move(desc)};
            runPass(encoder.get(), pass);

            cmd->EndRenderPass(std::move(encoder));
        }

        // ============ 5. Transparent(alpha-over,depth LessEqual + 不写深度)============
        {
            render::TextureView* depthView = AcquireDepthView(_resourcePool, flight, *device, cmd, width, height, DepthFormat);
            if (depthView == nullptr) {
                return false;
            }
            render::ColorAttachment colorAttachment{
                .Target = target.BackBufferView,
                .Load = render::LoadAction::Load,
                .Store = render::StoreAction::Store,
                .ClearValue = {}};
            render::DepthStencilAttachment depthAttachment{
                .Target = depthView,
                .DepthLoad = render::LoadAction::Load,
                .DepthStore = render::StoreAction::Store,
                .StencilLoad = render::LoadAction::DontCare,
                .StencilStore = render::StoreAction::Discard,
                .ClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}}};
            render::RenderPassDescriptor passDesc{
                .ColorAttachments = std::span{&colorAttachment, 1},
                .DepthStencilAttachment = depthAttachment,
                .Name = "glTF Viewer Transparent"};
            auto encoderOpt = cmd->BeginRenderPass(passDesc);
            if (!encoderOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to begin glTF viewer transparent pass");
                return false;
            }
            auto encoder = encoderOpt.Release();
            SetViewportAndScissor(encoder.get(), *device, width, height);

            srp::DrawObjectsPass::Desc desc{};
            desc.Event = srp::RenderPassEvent::BeforeRenderingTransparents;
            desc.ShaderTags = {"UniversalForward"};
            desc.Filtering.QueueRange = srp::RenderQueueRange::Transparent();
            desc.SortFlags = srp::SortingCriteria::BackToFront;
            desc.RenderState = srp::MeshPassRenderState::Transparent();
            desc.RTFormats.ColorFormats = {BackBufferFormat};
            desc.RTFormats.DepthFormat = DepthFormat;
            desc.ViewSetIndex = render::DescriptorSetIndex{0};
            desc.ViewSetFn = [lightSet](const srp::SceneView&) { return lightSet; };
            desc.PerObjectParamName = "gScene";
            desc.PerObjectByteSize = sizeof(SceneConstants);
            desc.PerObjectFn = [](std::span<byte> dst, const srp::Renderer& renderer, const srp::SceneView& v) {
                if (dst.size() < sizeof(SceneConstants)) {
                    return;
                }
                SceneConstants sc{};
                const Eigen::Matrix4f& model = renderer.WorldMatrix();
                WriteMatrix(sc.MVP, v.ViewProjMatrix * model);
                WriteMatrix(sc.Model, model);
                sc.CameraPosition[0] = v.EyePosition.x();
                sc.CameraPosition[1] = v.EyePosition.y();
                sc.CameraPosition[2] = v.EyePosition.z();
                sc.CameraPosition[3] = 1.0f;
                std::memcpy(dst.data(), &sc, sizeof(SceneConstants));
            };
            srp::DrawObjectsPass pass{std::move(desc)};
            runPass(encoder.get(), pass);

            cmd->EndRenderPass(std::move(encoder));
        }

        RenderShadowPreviewAtlas(_resourcePool, flight, *device, cmd, shadowSrv);
        return true;
    }

    void OnShutdown() override {
        DestroyExportedScene();
        _lightBuffer.Clear();
        _resourcePool.Clear();
        _shadowPreviewSets.clear();
        _shadowPreviewPso.reset();
        _shadowPreviewRootSig = nullptr;
        _shadowPreviewShader = nullptr;
        _shadowPreviewTexture = ImTextureID_Invalid;
        _gltfAsset.Reset();
        _cameraControlComp = nullptr;
        _cameraComp = nullptr;
        _directionalLight = nullptr;
        _spotLight = nullptr;
        _pointLight = nullptr;
        _variantCache.reset();
        _viewerShader.reset();
    }

private:
    bool EnsureShadowPreviewPipeline(render::Device& device, GpuSystem& gpu) {
        if (_shadowPreviewPso != nullptr && _shadowPreviewRootSig != nullptr) {
            return true;
        }

        ShaderCompileDescriptor shaderDesc{};
        shaderDesc.Name = "gltf_viewer_shadow_preview";
        shaderDesc.Source = ShadowPreviewShaderSource;
        shaderDesc.EntryPoint = "CSMain";
        shaderDesc.Stage = render::ShaderStage::Compute;
        _shadowPreviewShader = gpu.GetOrCompileShader(shaderDesc).Get();
        if (_shadowPreviewShader == nullptr) {
            RADRAY_ERR_LOG("failed to compile shadow preview shader");
            return false;
        }

        render::Shader* shaders[] = {_shadowPreviewShader};
        _shadowPreviewRootSig = gpu.GetOrCreateRootSignature(std::span<render::Shader*>{shaders}).Get();
        if (_shadowPreviewRootSig == nullptr) {
            RADRAY_ERR_LOG("failed to create shadow preview root signature");
            return false;
        }

        render::ComputePipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = _shadowPreviewRootSig;
        psoDesc.CS = render::ShaderEntry{
            .Target = _shadowPreviewShader,
            .EntryPoint = "CSMain"};
        auto psoOpt = device.CreateComputePipelineState(psoDesc);
        if (!psoOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to create shadow preview compute pipeline");
            return false;
        }
        _shadowPreviewPso = psoOpt.Release();
        _shadowPreviewPso->SetDebugName("Shadow preview atlas PSO");
        return true;
    }

    render::DescriptorSet* GetShadowPreviewDescriptorSet(
        render::Device& device,
        uint32_t flight,
        render::TextureView* shadowSrv,
        render::TextureView* atlasUav) {
        if (_shadowPreviewRootSig == nullptr || shadowSrv == nullptr || atlasUav == nullptr) {
            return nullptr;
        }
        if (_shadowPreviewSets.size() <= flight) {
            _shadowPreviewSets.resize(static_cast<size_t>(flight) + 1);
        }

        unique_ptr<render::DescriptorSet>& set = _shadowPreviewSets[flight];
        if (set == nullptr) {
            auto setOpt = device.CreateDescriptorSet(_shadowPreviewRootSig, render::DescriptorSetIndex{0});
            if (!setOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to create shadow preview descriptor set");
                return nullptr;
            }
            set = setOpt.Release();
            set->SetDebugName("shadow_preview_set");
        }
        if (!set->WriteResource("gShadowMap", shadowSrv) || !set->WriteResource("gPreviewAtlas", atlasUav)) {
            RADRAY_ERR_LOG("failed to update shadow preview descriptor set");
            return nullptr;
        }
        return set.get();
    }

    void RenderShadowPreviewAtlas(
        srp::RenderResourcePool& pool,
        uint32_t flight,
        render::Device& device,
        render::CommandBuffer* cmd,
        render::TextureView* shadowSrv) {
        _shadowPreviewReady = false;
        if (!_showShadowMapPreview || !_lastShadow.Enabled || shadowSrv == nullptr || cmd == nullptr) {
            return;
        }
        if (!EnsureShadowPreviewPipeline(device, *GetGpuSystem())) {
            return;
        }

        render::TextureView* atlasUav = AcquireShadowPreviewAtlasUnorderedAccessView(pool, flight, device);
        render::TextureView* atlasSrv = AcquireShadowPreviewAtlasResourceView(pool, flight, device);
        render::DescriptorSet* set = GetShadowPreviewDescriptorSet(device, flight, shadowSrv, atlasUav);
        if (atlasUav == nullptr || atlasSrv == nullptr || set == nullptr) {
            return;
        }

        pool.Transition(ShadowPreviewAtlasName, flight, render::TextureState::UnorderedAccess, *cmd);
        auto encoderOpt = cmd->BeginComputePass();
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to begin shadow preview compute pass");
            return;
        }
        auto encoder = encoderOpt.Release();
        encoder->BindRootSignature(_shadowPreviewRootSig);
        encoder->BindComputePipelineState(_shadowPreviewPso.get());
        encoder->BindDescriptorSet(render::DescriptorSetIndex{0}, set);
        constexpr uint32_t groupSize = 8;
        encoder->Dispatch(
            (ShadowPreviewAtlasSize + groupSize - 1) / groupSize,
            (ShadowPreviewAtlasSize + groupSize - 1) / groupSize,
            1);
        cmd->EndComputePass(std::move(encoder));
        pool.Transition(ShadowPreviewAtlasName, flight, render::TextureState::ShaderRead, *cmd);

        if (ImGuiSystem* imgui = GetSubsystem<ImGuiSystem>()) {
            ImTextureID updated = imgui->CreateOrUpdateExternalTexture(_shadowPreviewTexture, flight, atlasSrv);
            if (updated != ImTextureID_Invalid) {
                _shadowPreviewTexture = updated;
                _shadowPreviewReady = true;
            }
        }
    }

    void SpawnCamera() {
        Actor* cameraActor = GetWorld()->SpawnActor<Actor>();
        _cameraComp = cameraActor->AddComponent<CameraComponent>();
        cameraActor->SetRootComponent(_cameraComp);
        _cameraComp->SetPerspective(Radian(60.0f), 0.01f, 10000.0f);
        _cameraControlComp = cameraActor->AddComponent<CameraControlComponent>();
        _cameraControlComp->SetCamera(_cameraComp);
        _cameraControlComp->BindToMainWindow(*this);
    }

    void SpawnDefaultLights() {
        Actor* directionalActor = GetWorld()->SpawnActor<Actor>();
        auto* directional = directionalActor->AddComponent<LightComponent>();
        _directionalLight = directional;
        directionalActor->SetRootComponent(directional);
        directional->SetLightType(srp::LightType::Directional);
        directional->SetDirection(Eigen::Vector3f{-0.3f, -1.0f, -0.3f}.normalized());
        directional->SetColor(Eigen::Vector3f{0.8f, 0.6f, 1.0f});
        directional->SetIntensity(30.0f);
        directional->SetCastShadow(true);
        directional->SetShadowBias(1.0f, 0.75f);

        // Actor* spotActor = GetWorld()->SpawnActor<Actor>();
        // auto* spot = spotActor->AddComponent<LightComponent>();
        // _spotLight = spot;
        // spotActor->SetRootComponent(spot);
        // spot->SetLightType(srp::LightType::Spot);
        // spot->SetWorldLocation(Eigen::Vector3f{2.5f, 3.0f, 1.5f});
        // spot->SetDirection(Eigen::Vector3f{-0.6f, -1.0f, -0.35f}.normalized());
        // spot->SetColor(Eigen::Vector3f{1.0f, 0.95f, 0.85f});
        // spot->SetIntensity(120.0f);
        // spot->SetRange(20.0f);
        // spot->SetSpotAngles(Radian(22.0f), Radian(32.0f));
        // spot->SetCastShadow(true);
        // spot->SetShadowBias(1.0f, 1.0f);

        // Actor* pointActor = GetWorld()->SpawnActor<Actor>();
        // auto* point = pointActor->AddComponent<LightComponent>();
        // _pointLight = point;
        // pointActor->SetRootComponent(point);
        // point->SetLightType(srp::LightType::Point);
        // point->SetWorldLocation(Eigen::Vector3f{-2.0f, 2.0f, -2.0f});
        // point->SetColor(Eigen::Vector3f{0.7f, 0.85f, 1.0f});
        // point->SetIntensity(60.0f);
        // point->SetRange(15.0f);
        // point->SetCastShadow(true);
        // point->SetShadowBias(1.5f, 0.0f);
    }

    bool ComputeDirectionalCascades(const srp::SceneView& camView, gltfviewer::ShadowCascadeData& out) {
        out = gltfviewer::ShadowCascadeData{};
        _lastShadowSplits.fill(0.0f);
        srp::Scene* scene = GetWorld() != nullptr ? GetWorld()->GetScene() : nullptr;
        if (scene == nullptr || _cameraComp == nullptr) {
            return false;
        }

        Eigen::Vector3f lightDir{0.0f, -1.0f, 0.0f};
        float depthBiasTexels = 0.0f;
        float normalBiasTexels = 0.0f;
        bool found = false;
        for (const unique_ptr<srp::Light>& light : scene->Lights()) {
            if (light == nullptr || light->Type != srp::LightType::Directional || !light->CastShadow) {
                continue;
            }
            lightDir = light->Direction;
            depthBiasTexels = light->ShadowDepthBias;
            normalBiasTexels = light->ShadowNormalBias;
            found = true;
            break;
        }
        if (!found) {
            return false;
        }

        const float lightDirLen = lightDir.norm();
        if (lightDirLen > 1e-6f) {
            lightDir /= lightDirLen;
        } else {
            lightDir = Eigen::Vector3f{0.0f, -1.0f, 0.0f};
        }

        const float cameraNear = std::max(_cameraComp->GetNearZ(), 0.001f);
        const float cameraFar = std::max(_cameraComp->GetFarZ(), cameraNear + 0.1f);
        const float shadowFar = std::max(std::min(cameraFar, cameraNear + _shadowMaxDistance), cameraNear + 0.1f);
        const float aspect = camView.ViewportHeight != 0
            ? static_cast<float>(camView.ViewportWidth) / static_cast<float>(camView.ViewportHeight)
            : 1.0f;
        const float tanHalfFovY = std::tan(_cameraComp->GetFovY() * 0.5f);

        std::array<float, MaxShadowCascades + 1> splits{};
        splits[0] = cameraNear;
        const float farNearRatio = shadowFar / cameraNear;
        for (uint32_t i = 1; i <= MaxShadowCascades; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(MaxShadowCascades);
            const float uniformSplit = cameraNear + (shadowFar - cameraNear) * t;
            const float logSplit = cameraNear * std::pow(farNearRatio, t);
            splits[i] = std::lerp(uniformSplit, logSplit, _shadowSplitLambda);
        }
        splits[MaxShadowCascades] = shadowFar;
        _lastShadowSplits = splits;

        const Eigen::Vector3f up = std::abs(lightDir.y()) > 0.99f
            ? Eigen::Vector3f{0.0f, 0.0f, 1.0f}
            : Eigen::Vector3f{0.0f, 1.0f, 0.0f};
        const Eigen::Matrix4f invCamView = camView.ViewMatrix.inverse();

        out.LightDirectionForBias = -lightDir;
        out.SoftMode = _shadowSoftMode;
        const float kernelRadius = ShadowSoftKernelRadius(out.SoftMode);
        for (uint32_t cascadeIndex = 0; cascadeIndex < MaxShadowCascades; ++cascadeIndex) {
            const float zNear = splits[cascadeIndex];
            const float zFar = std::max(splits[cascadeIndex + 1], zNear + 0.01f);

            std::array<Eigen::Vector3f, 8> corners{};
            uint32_t cornerIndex = 0;
            for (float z : {zNear, zFar}) {
                const float halfY = tanHalfFovY * z;
                const float halfX = halfY * aspect;
                for (float ySign : {-1.0f, 1.0f}) {
                    for (float xSign : {-1.0f, 1.0f}) {
                        const Eigen::Vector4f world = invCamView * Eigen::Vector4f{xSign * halfX, ySign * halfY, z, 1.0f};
                        corners[cornerIndex++] = world.head<3>() / world.w();
                    }
                }
            }

            Eigen::Vector3f center = Eigen::Vector3f::Zero();
            for (const Eigen::Vector3f& corner : corners) {
                center += corner;
            }
            center /= static_cast<float>(corners.size());

            float radius = 0.0f;
            for (const Eigen::Vector3f& corner : corners) {
                radius = std::max(radius, (corner - center).norm());
            }
            radius = std::max(radius, 0.1f);

            const float margin = std::max(radius * 0.05f, 0.05f);
            const Eigen::Vector3f eye = center - lightDir * (radius + margin);
            Eigen::Matrix4f viewM = LookAtFrontLH(eye, lightDir, up);
            const Eigen::Vector4f lightCenter = viewM * Eigen::Vector4f{center.x(), center.y(), center.z(), 1.0f};
            const float centerZ = lightCenter.z() / lightCenter.w();
            const float nearZ = std::max(0.001f, centerZ - radius - margin);
            const float farZ = std::max(nearZ + 0.1f, centerZ + radius + margin);
            Eigen::Matrix4f projM = OrthoLH(-radius, radius, -radius, radius, nearZ, farZ);

            gltfviewer::ShadowCascade& cascade = out.Cascades[cascadeIndex];
            cascade.View = srp::SceneView{};
            cascade.View.ViewMatrix = viewM;
            cascade.View.ProjMatrix = projM;
            cascade.View.ViewProjMatrix = projM * viewM;
            cascade.View.EyePosition = eye;
            cascade.View.ViewportWidth = ShadowMapResolution;
            cascade.View.ViewportHeight = ShadowMapResolution;

            const float texelWorldSize = (2.0f * radius) / static_cast<float>(ShadowMapResolution);
            cascade.DepthBias = -std::max(depthBiasTexels, 0.0f) * texelWorldSize * kernelRadius;
            cascade.NormalBias = -std::max(normalBiasTexels, 0.0f) * texelWorldSize * kernelRadius;

            out.SphereCenters[cascadeIndex] = center;
            out.SphereRadiiSq[cascadeIndex] = radius * radius;
        }

        out.CascadeCount = MaxShadowCascades;
        out.Enabled = true;
        return true;
    }

    void ConfigureCameraControl() {
        if (_cameraControlComp == nullptr) {
            return;
        }
        CameraControl& control = _cameraControlComp->GetControl();
        control.MinDistance = 0.05f;
        control.MaxDistance = 10000.0f;
        control.OrbitSensitivity = 0.003f;
        control.PanSensitivity = 0.003f;
        control.DollySensitivity = 0.15f;
        control.UseTrackball = false;
        control.InvertZoom = false;
    }

    void LoadSceneFromPath(const std::filesystem::path& path) {
        UnloadCurrentScene();
        _loadError.clear();
        _selectedNode = -1;

        GltfAssetLoadOptions options{};
        _gltfAsset = LoadGltfAsset(
            *GetAssetManager(),
            GetGpuSystem()->GetFrameUploadScheduler(),
            path,
            options);
        if (!_gltfAsset.IsValid()) {
            _loadError = "failed to start glTF asset load";
            RADRAY_ERR_LOG("{}", _loadError);
            return;
        }
    }

    void UnloadCurrentScene() {
        DestroyExportedScene();
        if (_gltfAsset.IsValid()) {
            if (GltfAsset* asset = _gltfAsset.Get()) {
                for (const GltfTextureDesc& texture : asset->GetTextures()) {
                    if (texture.Texture.IsValid()) {
                        GetAssetManager()->Unload(texture.Texture.GetAssetId());
                    }
                }
            }
            GetAssetManager()->Unload(_gltfAsset.GetAssetId());
            _gltfAsset.Reset();
        }
    }

    static void CollectAttachedActors(SceneComponent* component, Actor* exclude, vector<Actor*>& out) {
        if (component == nullptr) {
            return;
        }
        for (SceneComponent* child : component->GetAttachChildren()) {
            if (child == nullptr) {
                continue;
            }
            CollectAttachedActors(child, exclude, out);
            auto owner = child->GetOwner();
            if (!owner || owner.Get() == exclude) {
                continue;
            }
            Actor* actor = owner.Get();
            if (std::find(out.begin(), out.end(), actor) == out.end()) {
                out.push_back(actor);
            }
        }
    }

    void DestroyExportedScene() {
        Actor* rootActor = _rootActor;
        if (rootActor == nullptr) {
            return;
        }

        vector<Actor*> childActors;
        if (auto rootComponent = rootActor->GetRootComponent()) {
            CollectAttachedActors(rootComponent.Get(), rootActor, childActors);
        }

        for (Actor* actor : childActors) {
            if (actor == nullptr) {
                continue;
            }
            auto actorWorld = actor->GetWorld();
            if (actorWorld) {
                actorWorld.Get()->DestroyActor(actor);
            }
        }

        auto rootWorld = rootActor->GetWorld();
        if (rootWorld) {
            rootWorld.Get()->DestroyActor(rootActor);
        }
        _rootActor = nullptr;
    }

    void TryExportReadyAsset() {
        if (!_gltfAsset.IsValid()) {
            return;
        }
        if (_gltfAsset.IsFaulted()) {
            if (_loadError.empty()) {
                _loadError = "glTF asset load failed";
            }
            return;
        }
        if (_rootActor == nullptr && _gltfAsset.IsReady()) {
            _rootActor = _gltfAsset->ExportToScene(*GetWorld(), _viewerShader.get(), GetDevice());
            FrameCameraToAsset();
        }
    }

    void FrameCameraToAsset() {
        Eigen::Vector3f center{Eigen::Vector3f::Zero()};
        float distance = 4.0f;
        const GltfAsset* asset = _gltfAsset.Get();
        if (asset != nullptr && asset->HasBounds()) {
            center = (asset->GetBoundsMin() + asset->GetBoundsMax()) * 0.5f;
            const float radius = std::max((asset->GetBoundsMax() - asset->GetBoundsMin()).norm() * 0.5f, 0.1f);
            distance = radius * 2.8f;
        }
        SetCameraFrame(center, distance);
    }

    void SetCameraFrame(const Eigen::Vector3f& center, float distance) {
        if (_cameraControlComp != nullptr) {
            _cameraControlComp->SetFrame(center, distance, 0.0f, Radian(20.0f));
        }
    }

    void DrawNodeTree(const GltfAsset& asset, int nodeIndex) {
        const vector<GltfNodeDesc>& nodes = asset.GetNodes();
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(nodes.size())) {
            return;
        }
        const GltfNodeDesc& node = nodes[static_cast<size_t>(nodeIndex)];
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (node.Children.empty()) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (_selectedNode == nodeIndex) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<intptr_t>(nodeIndex)),
            flags,
            "%s%s",
            node.Name.c_str(),
            node.HasMesh ? "  [mesh]" : "");
        if (ImGui::IsItemClicked()) {
            _selectedNode = nodeIndex;
        }
        if (open) {
            for (int child : node.Children) {
                DrawNodeTree(asset, child);
            }
            ImGui::TreePop();
        }
    }

    void DrawShadowDebugUi() {
        if (!ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }

        ImGui::Checkbox("Cascade overlay", &_showShadowCascadeOverlay);
        ImGui::Checkbox("Shadow map preview", &_showShadowMapPreview);
        const char* softModeNames[] = {"Hard (1 tap)", "Low (4 tap)", "Medium (5x5 Tent)"};
        int softModeIndex = static_cast<int>(_shadowSoftMode);
        if (ImGui::Combo("Soft quality", &softModeIndex, softModeNames, IM_ARRAYSIZE(softModeNames))) {
            _shadowSoftMode = static_cast<gltfviewer::ShadowSoftMode>(softModeIndex);
        }
        if (_directionalLight != nullptr) {
            float depthBias = _directionalLight->GetShadowDepthBias();
            float normalBias = _directionalLight->GetShadowNormalBias();
            bool changed = false;
            changed |= ImGui::SliderFloat("Depth bias", &depthBias, 0.0f, 5.0f, "%.2f");
            changed |= ImGui::SliderFloat("Normal bias", &normalBias, 0.0f, 5.0f, "%.2f");
            if (changed) {
                _directionalLight->SetShadowBias(depthBias, normalBias);
            }
        }
        ImGui::SliderFloat("Split lambda", &_shadowSplitLambda, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Max distance", &_shadowMaxDistance, 5.0f, 200.0f, "%.1f");
        ImGui::Text("Enabled: %s", _lastShadow.Enabled ? "yes" : "no");
        ImGui::Text("Cascades: %u / %u", _lastShadow.CascadeCount, MaxShadowCascades);
        ImGui::Text("Resolution: %u", ShadowMapResolution);
        ImGui::Text(
            "Additional (spot/point): %u lights, %u / %u slices @ %u",
            _lastAdditionalShadowLights,
            _lastAdditionalShadowSlices,
            MaxAdditionalShadowSlices,
            AdditionalShadowMapResolution);
        if (_lastShadow.Enabled && _lastShadow.CascadeCount > 0) {
            ImGui::Text("Cascade 0 end: %.3f", _lastShadowSplits[1]);
        }
        const Eigen::Vector3f& lightDir = _lastShadow.LightDirectionForBias;
        ImGui::Text("Bias light dir: %.3f %.3f %.3f", lightDir.x(), lightDir.y(), lightDir.z());

        if (_showShadowMapPreview) {
            ImGui::Separator();
            ImGui::Text("Shadow map atlas: 0 1 / 2 3");
            if (_shadowPreviewTexture != ImTextureID_Invalid && _shadowPreviewReady) {
                const float availableWidth = std::max(ImGui::GetContentRegionAvail().x, 120.0f);
                const float previewSize = std::min(availableWidth, 384.0f);
                ImGui::Image(ImTextureRef{_shadowPreviewTexture}, ImVec2{previewSize, previewSize});
            } else {
                ImGui::TextUnformatted("Shadow map preview unavailable");
            }
        }

        static constexpr ImVec4 colors[] = {
            ImVec4{1.00f, 0.20f, 0.16f, 1.0f},
            ImVec4{0.15f, 0.85f, 0.25f, 1.0f},
            ImVec4{0.20f, 0.45f, 1.00f, 1.0f},
            ImVec4{1.00f, 0.86f, 0.18f, 1.0f}};

        const uint32_t count = std::min<uint32_t>(_lastShadow.CascadeCount, MaxShadowCascades);
        for (uint32_t i = 0; i < count; ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::ColorButton("##cascade_color", colors[i], ImGuiColorEditFlags_NoTooltip, ImVec2{12.0f, 12.0f});
            ImGui::SameLine();
            const bool open = ImGui::TreeNodeEx("Cascade", ImGuiTreeNodeFlags_DefaultOpen, "Cascade %u", i);
            if (open) {
                const Eigen::Vector3f& center = _lastShadow.SphereCenters[i];
                const float radius = std::sqrt(std::max(_lastShadow.SphereRadiiSq[i], 0.0f));
                const gltfviewer::ShadowCascade& cascade = _lastShadow.Cascades[i];
                ImGui::Text("Split: %.3f - %.3f", _lastShadowSplits[i], _lastShadowSplits[i + 1]);
                ImGui::Text("Sphere center: %.3f %.3f %.3f", center.x(), center.y(), center.z());
                ImGui::Text("Sphere radius: %.3f", radius);
                ImGui::Text("Depth bias: %.6f", cascade.DepthBias);
                ImGui::Text("Normal bias: %.6f", cascade.NormalBias);
                ImGui::Text(
                    "Eye: %.3f %.3f %.3f",
                    cascade.View.EyePosition.x(),
                    cascade.View.EyePosition.y(),
                    cascade.View.EyePosition.z());
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    void DrawUi(const AppUpdateContext& ctx) {
        ImGui::SetNextWindowSize(ImVec2{380.0f, 560.0f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("glTF Viewer")) {
            ImGui::Text("Frame %.3f ms  GPU %.3f ms", ctx.DeltaTime.count() * 1000.0f, GetGpuSystem()->GetLastGpuTimeMs());
            ImGui::InputText("Path", _loadPath.data(), _loadPath.size());
            if (ImGui::Button("Load")) {
                _pendingLoadPath = _loadPath.data();
                _pendingLoad = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Unload")) {
                UnloadCurrentScene();
                _selectedNode = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Frame")) {
                FrameCameraToAsset();
            }
            if (!_loadError.empty()) {
                ImGui::TextWrapped("Error: %s", _loadError.c_str());
            }
            ImGui::Separator();
            DrawShadowDebugUi();
            ImGui::Separator();
            if (_gltfAsset.IsValid()) {
                const char* state = "loading";
                if (_gltfAsset.IsReady()) {
                    state = "ready";
                } else if (_gltfAsset.IsFaulted()) {
                    state = "faulted";
                } else if (_gltfAsset.IsCanceled()) {
                    state = "canceled";
                }
                ImGui::Text("Asset: %s", state);
            }
            if (GltfAsset* asset = _gltfAsset.Get()) {
                const size_t primitiveCount = asset->GetPrimitives().size();
                const size_t actorCount = _rootActor != nullptr ? primitiveCount + 1 : 0;
                const size_t componentCount = _rootActor != nullptr ? primitiveCount : 0;
                ImGui::Text("Primitives: %zu", primitiveCount);
                ImGui::Text("Materials: %zu", asset->GetMaterialNames().size());
                ImGui::Text("Textures: %zu", asset->GetTextures().size());
                ImGui::Text("Nodes: %zu", asset->GetNodes().size());
                ImGui::Text("Actors: %zu", actorCount);
                ImGui::Text("Components: %zu", componentCount);
                if (ImGui::CollapsingHeader("Scene Nodes", ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (int root : asset->GetRootNodes()) {
                        DrawNodeTree(*asset, root);
                    }
                }
                if (ImGui::CollapsingHeader("Materials")) {
                    const vector<string>& materialNames = asset->GetMaterialNames();
                    for (size_t i = 0; i < materialNames.size(); ++i) {
                        ImGui::BulletText("%zu: %s", i, materialNames[i].c_str());
                    }
                }
                if (_selectedNode >= 0 && _selectedNode < static_cast<int>(asset->GetNodes().size())) {
                    const GltfNodeDesc& node = asset->GetNodes()[static_cast<size_t>(_selectedNode)];
                    ImGui::Separator();
                    ImGui::Text("Selected: %s", node.Name.c_str());
                    ImGui::Text("Parent: %d", node.Parent);
                    ImGui::Text("Children: %zu", node.Children.size());
                    ImGui::Text("Has mesh: %s", node.HasMesh ? "yes" : "no");
                }
            }
        }
        ImGui::End();
    }

    // —— srp 渲染设施(game 侧持有)——
    unique_ptr<srp::Shader> _viewerShader;
    unique_ptr<srp::ShaderVariantCache> _variantCache;
    srp::RenderResourcePool _resourcePool;
    gltfviewer::SceneLightBuffer _lightBuffer;

    // —— 阴影状态 ——
    gltfviewer::ShadowCascadeData _shadow{};
    gltfviewer::ShadowCascadeData _lastShadow{};
    gltfviewer::AdditionalShadowData _additionalShadow{};
    std::array<float, MaxShadowCascades + 1> _lastShadowSplits{};
    uint32_t _lastAdditionalShadowSlices{0};
    uint32_t _lastAdditionalShadowLights{0};
    float _shadowMaxDistance{DefaultShadowMaxDistance};
    float _shadowSplitLambda{DefaultShadowSplitLambda};
    gltfviewer::ShadowSoftMode _shadowSoftMode{gltfviewer::ShadowSoftMode::Medium};
    bool _showShadowCascadeOverlay{false};
    bool _showShadowMapPreview{true};
    bool _shadowPreviewReady{false};
    ImTextureID _shadowPreviewTexture{ImTextureID_Invalid};
    render::Shader* _shadowPreviewShader{nullptr};
    render::RootSignature* _shadowPreviewRootSig{nullptr};
    unique_ptr<render::ComputePipelineState> _shadowPreviewPso;
    vector<unique_ptr<render::DescriptorSet>> _shadowPreviewSets;

    // —— 资产 / 场景 ——
    StreamingAssetRef<GltfAsset> _gltfAsset;
    Actor* _rootActor{nullptr};

    vector<char> _loadPath;
    string _loadError;
    std::filesystem::path _initialLoadPath;
    bool _pendingLoad{false};
    std::filesystem::path _pendingLoadPath;
    int _selectedNode{-1};

    CameraComponent* _cameraComp{nullptr};
    CameraControlComponent* _cameraControlComp{nullptr};
    LightComponent* _directionalLight{nullptr};
    LightComponent* _spotLight{nullptr};
    LightComponent* _pointLight{nullptr};
};

int main(int argc, char* argv[]) {
    ApplicationRuntimeDescriptor desc{};
    desc.Backend = render::RenderBackend::Vulkan;
    desc.AppName = "RadRay glTF Viewer";
    desc.EngineName = "RadRay";
    desc.WindowTitle = "RadRay glTF Viewer";
    desc.WindowWidth = 1280;
    desc.WindowHeight = 720;
    desc.BackBufferFormat = GltfViewerApp::BackBufferFormat;
    desc.PresentMode = render::PresentMode::FIFO;

    std::optional<std::filesystem::path> loadPath;
    for (int i = 0; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--backend" && i + 1 < argc) {
            std::string_view backendStr{argv[++i]};
            if (backendStr == "vulkan") {
                desc.Backend = render::RenderBackend::Vulkan;
            } else if (backendStr == "d3d12") {
                desc.Backend = render::RenderBackend::D3D12;
            }
        } else if (arg == "--valid-layer") {
            desc.EnableValidation = true;
        } else if (arg == "--multithread") {
            desc.Multithreaded = true;
        } else if (arg == "--load" && i + 1 < argc) {
            loadPath = std::filesystem::path{argv[++i]};
        }
    }

    GltfViewerApp app{};
    if (loadPath.has_value()) {
        app.SetInitialLoadPath(loadPath.value());
    }
    app.RegisterSubsystem<ImGuiSystem>();
    return app.Run(desc);
}
