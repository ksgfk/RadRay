#pragma once

#include <limits>

#include <radray/runtime/render_framework/light_scene_proxy.h>

namespace radray {

class DirectionalLightComponent;

/// 方向光的渲染代理 (对应 UE5 的 FDirectionalLightSceneProxy 精简版)。
///
/// 方向光无位置、半径无穷 (照亮整个场景), 光照方向取自组件世界旋转的 +Z。
/// 额外携带级联阴影 (CSM) 配置, 供 ForwardPipeline 在阴影 pass 里计算级联划分 /
/// 正交投影矩阵。参数语义与 UE5 的 UDirectionalLightComponent 对齐。
class DirectionalLightSceneProxy final : public LightSceneProxy {
public:
    explicit DirectionalLightSceneProxy(const DirectionalLightComponent& component);
    ~DirectionalLightSceneProxy() noexcept override;

    // 方向光: 半径无穷, 非局部光。
    float GetRadius() const noexcept override { return std::numeric_limits<float>::max(); }
    bool IsLocalLight() const noexcept override { return false; }

    // ── CSM 配置 ──
    uint32_t GetCascadeCount() const noexcept { return _cascadeCount; }
    float GetShadowDistance() const noexcept { return _shadowDistance; }
    /// 级联划分的 对数/均匀 混合系数 [0,1]: 0 = 纯均匀, 1 = 纯对数。
    float GetCascadeSplitLambda() const noexcept { return _cascadeSplitLambda; }
    uint32_t GetShadowMapResolution() const noexcept { return _shadowMapResolution; }
    float GetShadowDepthBias() const noexcept { return _shadowDepthBias; }
    float GetShadowNormalBias() const noexcept { return _shadowNormalBias; }
    /// PCF 模式: 0 = 单 tap, 1 = 4-tap, 2 = 5x5 tent (与 shadow.hlsl softMode 对齐)。
    uint32_t GetShadowSoftMode() const noexcept { return _shadowSoftMode; }

private:
    uint32_t _cascadeCount{4};
    float _shadowDistance{100.0f};
    float _cascadeSplitLambda{0.75f};
    uint32_t _shadowMapResolution{2048};
    float _shadowDepthBias{0.002f};
    float _shadowNormalBias{1.0f};
    uint32_t _shadowSoftMode{2};
};

}  // namespace radray
