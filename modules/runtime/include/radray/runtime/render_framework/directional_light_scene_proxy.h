#pragma once

#include <array>
#include <limits>

#include <radray/runtime/render_framework/light_scene_proxy.h>

namespace radray {

class DirectionalLightComponent;
enum class CascadeSplitMode : uint32_t;

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
    /// 级联划分模式: 自动 (混合系数) 或 手动 (划分比例)。
    CascadeSplitMode GetCascadeSplitMode() const noexcept { return _cascadeSplitMode; }
    /// 级联划分的 对数/均匀 混合系数 [0,1]: 0 = 纯均匀, 1 = 纯对数 (仅自动模式生效)。
    float GetCascadeSplitLambda() const noexcept { return _cascadeSplitLambda; }
    /// 手动级联划分比例 [0,1] 累积归一化边界 (仅手动模式生效, 对齐 Unity URP)。
    const std::array<float, 3>& GetCascadeSplitRatios() const noexcept { return _cascadeSplitRatios; }
    uint32_t GetShadowMapResolution() const noexcept { return _shadowMapResolution; }
    /// 阴影 bias 倍率 (无量纲, URP 风格): 逐级联乘以 texel 世界尺寸得到实际偏移。
    float GetShadowDepthBias() const noexcept { return _shadowDepthBias; }
    float GetShadowNormalBias() const noexcept { return _shadowNormalBias; }
    /// PCF 模式: 0 = 单 tap, 1 = 4-tap, 2 = 5x5 tent (与 shadow.hlsl softMode 对齐)。
    uint32_t GetShadowSoftMode() const noexcept { return _shadowSoftMode; }

private:
    uint32_t _cascadeCount{4};
    float _shadowDistance{100.0f};
    CascadeSplitMode _cascadeSplitMode;
    float _cascadeSplitLambda{0.75f};
    std::array<float, 3> _cascadeSplitRatios{0.067f, 0.2f, 0.467f};
    uint32_t _shadowMapResolution{2048};
    float _shadowDepthBias{1.0f};
    float _shadowNormalBias{1.0f};
    uint32_t _shadowSoftMode{2};
};

}  // namespace radray
