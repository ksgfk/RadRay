#include <radray/runtime/application.h>
#include <radray/runtime/imgui_system.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gltf_asset.h>
#include <radray/runtime/material.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/camera_control_component.h>
#include <radray/runtime/components/light_component.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/renderer/render_context.h>
#include <radray/runtime/renderer/render_pass.h>
#include <radray/runtime/renderer/render_pipeline.h>
#include <radray/runtime/renderer/render_resource_pool.h>
#include <radray/runtime/renderer/light_scene_proxy.h>
#include <radray/runtime/renderer/scene.h>
#include <radray/runtime/renderer/scene_light_buffer.h>
#include <radray/runtime/renderer/scene_renderer.h>
#include <radray/render/common.h>
#include <radray/basic_math.h>
#include <radray/logger.h>
#include <radray/types.h>

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
constexpr uint32_t MaxShadowCascades = ShadowCascadeData::MaxCascades; // Must match SceneLightBuffer/Shader cascade constants.
constexpr uint32_t ShadowMapResolution = 2048;
constexpr uint32_t AdditionalShadowMapResolution = 1024;
constexpr uint32_t MaxAdditionalShadowSlices = AdditionalShadowData::MaxSlices; // Must match SceneLightBuffer/Shader slice constants.
constexpr uint32_t ShadowPreviewCellSize = 256;
constexpr uint32_t ShadowPreviewAtlasSize = ShadowPreviewCellSize * 2;
constexpr float DefaultShadowMaxDistance = 50.0f;
constexpr float DefaultShadowSplitLambda = 0.85f;
constexpr int32_t ShadowRasterDepthBias = 0;
constexpr float ShadowRasterSlopeBias = 0.0f;
static_assert(MaxShadowCascades == SceneLightBuffer::MaxShadowCascadesGpu);
static_assert(MaxShadowCascades == 4);
static_assert(MaxAdditionalShadowSlices == SceneLightBuffer::MaxAdditionalShadowSlicesGpu);

float ShadowSoftKernelRadius(ShadowSoftMode mode) noexcept {
    switch (mode) {
    case ShadowSoftMode::Low:
        return 1.5f;
    case ShadowSoftMode::Medium:
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

bool IsCommonRenderContextValid(const RenderContext& ctx) noexcept {
    return ctx.Width != 0 && ctx.Height != 0 && ctx.Scene != nullptr &&
        ctx.CmdBuffer != nullptr && ctx.Resources != nullptr &&
        ctx.Device != nullptr && ctx.Gpu != nullptr;
}

render::TextureView* AcquireDepthView(RenderContext& ctx, render::TextureFormat depthFormat) {
    render::TextureDescriptor depthDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Width = ctx.Width,
        .Height = ctx.Height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = depthFormat,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::DepthStencilWrite,
        .Hints = render::ResourceHint::None};
    if (ctx.Resources->Acquire(GltfViewerDepthName, ctx.FlightIndex, depthDesc, *ctx.Device) == nullptr) {
        return nullptr;
    }
    ctx.Resources->Transition(GltfViewerDepthName, ctx.FlightIndex, render::TextureState::DepthWrite, *ctx.CmdBuffer);

    render::TextureViewDescriptor depthViewDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Format = depthFormat,
        .Range = render::SubresourceRange::AllSub(),
        .Usage = render::TextureViewUsage::DepthWrite};
    return ctx.Resources->GetView(GltfViewerDepthName, ctx.FlightIndex, depthViewDesc, *ctx.Device);
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

render::TextureView* AcquireShadowSliceDepthView(RenderContext& ctx, uint32_t cascade) {
    const render::TextureDescriptor shadowDesc = MakeShadowDepthDescriptor();
    if (ctx.Resources->Acquire(ShadowMapDepthName, ctx.FlightIndex, shadowDesc, *ctx.Device) == nullptr) {
        return nullptr;
    }
    ctx.Resources->Transition(ShadowMapDepthName, ctx.FlightIndex, render::TextureState::DepthWrite, *ctx.CmdBuffer);

    render::TextureViewDescriptor depthViewDesc{
        .Dim = render::TextureDimension::Dim2DArray,
        .Format = render::TextureFormat::D32_FLOAT,
        .Range = render::SubresourceRange{cascade, 1, 0, 1},
        .Usage = render::TextureViewUsage::DepthWrite};
    return ctx.Resources->GetView(ShadowMapDepthName, ctx.FlightIndex, depthViewDesc, *ctx.Device);
}

render::TextureView* AcquireShadowArrayResourceView(RenderContext& ctx) {
    const render::TextureDescriptor shadowDesc = MakeShadowDepthDescriptor();
    if (ctx.Resources->Acquire(ShadowMapDepthName, ctx.FlightIndex, shadowDesc, *ctx.Device) == nullptr) {
        return nullptr;
    }

    render::TextureViewDescriptor viewDesc{
        .Dim = render::TextureDimension::Dim2DArray,
        .Format = render::TextureFormat::D32_FLOAT,
        .Range = render::SubresourceRange::AllSub(),
        .Usage = render::TextureViewUsage::Resource};
    return ctx.Resources->GetView(ShadowMapDepthName, ctx.FlightIndex, viewDesc, *ctx.Device);
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

render::TextureView* AcquireAdditionalShadowSliceDepthView(RenderContext& ctx, uint32_t slice) {
    const render::TextureDescriptor shadowDesc = MakeAdditionalShadowDepthDescriptor();
    if (ctx.Resources->Acquire(AdditionalShadowMapDepthName, ctx.FlightIndex, shadowDesc, *ctx.Device) == nullptr) {
        return nullptr;
    }
    ctx.Resources->Transition(AdditionalShadowMapDepthName, ctx.FlightIndex, render::TextureState::DepthWrite, *ctx.CmdBuffer);

    render::TextureViewDescriptor depthViewDesc{
        .Dim = render::TextureDimension::Dim2DArray,
        .Format = render::TextureFormat::D32_FLOAT,
        .Range = render::SubresourceRange{slice, 1, 0, 1},
        .Usage = render::TextureViewUsage::DepthWrite};
    return ctx.Resources->GetView(AdditionalShadowMapDepthName, ctx.FlightIndex, depthViewDesc, *ctx.Device);
}

render::TextureView* AcquireAdditionalShadowArrayResourceView(RenderContext& ctx) {
    const render::TextureDescriptor shadowDesc = MakeAdditionalShadowDepthDescriptor();
    if (ctx.Resources->Acquire(AdditionalShadowMapDepthName, ctx.FlightIndex, shadowDesc, *ctx.Device) == nullptr) {
        return nullptr;
    }

    render::TextureViewDescriptor viewDesc{
        .Dim = render::TextureDimension::Dim2DArray,
        .Format = render::TextureFormat::D32_FLOAT,
        .Range = render::SubresourceRange::AllSub(),
        .Usage = render::TextureViewUsage::Resource};
    return ctx.Resources->GetView(AdditionalShadowMapDepthName, ctx.FlightIndex, viewDesc, *ctx.Device);
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

render::TextureView* AcquireShadowPreviewAtlasResourceView(RenderContext& ctx) {
    const render::TextureDescriptor desc = MakeShadowPreviewAtlasDescriptor();
    if (ctx.Resources->Acquire(ShadowPreviewAtlasName, ctx.FlightIndex, desc, *ctx.Device) == nullptr) {
        return nullptr;
    }

    render::TextureViewDescriptor viewDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Format = desc.Format,
        .Range = render::SubresourceRange::AllSub(),
        .Usage = render::TextureViewUsage::Resource};
    return ctx.Resources->GetView(ShadowPreviewAtlasName, ctx.FlightIndex, viewDesc, *ctx.Device);
}

render::TextureView* AcquireShadowPreviewAtlasUnorderedAccessView(RenderContext& ctx) {
    const render::TextureDescriptor desc = MakeShadowPreviewAtlasDescriptor();
    if (ctx.Resources->Acquire(ShadowPreviewAtlasName, ctx.FlightIndex, desc, *ctx.Device) == nullptr) {
        return nullptr;
    }

    render::TextureViewDescriptor viewDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Format = desc.Format,
        .Range = render::SubresourceRange::AllSub(),
        .Usage = render::TextureViewUsage::UnorderedAccess};
    return ctx.Resources->GetView(ShadowPreviewAtlasName, ctx.FlightIndex, viewDesc, *ctx.Device);
}

void SetViewportAndScissor(render::GraphicsCommandEncoder* encoder, const RenderContext& ctx) {
    Viewport vp{
        .X = 0.0f,
        .Y = 0.0f,
        .Width = static_cast<float>(ctx.Width),
        .Height = static_cast<float>(ctx.Height),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f};
    if (ctx.Device->GetBackend() == render::RenderBackend::Vulkan) {
        vp.Y = static_cast<float>(ctx.Height);
        vp.Height = -static_cast<float>(ctx.Height);
    }
    encoder->SetViewport(vp);
    encoder->SetScissor(Rect{0, 0, ctx.Width, ctx.Height});
}

void SetShadowViewportAndScissor(render::GraphicsCommandEncoder* encoder, render::Device& device) {
    Viewport vp{
        .X = 0.0f,
        .Y = 0.0f,
        .Width = static_cast<float>(ShadowMapResolution),
        .Height = static_cast<float>(ShadowMapResolution),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f};
    if (device.GetBackend() == render::RenderBackend::Vulkan) {
        vp.Y = static_cast<float>(ShadowMapResolution);
        vp.Height = -static_cast<float>(ShadowMapResolution);
    }
    encoder->SetViewport(vp);
    encoder->SetScissor(Rect{0, 0, ShadowMapResolution, ShadowMapResolution});
}

void SetResolutionViewportAndScissor(render::GraphicsCommandEncoder* encoder, render::Device& device, uint32_t resolution) {
    Viewport vp{
        .X = 0.0f,
        .Y = 0.0f,
        .Width = static_cast<float>(resolution),
        .Height = static_cast<float>(resolution),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f};
    if (device.GetBackend() == render::RenderBackend::Vulkan) {
        vp.Y = static_cast<float>(resolution);
        vp.Height = -static_cast<float>(resolution);
    }
    encoder->SetViewport(vp);
    encoder->SetScissor(Rect{0, 0, resolution, resolution});
}

bool OpaquePrimitiveFilter(const PrimitiveSceneProxy& primitive) noexcept {
    // glTF 每个 primitive 只引用一个材质,透明(alphaMode=BLEND)与不透明天然按
    // primitive 切分。不透明 pass(Pre-Z + Base)只画不透明部分,透明部分留给
    // TransparentPass,避免玻璃写深度/遮挡后方几何。
    return !primitive.IsTransparent();
}

bool TransparentPrimitiveFilter(const PrimitiveSceneProxy& primitive) noexcept {
    return primitive.IsTransparent();
}

class ShadowCasterPass : public RenderPass {
public:
    std::string_view GetName() const noexcept override { return "GltfViewerShadowCasterPass"; }

    void Execute(RenderContext& ctx) override {
        if (!IsCommonRenderContextValid(ctx)) {
            return;
        }

        const uint32_t cascadeCount = ctx.Shadow.Enabled
            ? std::min<uint32_t>(ctx.Shadow.CascadeCount, MaxShadowCascades)
            : 0u;
        const uint32_t passes = std::max<uint32_t>(cascadeCount, 1u);

        for (uint32_t i = 0; i < passes; ++i) {
            render::TextureView* depthView = AcquireShadowSliceDepthView(ctx, i);
            if (depthView == nullptr) {
                return;
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
            auto encoderOpt = ctx.CmdBuffer->BeginRenderPass(passDesc);
            if (!encoderOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to begin glTF viewer shadow caster pass");
                return;
            }
            auto encoder = encoderOpt.Release();

            SetShadowViewportAndScissor(encoder.get(), *ctx.Device);

            render::DepthStencilState depthState = render::DepthStencilState::Default();
            depthState.DepthCompare = render::CompareFunction::LessEqual;
            depthState.DepthWriteEnable = true;
            depthState.DepthBias = render::DepthBiasState{ShadowRasterDepthBias, ShadowRasterSlopeBias, 0.0f};

            const ShadowCascade& cascade = ctx.Shadow.Cascades[i];
            MeshPassProcessor::Config processorConfig{};
            processorConfig.Cache = &ctx.Gpu->GetPSOCache();
            processorConfig.RtFormats.DepthFormat = render::TextureFormat::D32_FLOAT;
            processorConfig.ObjectConstantsParam = "gScene";
            processorConfig.Gpu = ctx.Gpu;
            processorConfig.PipelineOverride.DepthStencil = depthState;
            processorConfig.PipelineOverride.DisablePixelShader = true;
            processorConfig.PipelineOverride.KeyTag = "shadow";
            processorConfig.ObjectConstantsOverride = [&ctx, &cascade](ObjectConstants& constants, const PrimitiveSceneProxy&, const SceneView& view) {
                std::memcpy(constants.MVP, view.ViewProjMatrix.data(), sizeof(constants.MVP));
                constants.CameraPosition[0] = ctx.Shadow.LightDirectionForBias.x();
                constants.CameraPosition[1] = ctx.Shadow.LightDirectionForBias.y();
                constants.CameraPosition[2] = ctx.Shadow.LightDirectionForBias.z();
                constants.CameraPosition[3] = cascade.DepthBias;
                constants.Debug[0] = 6;
                std::memcpy(&constants.Debug[1], &cascade.NormalBias, sizeof(cascade.NormalBias));
            };

            if (cascadeCount > 0 && ctx.Visible != nullptr) {
                _sceneRenderer.DrawRenderers(
                    encoder.get(),
                    *ctx.Visible,
                    cascade.View,
                    processorConfig,
                    OpaquePrimitiveFilter);
            }

            ctx.CmdBuffer->EndRenderPass(std::move(encoder));
        }

        ctx.Resources->Transition(ShadowMapDepthName, ctx.FlightIndex, render::TextureState::ShaderRead, *ctx.CmdBuffer);
    }

private:
    SceneRenderer _sceneRenderer;
};

class AdditionalShadowCasterPass : public RenderPass {
public:
    std::string_view GetName() const noexcept override { return "GltfViewerAdditionalShadowCasterPass"; }

    void Execute(RenderContext& ctx) override {
        if (!IsCommonRenderContextValid(ctx)) {
            return;
        }

        const AdditionalShadowData& shadow = ctx.AdditionalShadow;
        const uint32_t sliceCount = shadow.Enabled
            ? std::min<uint32_t>(shadow.SliceCount, MaxAdditionalShadowSlices)
            : 0u;
        // Always clear at least one slice so a stale atlas never leaks shadows when casters disappear.
        const uint32_t passes = std::max<uint32_t>(sliceCount, 1u);

        for (uint32_t i = 0; i < passes; ++i) {
            render::TextureView* depthView = AcquireAdditionalShadowSliceDepthView(ctx, i);
            if (depthView == nullptr) {
                return;
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
            auto encoderOpt = ctx.CmdBuffer->BeginRenderPass(passDesc);
            if (!encoderOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to begin glTF viewer additional shadow caster pass");
                return;
            }
            auto encoder = encoderOpt.Release();

            SetResolutionViewportAndScissor(encoder.get(), *ctx.Device, shadow.Resolution);

            render::DepthStencilState depthState = render::DepthStencilState::Default();
            depthState.DepthCompare = render::CompareFunction::LessEqual;
            depthState.DepthWriteEnable = true;
            depthState.DepthBias = render::DepthBiasState{ShadowRasterDepthBias, ShadowRasterSlopeBias, 0.0f};

            if (i >= sliceCount) {
                // No caster for this padding slice; the clear above already filled it with far depth.
                ctx.CmdBuffer->EndRenderPass(std::move(encoder));
                continue;
            }

            const AdditionalShadowSlice& slice = shadow.Slices[i];
            MeshPassProcessor::Config processorConfig{};
            processorConfig.Cache = &ctx.Gpu->GetPSOCache();
            processorConfig.RtFormats.DepthFormat = render::TextureFormat::D32_FLOAT;
            processorConfig.ObjectConstantsParam = "gScene";
            processorConfig.Gpu = ctx.Gpu;
            processorConfig.PipelineOverride.DepthStencil = depthState;
            processorConfig.PipelineOverride.DisablePixelShader = true;
            processorConfig.PipelineOverride.KeyTag = "shadow";
            processorConfig.ObjectConstantsOverride = [&slice](ObjectConstants& constants, const PrimitiveSceneProxy&, const SceneView& view) {
                std::memcpy(constants.MVP, view.ViewProjMatrix.data(), sizeof(constants.MVP));
                constants.CameraPosition[0] = slice.LightDirectionForBias.x();
                constants.CameraPosition[1] = slice.LightDirectionForBias.y();
                constants.CameraPosition[2] = slice.LightDirectionForBias.z();
                constants.CameraPosition[3] = slice.DepthBias;
                constants.Debug[0] = 6;
                std::memcpy(&constants.Debug[1], &slice.NormalBias, sizeof(slice.NormalBias));
            };

            if (ctx.Visible != nullptr) {
                _sceneRenderer.DrawRenderers(
                    encoder.get(),
                    *ctx.Visible,
                    slice.View,
                    processorConfig,
                    OpaquePrimitiveFilter);
            }

            ctx.CmdBuffer->EndRenderPass(std::move(encoder));
        }

        ctx.Resources->Transition(AdditionalShadowMapDepthName, ctx.FlightIndex, render::TextureState::ShaderRead, *ctx.CmdBuffer);
    }

private:
    SceneRenderer _sceneRenderer;
};

class PreZPass : public RenderPass {
public:
    explicit PreZPass(render::TextureFormat depthFormat)
        : _depthFormat(depthFormat) {}

    std::string_view GetName() const noexcept override { return "GltfViewerPreZPass"; }

    void Execute(RenderContext& ctx) override {
        if (!IsCommonRenderContextValid(ctx)) {
            return;
        }

        render::TextureView* depthView = AcquireDepthView(ctx, _depthFormat);
        if (depthView == nullptr) {
            return;
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
        auto encoderOpt = ctx.CmdBuffer->BeginRenderPass(passDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to begin glTF viewer Pre-Z pass");
            return;
        }
        auto encoder = encoderOpt.Release();

        SetViewportAndScissor(encoder.get(), ctx);

        render::DepthStencilState depthState = render::DepthStencilState::Default();
        depthState.DepthCompare = render::CompareFunction::Less;
        depthState.DepthWriteEnable = true;

        MeshPassProcessor::Config processorConfig{};
        processorConfig.Cache = &ctx.Gpu->GetPSOCache();
        processorConfig.RtFormats.DepthFormat = _depthFormat;
        processorConfig.ObjectConstantsParam = "gScene";
        processorConfig.Gpu = ctx.Gpu;
        processorConfig.PipelineOverride.DepthStencil = depthState;
        processorConfig.PipelineOverride.DisablePixelShader = true;
        processorConfig.PipelineOverride.KeyTag = "prez";

        if (ctx.Visible != nullptr) {
            _sceneRenderer.DrawRenderers(encoder.get(), *ctx.Visible, ctx.View, processorConfig, OpaquePrimitiveFilter);
        }

        ctx.CmdBuffer->EndRenderPass(std::move(encoder));
    }

private:
    SceneRenderer _sceneRenderer;
    render::TextureFormat _depthFormat;
};

class BasePass : public RenderPass {
public:
    BasePass(
        render::TextureFormat colorFormat,
        render::TextureFormat depthFormat,
        render::ColorClearValue clearColor,
        const bool* showShadowCascadeOverlay)
        : _colorFormat(colorFormat),
          _depthFormat(depthFormat),
          _clearColor(clearColor),
          _showShadowCascadeOverlay(showShadowCascadeOverlay) {}

    std::string_view GetName() const noexcept override { return "GltfViewerBasePass"; }

    void Execute(RenderContext& ctx) override {
        if (!IsCommonRenderContextValid(ctx) || ctx.ColorTarget == nullptr) {
            return;
        }

        render::TextureView* depthView = AcquireDepthView(ctx, _depthFormat);
        if (depthView == nullptr) {
            return;
        }

        render::ColorAttachment colorAttachment{
            .Target = ctx.ColorTarget,
            .Load = render::LoadAction::Clear,
            .Store = render::StoreAction::Store,
            .ClearValue = _clearColor};
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
        auto encoderOpt = ctx.CmdBuffer->BeginRenderPass(passDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to begin glTF viewer base pass");
            return;
        }
        auto encoder = encoderOpt.Release();

        SetViewportAndScissor(encoder.get(), ctx);

        render::DepthStencilState depthState = render::DepthStencilState::Default();
        depthState.DepthCompare = render::CompareFunction::Equal;
        depthState.DepthWriteEnable = false;

        MeshPassProcessor::Config processorConfig{};
        processorConfig.Cache = &ctx.Gpu->GetPSOCache();
        processorConfig.RtFormats.ColorFormats = {_colorFormat};
        processorConfig.RtFormats.DepthFormat = _depthFormat;
        processorConfig.ObjectConstantsParam = "gScene";
        processorConfig.Gpu = ctx.Gpu;
        processorConfig.ViewDescriptorSet = ctx.ViewDescriptorSet;
        processorConfig.ViewDescriptorSetIndex = ctx.ViewDescriptorSetIndex;
        processorConfig.PipelineOverride.DepthStencil = depthState;
        // 不透明物体不能写 backbuffer alpha:base-color alpha 对不透明渲染无意义,
        // 若写入 BGRA8 backbuffer,Windows 合成器会把它当成窗口透明度,导致整模型透视。
        processorConfig.PipelineOverride.ColorWriteMask = render::ColorWrite::Color;
        processorConfig.PipelineOverride.KeyTag = "opaque";
        if (_showShadowCascadeOverlay != nullptr && *_showShadowCascadeOverlay) {
            processorConfig.ObjectConstantsOverride = [](ObjectConstants& constants, const PrimitiveSceneProxy&, const SceneView&) {
                constants.Debug[0] = 7;
            };
        }

        if (ctx.Visible != nullptr) {
            _sceneRenderer.DrawRenderers(encoder.get(), *ctx.Visible, ctx.View, processorConfig, OpaquePrimitiveFilter);
        }

        ctx.CmdBuffer->EndRenderPass(std::move(encoder));
    }

private:
    SceneRenderer _sceneRenderer;
    render::TextureFormat _colorFormat;
    render::TextureFormat _depthFormat;
    render::ColorClearValue _clearColor;
    const bool* _showShadowCascadeOverlay{nullptr};
};

class TransparentPass : public RenderPass {
public:
    TransparentPass(
        render::TextureFormat colorFormat,
        render::TextureFormat depthFormat)
        : _colorFormat(colorFormat),
          _depthFormat(depthFormat) {}

    std::string_view GetName() const noexcept override { return "GltfViewerTransparentPass"; }

    void Execute(RenderContext& ctx) override {
        if (!IsCommonRenderContextValid(ctx) || ctx.ColorTarget == nullptr) {
            return;
        }

        render::TextureView* depthView = AcquireDepthView(ctx, _depthFormat);
        if (depthView == nullptr) {
            return;
        }

        render::ColorAttachment colorAttachment{
            .Target = ctx.ColorTarget,
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
        auto encoderOpt = ctx.CmdBuffer->BeginRenderPass(passDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to begin glTF viewer transparent pass");
            return;
        }
        auto encoder = encoderOpt.Release();

        SetViewportAndScissor(encoder.get(), ctx);

        render::DepthStencilState depthState = render::DepthStencilState::Default();
        depthState.DepthCompare = render::CompareFunction::LessEqual;
        depthState.DepthWriteEnable = false;

        render::BlendState alphaOverBlend{
            .Color = {
                .Src = render::BlendFactor::SrcAlpha,
                .Dst = render::BlendFactor::OneMinusSrcAlpha,
                .Op = render::BlendOperation::Add},
            .Alpha = {
                .Src = render::BlendFactor::One,
                .Dst = render::BlendFactor::OneMinusSrcAlpha,
                .Op = render::BlendOperation::Add}};

        MeshPassProcessor::Config processorConfig{};
        processorConfig.Cache = &ctx.Gpu->GetPSOCache();
        processorConfig.RtFormats.ColorFormats = {_colorFormat};
        processorConfig.RtFormats.DepthFormat = _depthFormat;
        processorConfig.ObjectConstantsParam = "gScene";
        processorConfig.Gpu = ctx.Gpu;
        processorConfig.ViewDescriptorSet = ctx.ViewDescriptorSet;
        processorConfig.ViewDescriptorSetIndex = ctx.ViewDescriptorSetIndex;
        processorConfig.PipelineOverride.DepthStencil = depthState;
        processorConfig.PipelineOverride.OverrideBlend = true;
        processorConfig.PipelineOverride.Blend = alphaOverBlend;
        // 只混合 RGB,不写 backbuffer alpha:否则 BGRA8 backbuffer 的 alpha 会被
        // Windows 合成器当成窗口透明度,玻璃会把窗口戳透。
        processorConfig.PipelineOverride.ColorWriteMask = render::ColorWrite::Color;
        processorConfig.PipelineOverride.KeyTag = "transparent";

        if (ctx.Visible != nullptr) {
            // Transparent primitives are not depth-sorted in this run.
            _sceneRenderer.DrawRenderers(encoder.get(), *ctx.Visible, ctx.View, processorConfig, TransparentPrimitiveFilter);
        }

        ctx.CmdBuffer->EndRenderPass(std::move(encoder));
    }

private:
    SceneRenderer _sceneRenderer;
    render::TextureFormat _colorFormat;
    render::TextureFormat _depthFormat;
};

}  // namespace

class GltfViewerApp : public Application {
public:
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;
    static constexpr render::TextureFormat DepthFormat = render::TextureFormat::D32_FLOAT;

    void SetInitialLoadPath(std::filesystem::path path) {
        _initialLoadPath = std::move(path);
    }

    void OnInit() override {
        InitMaterial();
        InitPipeline();
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

        RenderContext rc{};
        rc.FlightIndex = ctx.FlightIndex();
        rc.CmdBuffer = ctx.GetCommandBuffer();
        rc.Device = GetDevice();
        rc.Gpu = GetGpuSystem();
        rc.Scene = GetWorld()->GetScene();
        rc.ColorTarget = target.BackBufferView;
        rc.ColorFormat = bbDesc.Format;
        rc.Width = bbDesc.Width;
        rc.Height = bbDesc.Height;
        if (_cameraComp != nullptr) {
            _cameraComp->FillSceneView(rc.View, bbDesc.Width, bbDesc.Height);
        }

        if (rc.Scene != nullptr) {
            SceneRenderer::Cull(*rc.Scene, rc.View, _visible);
            rc.Visible = &_visible;
        }
        rc.Resources = &_resourcePool;

        ComputeDirectionalCascades(rc.View, rc.Shadow);
        _lastShadow = rc.Shadow;
        render::TextureView* shadowSrv = AcquireShadowArrayResourceView(rc);

        if (rc.Scene != nullptr) {
            BuildAdditionalShadows(*rc.Scene, AdditionalShadowMapResolution, _shadowSoftMode, rc.AdditionalShadow);
        }
        _lastAdditionalShadowSlices = rc.AdditionalShadow.SliceCount;
        _lastAdditionalShadowLights = static_cast<uint32_t>(rc.AdditionalShadow.Lights.size());
        render::TextureView* additionalShadowSrv = AcquireAdditionalShadowArrayResourceView(rc);

        if (rc.Scene != nullptr && _viewerMaterial.IsReady()) {
            Material* material = _viewerMaterial.Get();
            if (material != nullptr && material->GetRootSignature() != nullptr) {
                rc.ViewDescriptorSetIndex = render::DescriptorSetIndex{0};
                rc.ViewDescriptorSet = _lightBuffer.Update(
                    *GetDevice(),
                    material->GetRootSignature(),
                    rc.ViewDescriptorSetIndex,
                    rc.FlightIndex,
                    *rc.Scene,
                    rc.Shadow,
                    shadowSrv,
                    rc.AdditionalShadow,
                    additionalShadowSrv);
            }
        }

        _pipeline.Render(rc);

        RenderShadowPreviewAtlas(rc, shadowSrv);
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
        _viewerMaterial.Reset();
    }

private:
    void InitMaterial() {
        std::filesystem::path shaderPath = std::filesystem::path{RADRAY_GLTF_VIEWER_ASSET_DIR} / "gltf_viewer.hlsl";
        MaterialDescriptor matDesc{};
        matDesc.ShaderPath = shaderPath;
        matDesc.ShaderName = "gltf_viewer";
        matDesc.VsEntry = "VSMain";
        matDesc.PsEntry = "PSMain";
        matDesc.Primitive = render::PrimitiveState::Default();
        matDesc.Primitive.Cull = render::CullMode::Back;
        matDesc.DepthStencil = render::DepthStencilState::Default();
        matDesc.DepthStencil.Format = DepthFormat;
        const AssetId matId = Guid::Parse("91f5f8e0-7cb3-41c5-8b19-a62253f19f2a");
        _viewerMaterial = GetAssetManager()->Load<Material>(AssetLoadRequest{
            .Id = matId,
            .Task = LoadMaterial(*GetGpuSystem(), matDesc),
            .DebugName = "glTF viewer material"});
        if (!_viewerMaterial.IsValid()) {
            RADRAY_ERR_LOG("failed to start loading glTF viewer material");
        }
    }

    void InitPipeline() {
        const render::ColorClearValue clearColor{{0.06f, 0.07f, 0.08f, 1.0f}};
        _pipeline.AddPass(make_unique<ShadowCasterPass>());
        _pipeline.AddPass(make_unique<AdditionalShadowCasterPass>());
        _pipeline.AddPass(make_unique<PreZPass>(DepthFormat));
        _pipeline.AddPass(make_unique<BasePass>(BackBufferFormat, DepthFormat, clearColor, &_showShadowCascadeOverlay));
        // glTF 透明部分(alphaMode=BLEND)按 primitive 切分,在不透明 Base 之后做 alpha 混合。
        _pipeline.AddPass(make_unique<TransparentPass>(BackBufferFormat, DepthFormat));
    }

    bool EnsureShadowPreviewPipeline(RenderContext& ctx) {
        if (_shadowPreviewPso != nullptr && _shadowPreviewRootSig != nullptr) {
            return true;
        }
        if (ctx.Device == nullptr || ctx.Gpu == nullptr) {
            return false;
        }

        ShaderCompileDescriptor shaderDesc{};
        shaderDesc.Name = "gltf_viewer_shadow_preview";
        shaderDesc.Source = ShadowPreviewShaderSource;
        shaderDesc.EntryPoint = "CSMain";
        shaderDesc.Stage = render::ShaderStage::Compute;
        _shadowPreviewShader = ctx.Gpu->GetOrCompileShader(shaderDesc).Get();
        if (_shadowPreviewShader == nullptr) {
            RADRAY_ERR_LOG("failed to compile shadow preview shader");
            return false;
        }

        render::Shader* shaders[] = {_shadowPreviewShader};
        _shadowPreviewRootSig = ctx.Gpu->GetOrCreateRootSignature(std::span<render::Shader*>{shaders}).Get();
        if (_shadowPreviewRootSig == nullptr) {
            RADRAY_ERR_LOG("failed to create shadow preview root signature");
            return false;
        }

        render::ComputePipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = _shadowPreviewRootSig;
        psoDesc.CS = render::ShaderEntry{
            .Target = _shadowPreviewShader,
            .EntryPoint = "CSMain"};
        auto psoOpt = ctx.Device->CreateComputePipelineState(psoDesc);
        if (!psoOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to create shadow preview compute pipeline");
            return false;
        }
        _shadowPreviewPso = psoOpt.Release();
        _shadowPreviewPso->SetDebugName("Shadow preview atlas PSO");
        return true;
    }

    render::DescriptorSet* GetShadowPreviewDescriptorSet(RenderContext& ctx, render::TextureView* shadowSrv, render::TextureView* atlasUav) {
        if (_shadowPreviewRootSig == nullptr || ctx.Device == nullptr || shadowSrv == nullptr || atlasUav == nullptr) {
            return nullptr;
        }
        if (_shadowPreviewSets.size() <= ctx.FlightIndex) {
            _shadowPreviewSets.resize(static_cast<size_t>(ctx.FlightIndex) + 1);
        }

        unique_ptr<render::DescriptorSet>& set = _shadowPreviewSets[ctx.FlightIndex];
        if (set == nullptr) {
            auto setOpt = ctx.Device->CreateDescriptorSet(_shadowPreviewRootSig, render::DescriptorSetIndex{0});
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

    void RenderShadowPreviewAtlas(RenderContext& ctx, render::TextureView* shadowSrv) {
        _shadowPreviewReady = false;
        if (!_showShadowMapPreview || !ctx.Shadow.Enabled || shadowSrv == nullptr || ctx.CmdBuffer == nullptr || ctx.Resources == nullptr) {
            return;
        }
        if (!EnsureShadowPreviewPipeline(ctx)) {
            return;
        }

        render::TextureView* atlasUav = AcquireShadowPreviewAtlasUnorderedAccessView(ctx);
        render::TextureView* atlasSrv = AcquireShadowPreviewAtlasResourceView(ctx);
        render::DescriptorSet* set = GetShadowPreviewDescriptorSet(ctx, shadowSrv, atlasUav);
        if (atlasUav == nullptr || atlasSrv == nullptr || set == nullptr) {
            return;
        }

        ctx.Resources->Transition(ShadowPreviewAtlasName, ctx.FlightIndex, render::TextureState::UnorderedAccess, *ctx.CmdBuffer);
        auto encoderOpt = ctx.CmdBuffer->BeginComputePass();
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
        ctx.CmdBuffer->EndComputePass(std::move(encoder));
        ctx.Resources->Transition(ShadowPreviewAtlasName, ctx.FlightIndex, render::TextureState::ShaderRead, *ctx.CmdBuffer);

        if (ImGuiSystem* imgui = GetSubsystem<ImGuiSystem>()) {
            ImTextureID updated = imgui->CreateOrUpdateExternalTexture(_shadowPreviewTexture, ctx.FlightIndex, atlasSrv);
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
        directional->SetLightType(LightType::Directional);
        directional->SetDirection(Eigen::Vector3f{-0.3f, -1.0f, -0.3f}.normalized());
        directional->SetColor(Eigen::Vector3f{0.8f, 0.6f, 1.0f});
        directional->SetIntensity(15.0f);
        directional->SetCastShadow(true);
        directional->SetShadowBias(1.0f, 0.75f);

        Actor* spotActor = GetWorld()->SpawnActor<Actor>();
        auto* spot = spotActor->AddComponent<LightComponent>();
        _spotLight = spot;
        spotActor->SetRootComponent(spot);
        spot->SetLightType(LightType::Spot);
        spot->SetWorldLocation(Eigen::Vector3f{2.5f, 3.0f, 1.5f});
        spot->SetDirection(Eigen::Vector3f{-0.6f, -1.0f, -0.35f}.normalized());
        spot->SetColor(Eigen::Vector3f{1.0f, 0.95f, 0.85f});
        spot->SetIntensity(120.0f);
        spot->SetRange(20.0f);
        spot->SetSpotAngles(Radian(22.0f), Radian(32.0f));
        spot->SetCastShadow(true);
        spot->SetShadowBias(1.0f, 1.0f);

        Actor* pointActor = GetWorld()->SpawnActor<Actor>();
        auto* point = pointActor->AddComponent<LightComponent>();
        _pointLight = point;
        pointActor->SetRootComponent(point);
        point->SetLightType(LightType::Point);
        point->SetWorldLocation(Eigen::Vector3f{-2.0f, 2.0f, -2.0f});
        point->SetColor(Eigen::Vector3f{0.7f, 0.85f, 1.0f});
        point->SetIntensity(60.0f);
        point->SetRange(15.0f);
        point->SetCastShadow(true);
        point->SetShadowBias(1.5f, 0.0f);
    }

    bool ComputeDirectionalCascades(const SceneView& camView, ShadowCascadeData& out) {
        out = ShadowCascadeData{};
        _lastShadowSplits.fill(0.0f);
        const Scene* scene = GetWorld() != nullptr ? GetWorld()->GetScene() : nullptr;
        if (scene == nullptr || _cameraComp == nullptr) {
            return false;
        }

        Eigen::Vector3f lightDir{0.0f, -1.0f, 0.0f};
        float depthBiasTexels = 0.0f;
        float normalBiasTexels = 0.0f;
        bool found = false;
        for (const unique_ptr<LightSceneProxy>& light : scene->GetLights()) {
            if (light == nullptr || light->GetLightType() != LightType::Directional || !light->GetCastShadow()) {
                continue;
            }
            lightDir = light->GetDirection();
            depthBiasTexels = light->GetShadowDepthBias();
            normalBiasTexels = light->GetShadowNormalBias();
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

            ShadowCascade& cascade = out.Cascades[cascadeIndex];
            cascade.View = SceneView{};
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
        options.DefaultMaterial = _viewerMaterial.AsAny();
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
            _rootActor = _gltfAsset->ExportToScene(*GetWorld());
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
            _shadowSoftMode = static_cast<ShadowSoftMode>(softModeIndex);
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
                const ShadowCascade& cascade = _lastShadow.Cascades[i];
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

    RenderPipeline _pipeline;
    RenderResourcePool _resourcePool;
    SceneLightBuffer _lightBuffer;
    VisiblePrimitiveList _visible;
    ShadowCascadeData _lastShadow{};
    std::array<float, MaxShadowCascades + 1> _lastShadowSplits{};
    uint32_t _lastAdditionalShadowSlices{0};
    uint32_t _lastAdditionalShadowLights{0};
    float _shadowMaxDistance{DefaultShadowMaxDistance};
    float _shadowSplitLambda{DefaultShadowSplitLambda};
    ShadowSoftMode _shadowSoftMode{ShadowSoftMode::Medium};
    bool _showShadowCascadeOverlay{false};
    bool _showShadowMapPreview{true};
    bool _shadowPreviewReady{false};
    ImTextureID _shadowPreviewTexture{ImTextureID_Invalid};
    render::Shader* _shadowPreviewShader{nullptr};
    render::RootSignature* _shadowPreviewRootSig{nullptr};
    unique_ptr<render::ComputePipelineState> _shadowPreviewPso;
    vector<unique_ptr<render::DescriptorSet>> _shadowPreviewSets;

    StreamingAssetRef<GltfAsset> _gltfAsset;
    Actor* _rootActor{nullptr};
    StreamingAssetRef<Material> _viewerMaterial;

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
