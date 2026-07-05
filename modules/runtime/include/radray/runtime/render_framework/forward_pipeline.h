#pragma once

#include <cstdint>

#include <radray/basic_math.h>
#include <radray/render/common.h>
#include <radray/render/gpu_resource.h>
#include <radray/runtime/render_framework/mesh_pass_executor.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/types.h>

namespace radray {

class RenderSystem;
class Scene;
class CameraComponent;
class MeshPassExecutor;

/// 最小前向渲染管线: 单 forward pass, 点光源 + Principled BRDF, 无阴影/半透明。
///
/// 对应 Unity URP 的 UniversalRenderPipeline 精简到只剩不透明前向。每相机流程:
///   1. 从 World 收集相机 (OnBuildCameraList)。
///   2. 收集 Scene 的全部 primitive proxy 到 DrawList (无视锥裁剪)。
///   3. 收集点光源为 PointLightGpu 数组, 内联进 per-view cbuffer。
///   4. 单个 render pass: clear color + depth, 深度测试写, MeshPassExecutor 提交。
///
/// per-flight 深度纹理内部持有, 尺寸变化时重建。
class ForwardPipeline final : public RenderPipeline {
public:
    static constexpr uint32_t kMaxPointLights = 64;  // 与 shaderlib/light.hlsl RR_MAX_POINT_LIGHTS 对齐
    static constexpr render::TextureFormat kDepthFormat = render::TextureFormat::D32_FLOAT;

    /// 一个点光源的 GPU 布局 (匹配 shaderlib/light.hlsl PointLightGpu)。
    struct PointLightGpu {
        float Position[4];   // xyz 世界位置, w = range
        float Intensity[4];  // rgb 辐亮度, w 保留 (-1 = 无阴影)
    };
    static_assert(sizeof(PointLightGpu) == 32);

    /// per-view 常量 (匹配 forward.hlsl 的 ViewConstants, cbuffer, 列主序)。
    /// 灯光内联为定长数组 (arena 缓冲不支持 StructuredBuffer)。
    struct ViewConstants {
        float ViewProj[16];
        float CameraPosition[4];
        uint32_t LightCounts[4];
        PointLightGpu PointLights[kMaxPointLights];
    };

    explicit ForwardPipeline(RenderSystem* renderSystem) noexcept;
    ~ForwardPipeline() noexcept override;

protected:
    void OnBuildCameraList(RenderPipelineContext& ctx, RenderCameraList& cameras) override;
    void OnRenderCamera(RenderPipelineContext& ctx, const RenderCamera& camera) override;

private:
    struct DepthTarget {
        unique_ptr<render::Texture> Texture;
        unique_ptr<render::TextureView> View;
        uint32_t Width{0};
        uint32_t Height{0};
        render::TextureStates State{render::TextureState::Undefined};
    };

    DepthTarget* AcquireDepthTarget(uint32_t flight, uint32_t width, uint32_t height);

    render::Device* _device{nullptr};
    unique_ptr<MeshPassExecutor> _executor;
    vector<DepthTarget> _depthTargets;  // per-flight
};

}  // namespace radray
