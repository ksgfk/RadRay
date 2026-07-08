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
class LightSceneProxy;
class MeshPassExecutor;

namespace render {
class SamplerCache;
}  // namespace render

/// 最小前向渲染管线: 单 forward pass, 点光源 + Principled BRDF, 支持一盏点光源的立方体阴影。
///
/// 对应 Unity URP 的 UniversalRenderPipeline 精简版。每相机流程:
///   1. 从 World 收集相机 (OnBuildCameraList)。
///   2. 选取第一盏投影阴影的点光源, 生成 6 面 world->clip 矩阵, 内联 shadow caster pass:
///      逐面渲染场景 (ShadowCaster tag 的 DrawList) 到一张 cube 深度图的对应 slice。
///   3. 收集 Scene 的全部 primitive proxy 到 DrawList (无视锥裁剪)。
///   4. 收集点光源为 PointLightGpu 数组, 内联进 per-view cbuffer; 阴影光源附带 PointShadowData。
///   5. 单个 forward pass: clear color + depth, 深度测试写, MeshPassExecutor 提交,
///      forward.hlsl 用 cube 深度图 + comparison sampler 采样点光源阴影。
///
/// per-flight 深度纹理与 cube 阴影图内部持有, 尺寸变化时重建。
class ForwardPipeline final : public RenderPipeline {
public:
    static constexpr uint32_t kMaxPointLights = 64;  // 与 shaderlib/light.hlsl RR_MAX_POINT_LIGHTS 对齐
    static constexpr render::TextureFormat kDepthFormat = render::TextureFormat::D32_FLOAT;
    static constexpr render::TextureFormat kShadowFormat = render::TextureFormat::D32_FLOAT;
    static constexpr uint32_t kShadowCubeSize = 1024;  // cube 每面边长 (像素)
    static constexpr uint32_t kCubeFaceCount = 6;

    /// 一个点光源的 GPU 布局 (匹配 shaderlib/light.hlsl PointLightGpu)。
    struct PointLightGpu {
        float Position[4];   // xyz 世界位置, w = range
        float Intensity[4];  // rgb 辐亮度, w 保留
    };
    static_assert(sizeof(PointLightGpu) == 32);

    /// 点光源立方体阴影数据 (匹配 shaderlib/point_shadow.hlsl PointShadowData, 列主序)。
    struct PointShadowGpu {
        float ViewProj[kCubeFaceCount][16];  // 6 面 世界->裁剪, 面序 +X,-X,+Y,-Y,+Z,-Z
        float LightPosInvRadius[4];          // xyz 光源世界位置, w = 1/radius
        float Params[4];                     // x depthBias, y normalBias, z invResolution, w enable
    };
    static_assert(sizeof(PointShadowGpu) == 6 * 64 + 16 + 16);

    /// per-view 常量 (匹配 forward.hlsl 的 ViewConstants, cbuffer, 列主序)。
    /// 灯光内联为定长数组 (arena 缓冲不支持 StructuredBuffer)。
    struct ViewConstants {
        float ViewProj[16];
        float CameraPosition[4];
        uint32_t LightCounts[4];  // x = point light count, y = shadow point-light index+1 (0 = 无阴影)
        PointLightGpu PointLights[kMaxPointLights];
        PointShadowGpu PointShadow;
    };

    /// shadow caster 深度 pass 的 per-view 常量 (匹配 point_shadow_depth.hlsl ShadowViewConstants)。
    struct ShadowViewConstants {
        float ViewProj[16];
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

    struct ShadowCube {
        unique_ptr<render::Texture> Texture;
        unique_ptr<render::TextureView> Srv;                        // cube SRV (采样用)
        unique_ptr<render::TextureView> FaceDsv[kCubeFaceCount];    // 每面一个 DSV (2DArray slice)
        uint32_t Size{0};
        render::TextureStates State{render::TextureState::Undefined};
    };

    DepthTarget* AcquireDepthTarget(uint32_t flight, uint32_t width, uint32_t height);
    ShadowCube* AcquireShadowCube(uint32_t flight, uint32_t size);

    /// 渲染指定投影点光源的立方体阴影到 cube 各面 (6 次 render pass)。
    /// 填充 outShadow (供 forward pass 采样), 并把 cube 转到可采样布局。
    /// 返回 true 表示阴影可用。
    bool RenderPointShadow(
        RenderPipelineContext& ctx,
        Scene* scene,
        const LightSceneProxy& light,
        uint32_t flight,
        PointShadowGpu& outShadow,
        ShadowCube*& outCube);

    render::Device* _device{nullptr};
    render::SamplerCache* _samplerCache{nullptr};
    unique_ptr<MeshPassExecutor> _executor;         // forward pass
    unique_ptr<MeshPassExecutor> _shadowExecutor;   // shadow caster pass (独立 arena/tables)
    vector<DepthTarget> _depthTargets;              // per-flight
    vector<ShadowCube> _shadowCubes;                // per-flight
};

}  // namespace radray
